// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Thin adapter delegating HTTPS transport, TLS root-store verification,
// retries, wire JSON codecs, and credential lookup to `vn::llm_client`.
#include "cache/gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"

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

constexpr int kHttpOkStatus = 200;

void set_wide_env_from_utf8(const wchar_t* name, std::string_view utf8_value) {
    if (utf8_value.empty()) {
        return;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_value.data(),
        static_cast<int>(utf8_value.size()), nullptr, 0);
    if (needed <= 0) {
        return;
    }
    std::wstring wide_value(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_value.data(),
        static_cast<int>(utf8_value.size()), wide_value.data(), needed);
    if (written == needed) {
        _wputenv_s(name, wide_value.c_str());
    }
}

std::optional<std::string> wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return std::nullopt;
    }
    const std::wstring_view wide(val);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return std::nullopt;
    }
    return out;
}

vn::llm::LlmRequest json_body_to_llm_request(const std::string& model,
                                             const bj::value& body) {
    vn::llm::LlmRequest req;
    req.model = model;
    if (!body.is_object()) {
        return req;
    }
    const bj::object& root = body.as_object();
    if (const auto* sys = root.if_contains("systemInstruction");
        sys != nullptr && sys->is_object()) {
        if (const auto* parts = sys->as_object().if_contains("parts");
            parts != nullptr && parts->is_array() && !parts->as_array().empty() &&
            parts->as_array()[0].is_object()) {
            if (const auto* txt = parts->as_array()[0].as_object().if_contains("text");
                txt != nullptr && txt->is_string()) {
                req.system_prompt = std::string(txt->as_string());
            }
        }
    }
    if (const auto* contents = root.if_contains("contents");
        contents != nullptr && contents->is_array() && !contents->as_array().empty() &&
        contents->as_array()[0].is_object()) {
        if (const auto* parts = contents->as_array()[0].as_object().if_contains("parts");
            parts != nullptr && parts->is_array() && !parts->as_array().empty() &&
            parts->as_array()[0].is_object()) {
            if (const auto* txt = parts->as_array()[0].as_object().if_contains("text");
                txt != nullptr && txt->is_string()) {
                req.user_prompt = std::string(txt->as_string());
            }
        }
    }
    if (const auto* gen = root.if_contains("generationConfig");
        gen != nullptr && gen->is_object()) {
        const bj::object& g = gen->as_object();
        if (const auto* max_out = g.if_contains("maxOutputTokens");
            max_out != nullptr && max_out->is_int64()) {
            req.max_output_tokens = max_out->as_int64();
        }
        if (const auto* tc = g.if_contains("thinkingConfig");
            tc != nullptr && tc->is_object()) {
            if (const auto* lvl = tc->as_object().if_contains("thinkingLevel");
                lvl != nullptr && lvl->is_string()) {
                req.thinking_level = std::string(lvl->as_string());
            }
        }
    }
    return req;
}

bj::value llm_response_to_json_value(const vn::llm::LlmResponse& resp) {
    bj::object cand{
        {"content", bj::object{{"parts", bj::array{bj::object{{"text", resp.text}}}}}},
    };
    if (!resp.finish_reason.empty()) {
        cand["finishReason"] = resp.finish_reason;
    }
    bj::object usage{
        {"promptTokenCount", resp.usage.prompt_tokens},
        {"cachedContentTokenCount", resp.usage.cached_tokens},
        {"candidatesTokenCount", resp.usage.candidate_tokens},
        {"thoughtsTokenCount", resp.usage.thought_tokens},
    };
    return bj::object{
        {"candidates", bj::array{std::move(cand)}},
        {"usageMetadata", std::move(usage)},
    };
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
                "HKCU\\Environment, and .claude/settings.json)");
        }
        vn::llm::LlmClientOptions opts;
        opts.provider = vn::llm::LlmProvider::Gemini;
        opts.api_key = std::move(api_key);
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.log_fn = [](std::string_view msg) {
            log_info("    " + std::string(msg));
        };
        inner_ = vn::llm::create_llm_client(std::move(opts));
    }

    vn::llm::LlmResponse generate(const vn::llm::LlmRequest& request) override {
        vn::llm::ensure_supported_model(request.model);
        try {
            return inner_->generate(request);
        } catch (const vn::llm::LlmRateLimitError& ex) {
            throw RateLimitError(ex.what());
        } catch (const vn::llm::LlmError& ex) {
            if (ex.http_status() > 0) {
                throw ApiError(ex.http_status(), ex.what());
            }
            throw;
        }
    }

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        vn::llm::ensure_supported_model(model);
        return llm_response_to_json_value(
            generate(json_body_to_llm_request(model, body)));
    }

private:
    std::unique_ptr<vn::llm::ILlmClient> inner_;
};

}  // namespace

Client::~Client() = default;

vn::llm::LlmResponse Client::generate(const vn::llm::LlmRequest& request) {
    vn::llm::ensure_supported_model(request.model);
    const std::string wire_json = vn::llm::build_gemini_request_json(request);
    const std::string active_model =
        request.model.empty() ? std::string(vn::llm::kDefaultModel) : request.model;
    const bj::value raw_val = generate_content(active_model, bj::parse(wire_json));
    return vn::llm::parse_gemini_response_json(bj::serialize(raw_val), kHttpOkStatus);
}

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

    std::vector<fs::path> settings_candidates = {
        fs::u8path(project_dir).parent_path() / ".claude" / "settings.json",
    };
    std::optional<std::string> home = wide_env_to_utf8(L"USERPROFILE");
    if (!home.has_value()) {
        home = wide_env_to_utf8(L"HOME");
    }
    if (home.has_value()) {
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
