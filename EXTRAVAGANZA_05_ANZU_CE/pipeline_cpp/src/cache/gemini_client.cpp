// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Thin Object Adapter delegating Gemini HTTPS transport, TLS root store,
// retries, and credential resolution to shared `vn::llm_client`.
#include "cache/gemini_client.h"

#include "common/util.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exm::gemini {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kDisabledThinkingBudget = -1;

std::optional<std::string> wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = ::_wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return std::nullopt;
    }
    const std::wstring_view wide(val);
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return std::nullopt;
    }
    return out;
}

void set_wide_env_from_utf8(const wchar_t* name, std::string_view utf8_value) {
    if (utf8_value.empty()) {
        return;
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_value.data(),
        static_cast<int>(utf8_value.size()), nullptr, 0);
    if (needed <= 0) {
        return;
    }
    std::wstring wide_value(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_value.data(),
        static_cast<int>(utf8_value.size()), wide_value.data(), needed);
    if (written == needed) {
        ::_wputenv_s(name, wide_value.c_str());
    }
}

std::string extract_first_part_text(const bj::object& container) {
    const auto* parts = container.if_contains("parts");
    if (parts == nullptr || !parts->is_array() || parts->as_array().empty()) {
        return {};
    }
    const auto& p0 = parts->as_array()[0];
    if (!p0.is_object()) {
        return {};
    }
    const auto* txt = p0.as_object().if_contains("text");
    if (txt == nullptr || !txt->is_string()) {
        return {};
    }
    return std::string(txt->as_string());
}

vn::llm::LlmRequest json_body_to_llm_request(const std::string& model,
                                             const bj::value& body) {
    vn::llm::LlmRequest req;
    req.model = model;
    req.thinking_level = std::string(vn::llm::kDefaultThinkingLevel);
    req.thinking_budget = kDisabledThinkingBudget;

    if (!body.is_object()) {
        return req;
    }
    const bj::object& root = body.as_object();

    if (const auto* sys = root.if_contains("systemInstruction");
        sys != nullptr && sys->is_object()) {
        req.system_prompt = extract_first_part_text(sys->as_object());
    }

    if (const auto* contents = root.if_contains("contents");
        contents != nullptr && contents->is_array() && !contents->as_array().empty() &&
        contents->as_array()[0].is_object()) {
        req.user_prompt = extract_first_part_text(contents->as_array()[0].as_object());
    }

    req.safety_block_none = root.if_contains("safetySettings") != nullptr;

    if (const auto* gen_val = root.if_contains("generationConfig");
        gen_val != nullptr && gen_val->is_object()) {
        const bj::object& gen = gen_val->as_object();
        if (const auto* max_tok = gen.if_contains("maxOutputTokens");
            max_tok != nullptr && (max_tok->is_int64() || max_tok->is_uint64())) {
            req.max_output_tokens = max_tok->to_number<std::int64_t>();
        }
        req.thinking_level.clear();
        if (const auto* tc_val = gen.if_contains("thinkingConfig");
            tc_val != nullptr && tc_val->is_object()) {
            const bj::object& tc = tc_val->as_object();
            if (const auto* lvl = tc.if_contains("thinkingLevel");
                lvl != nullptr && lvl->is_string()) {
                req.thinking_level = std::string(lvl->as_string());
            } else if (const auto* bud = tc.if_contains("thinkingBudget");
                       bud != nullptr && (bud->is_int64() || bud->is_uint64())) {
                req.thinking_budget = bud->to_number<std::int64_t>();
            }
        }
        if (const auto* mime = gen.if_contains("responseMimeType");
            mime != nullptr && mime->is_string() &&
            mime->as_string() == "application/json") {
            req.json_mode = true;
        }
    }
    return req;
}

