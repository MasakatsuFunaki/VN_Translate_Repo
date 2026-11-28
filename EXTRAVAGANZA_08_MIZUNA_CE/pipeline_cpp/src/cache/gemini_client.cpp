// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Thin adapter bridging exm::gemini::Client onto shared vn::llm_client.
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

constexpr std::int64_t kOmitThinkingBudget = -1;

void sync_process_api_key(const std::string& key) {
    if (key.empty()) {
        return;
    }
    _putenv_s("GEMINI_API_KEY", key.c_str());
    ::SetEnvironmentVariableA("GEMINI_API_KEY", key.c_str());
}

std::optional<std::string> wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return std::nullopt;
    }
    const std::wstring_view wide(val);
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return std::nullopt;
    }
    return utf8;
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

vn::llm::LlmRequest unpack_llm_request(const std::string& model,
                                       const bj::value& body) {
    vn::llm::ensure_supported_model(model);
    vn::llm::LlmRequest req;
    req.model = model;
    req.thinking_level.clear();
    req.thinking_budget = kOmitThinkingBudget;

    if (!body.is_object()) {
        return req;
    }
    const auto& root = body.as_object();

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

    if (const auto* gen = root.if_contains("generationConfig");
        gen != nullptr && gen->is_object()) {
        const auto& go = gen->as_object();
        if (const auto* max_out = go.if_contains("maxOutputTokens")) {
            if (max_out->is_int64()) {
                req.max_output_tokens = max_out->as_int64();
            } else if (max_out->is_uint64()) {
                req.max_output_tokens = static_cast<std::int64_t>(max_out->as_uint64());
            }
        }
        if (const auto* tc = go.if_contains("thinkingConfig");
            tc != nullptr && tc->is_object()) {
            const auto& tco = tc->as_object();
            if (const auto* tl = tco.if_contains("thinkingLevel");
                tl != nullptr && tl->is_string()) {
                req.thinking_level = std::string(tl->as_string());
            }
            if (const auto* tb = tco.if_contains("thinkingBudget");
                tb != nullptr && tb->is_int64()) {
                req.thinking_budget = tb->as_int64();
            }
        }
    }
    return req;
}

bj::value pack_llm_response(const vn::llm::LlmResponse& resp) {
    bj::object root;

    bj::object usage;
    usage["promptTokenCount"] = resp.usage.prompt_tokens;
    usage["candidatesTokenCount"] = resp.usage.candidate_tokens;
    usage["thoughtsTokenCount"] = resp.usage.thought_tokens;
    usage["cachedContentTokenCount"] = resp.usage.cached_tokens;
    usage["totalTokenCount"] =
        resp.usage.prompt_tokens + resp.usage.total_output_tokens();
    root["usageMetadata"] = std::move(usage);

    if (resp.content_blocked && resp.text.empty()) {
        bj::object feedback;
        feedback["blockReason"] =
            resp.finish_reason.empty() ? std::string("OTHER") : resp.finish_reason;
        root["promptFeedback"] = std::move(feedback);
        return root;
    }

    bj::object part;
    part["text"] = resp.text;
    bj::object content;
    content["role"] = "model";
    content["parts"] = bj::array{std::move(part)};
    bj::object cand;
    cand["content"] = std::move(content);
    if (!resp.finish_reason.empty()) {
        cand["finishReason"] = resp.finish_reason;
    }
    root["candidates"] = bj::array{std::move(cand)};
    return root;
}

class HttpsClient final : public Client {
public:
    explicit HttpsClient(std::string api_key) {
        if (api_key.empty()) {
            api_key = vn::llm::resolve_gemini_api_key();
        }
        if (api_key.empty()) {
            throw std::runtime_error(
                "GEMINI_API_KEY is not set (checked environment, .claude/settings.json, "
                "~/.gemini/.env, and HKCU\\Environment)");
        }
        vn::llm::LlmClientOptions opts;
        opts.provider = vn::llm::LlmProvider::Gemini;
        opts.api_key = std::move(api_key);
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.log_fn = [](std::string_view msg) {
            log_info("    " + std::string(msg));
        };
        llm_ = vn::llm::create_llm_client(std::move(opts));
    }

    bj::value generate_content(const std::string& model,
                               const bj::value& body) override {
        const vn::llm::LlmRequest req = unpack_llm_request(model, body);
        try {
            return pack_llm_response(llm_->generate(req));
        } catch (const vn::llm::LlmRateLimitError& ex) {
            throw RateLimitError(ex.what());
        } catch (const vn::llm::LlmError& ex) {
            if (ex.http_status() != vn::llm::kHttpStatusUnset) {
                throw ApiError(ex.http_status(), ex.what());
            }
            throw;
        }
    }

private:
    std::unique_ptr<vn::llm::ILlmClient> llm_;
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
    const std::string resolved = vn::llm::resolve_gemini_api_key();
    if (!resolved.empty()) {
        sync_process_api_key(resolved);
        return;
    }

    std::vector<fs::path> settings_candidates = {
        fs::u8path(project_dir).parent_path() / ".claude" / "settings.json",
    };
    std::optional<std::string> home = wide_env_to_utf8(L"USERPROFILE");
    if (!home) {
        home = wide_env_to_utf8(L"HOME");
    }
    if (home) {
        settings_candidates.push_back(
            fs::u8path(*home) / ".claude_vn" / "settings.json");
    }

    for (const auto& p : settings_candidates) {
        if (!fs::exists(p)) {
            continue;
        }
        try {
            const bj::value v = json_parse_file(p.u8string());
            if (!v.is_object()) {
                continue;
            }
            const auto* env = v.get_object().if_contains("env");
            if (!env || !env->is_object()) {
                continue;
            }
            const auto* key = env->get_object().if_contains("GEMINI_API_KEY");
            if (key && key->is_string() && !key->get_string().empty()) {
                sync_process_api_key(std::string(key->get_string()));
                return;
            }
        } catch (const std::exception&) {
        }
    }
}

}  // namespace exm::gemini