bj::value llm_response_to_json(const vn::llm::LlmResponse& resp) {
    bj::object out;

    bj::array parts;
    if (!resp.text.empty()) {
        parts.push_back(bj::object{{"text", resp.text}});
    }
    bj::object cand;
    cand["content"] = bj::object{
        {"role", "model"},
        {"parts", std::move(parts)},
    };
    cand["finishReason"] =
        resp.finish_reason.empty() ? std::string("STOP") : resp.finish_reason;
    out["candidates"] = bj::array{std::move(cand)};

    if (resp.content_blocked && !resp.finish_reason.empty()) {
        out["promptFeedback"] = bj::object{
            {"blockReason", resp.finish_reason},
        };
    }

    const std::int64_t total_tokens =
        resp.usage.prompt_tokens + resp.usage.total_output_tokens();
    out["usageMetadata"] = bj::object{
        {"promptTokenCount", resp.usage.prompt_tokens},
        {"candidatesTokenCount", resp.usage.candidate_tokens},
        {"thoughtsTokenCount", resp.usage.thought_tokens},
        {"cachedContentTokenCount", resp.usage.cached_tokens},
        {"totalTokenCount", total_tokens},
    };
    return out;
}

class HttpsClient final : public Client {
public:
    explicit HttpsClient(std::string api_key) {
        if (api_key.empty()) {
            api_key = vn::llm::resolve_gemini_api_key();
        }
        if (api_key.empty()) {
            throw std::runtime_error(
                "GEMINI_API_KEY is not set (checked environment, ~/.gemini/.env, "
                "HKCU\\Environment, and settings.json)");
        }
        vn::llm::LlmClientOptions opts;
        opts.api_key = std::move(api_key);
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.log_fn = [](std::string_view msg) {
            log_info("    " + std::string(msg));
        };
        inner_ = vn::llm::create_llm_client(std::move(opts));
    }

    bj::value generate_content(const std::string& model,
                               const bj::value& body) override {
        vn::llm::ensure_supported_model(model);
        const vn::llm::LlmRequest req = json_body_to_llm_request(model, body);
        try {
            return llm_response_to_json(inner_->generate(req));
        } catch (const vn::llm::LlmRateLimitError& ex) {
            throw RateLimitError(ex.what());
        } catch (const vn::llm::LlmError& ex) {
            if (ex.http_status() > 0) {
                throw ApiError(ex.http_status(), ex.what());
            }
            throw;
        }
    }

private:
    std::unique_ptr<vn::llm::ILlmClient> inner_;
};

}  // namespace

Client::~Client() = default;

std::unique_ptr<Client> make_https_client(std::string api_key) {
    return std::make_unique<HttpsClient>(std::move(api_key));
}

struct LazyClient::Impl {
    Factory factory;
    std::unique_ptr<Client> client;

    explicit Impl(Factory f) : factory(std::move(f)) {}
};

LazyClient::LazyClient(Factory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}

LazyClient::~LazyClient() = default;
LazyClient::LazyClient(LazyClient&&) noexcept = default;
LazyClient& LazyClient::operator=(LazyClient&&) noexcept = default;

Client& LazyClient::get() {
    if (!impl_->client) {
        impl_->client = impl_->factory();
    }
    return *impl_->client;
}

void load_api_key(const std::string& project_dir) {
    if (wide_env_to_utf8(L"GEMINI_API_KEY").has_value()) {
        return;
    }

    const std::string resolved = vn::llm::resolve_gemini_api_key();
    if (!resolved.empty()) {
        set_wide_env_from_utf8(L"GEMINI_API_KEY", resolved);
        return;
    }

    std::vector<fs::path> candidates = {
        fs::u8path(project_dir).parent_path() / ".claude" / "settings.json",
    };
    std::optional<std::string> home = wide_env_to_utf8(L"USERPROFILE");
    if (!home.has_value()) {
        home = wide_env_to_utf8(L"HOME");
    }
    if (home.has_value()) {
        candidates.push_back(fs::u8path(*home) / ".claude_vn" / "settings.json");
    }

    for (const auto& p : candidates) {
        if (!fs::exists(p)) {
            continue;
        }
        try {
            const bj::value v = json_parse_file(p.u8string());
            if (!v.is_object()) {
                continue;
            }
            const auto* env = v.get_object().if_contains("env");
            if (env == nullptr || !env->is_object()) {
                continue;
            }
            const auto* key = env->get_object().if_contains("GEMINI_API_KEY");
            if (key != nullptr && key->is_string() && !key->get_string().empty()) {
                set_wide_env_from_utf8(L"GEMINI_API_KEY",
                                       std::string_view(key->get_string()));
                return;
            }
        } catch (const std::exception&) {
        }
    }
}

}  // namespace exm::gemini
