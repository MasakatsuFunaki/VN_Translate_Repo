// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Thin Ren_mushi_CE adapter around shared `vn::llm::ILlmClient` and
// `vn::llm::resolve_gemini_api_key`.
#include "cache/gemini_client.h"

#include "common/util.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace exm::gemini {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::int64_t extract_json_int64(const bj::value& v, std::int64_t fallback) noexcept {
    if (v.is_int64()) {
        return v.get_int64();
    }
    constexpr auto kMaxInt64 =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (v.is_uint64() && v.get_uint64() <= kMaxInt64) {
        return static_cast<std::int64_t>(v.get_uint64());
    }
    return fallback;
}

vn::llm::LlmRequest json_body_to_llm_request(const std::string& model,
                                             const bj::value& body) {
    vn::llm::ensure_supported_model(model);
    vn::llm::LlmRequest req;
    req.model = model;
    if (!body.is_object()) {
        return req;
    }

    const auto& root = body.get_object();
    if (const auto* sys = root.if_contains("systemInstruction");
        sys && sys->is_object()) {
        if (const auto* parts = sys->get_object().if_contains("parts");
            parts && parts->is_array() && !parts->get_array().empty()) {
            const auto& first = parts->get_array()[0];
            if (first.is_object()) {
                if (const auto* txt = first.get_object().if_contains("text");
                    txt && txt->is_string()) {
                    req.system_prompt = std::string(txt->get_string());
                }
            }
        }
    }

    if (const auto* contents = root.if_contains("contents");
        contents && contents->is_array() && !contents->get_array().empty()) {
        const auto& msg = contents->get_array()[0];
        if (msg.is_object()) {
            if (const auto* parts = msg.get_object().if_contains("parts");
                parts && parts->is_array() && !parts->get_array().empty()) {
                const auto& first = parts->get_array()[0];
                if (first.is_object()) {
                    if (const auto* txt = first.get_object().if_contains("text");
                        txt && txt->is_string()) {
                        req.user_prompt = std::string(txt->get_string());
                    }
                }
            }
        }
    }

    if (const auto* gen = root.if_contains("generationConfig");
        gen && gen->is_object()) {
        const auto& go = gen->get_object();
        if (const auto* max_out = go.if_contains("maxOutputTokens")) {
            req.max_output_tokens = extract_json_int64(*max_out, req.max_output_tokens);
        }
        if (const auto* tc = go.if_contains("thinkingConfig"); tc && tc->is_object()) {
            const auto& tco = tc->get_object();
            if (const auto* lvl = tco.if_contains("thinkingLevel");
                lvl && lvl->is_string()) {
                req.thinking_level = std::string(lvl->get_string());
            } else if (const auto* bud = tco.if_contains("thinkingBudget")) {
                req.thinking_level.clear();
                req.thinking_budget = extract_json_int64(*bud, req.thinking_budget);
            }
        }
    }

    if (const auto* safety = root.if_contains("safetySettings");
        safety && safety->is_array()) {
        req.safety_block_none = !safety->get_array().empty();
    }

    return req;
}

bj::value llm_response_to_json(const vn::llm::LlmResponse& resp) {
    bj::object usage{
        {"promptTokenCount", resp.usage.prompt_tokens},
        {"cachedContentTokenCount", resp.usage.cached_tokens},
        {"candidatesTokenCount", resp.usage.candidate_tokens},
        {"thoughtsTokenCount", resp.usage.thought_tokens},
    };

    if (resp.content_blocked && resp.text.empty() && resp.finish_reason.empty()) {
        return bj::object{
            {"promptFeedback", bj::object{{"blockReason", "SAFETY"}}},
            {"usageMetadata", std::move(usage)},
        };
    }

    const std::string finish =
        resp.finish_reason.empty() ? std::string("STOP") : resp.finish_reason;
    bj::object candidate{
        {"finishReason", finish},
        {"content", bj::object{{"parts", bj::array{bj::object{{"text", resp.text}}}}}},
    };
    return bj::object{
        {"candidates", bj::array{std::move(candidate)}},
        {"usageMetadata", std::move(usage)},
    };
}

std::string resolve_key_from_settings_fallback(const std::string& project_dir) {
    std::vector<fs::path> candidates;
    if (!project_dir.empty()) {
        candidates.push_back(
            fs::u8path(project_dir).parent_path() / ".claude" / "settings.json");
    }
    if (const char* home = std::getenv("USERPROFILE"); home && *home != '\0') {
        candidates.push_back(fs::u8path(home) / ".claude_vn" / "settings.json");
    } else if (const char* posix_home = std::getenv("HOME");
               posix_home && *posix_home != '\0') {
        candidates.push_back(fs::u8path(posix_home) / ".claude_vn" / "settings.json");
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
            if (!env || !env->is_object()) {
                continue;
            }
            const auto* key = env->get_object().if_contains("GEMINI_API_KEY");
            if (key && key->is_string() && !key->get_string().empty()) {
                return std::string(key->get_string());
            }
        } catch (const std::exception&) {
        }
    }
    return {};
}

class SharedLlmClientAdapter final : public Client {
public:
    explicit SharedLlmClientAdapter(std::string api_key) {
        if (api_key.empty()) {
            api_key = vn::llm::resolve_gemini_api_key();
        }
        if (api_key.empty()) {
            throw std::runtime_error(
                "GEMINI_API_KEY is not set (checked environment, ~/.gemini/.env, HKCU\\Environment, and .claude/settings.json)");
        }
        vn::llm::LlmClientOptions opts;
        opts.provider = vn::llm::LlmProvider::Gemini;
        opts.api_key = std::move(api_key);
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.log_fn = [](std::string_view msg) { log_info(std::string(msg)); };
        inner_ = vn::llm::create_llm_client(std::move(opts));
    }

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        const vn::llm::LlmRequest req = json_body_to_llm_request(model, body);
        try {
            const vn::llm::LlmResponse resp = inner_->generate(req);
            return llm_response_to_json(resp);
        } catch (const vn::llm::LlmRateLimitError& e) {
            throw RateLimitError(e.what());
        } catch (const vn::llm::LlmError& e) {
            if (e.http_status() != 0) {
                throw ApiError(e.http_status(), e.what());
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
    return std::make_unique<SharedLlmClientAdapter>(std::move(api_key));
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
    if (const char* existing = std::getenv("GEMINI_API_KEY");
        existing != nullptr && *existing != '\0') {
        return;
    }
    std::string resolved = vn::llm::resolve_gemini_api_key();
    if (resolved.empty()) {
        resolved = resolve_key_from_settings_fallback(project_dir);
    }
    if (!resolved.empty()) {
        _putenv_s("GEMINI_API_KEY", resolved.c_str());
    }
}

}  // namespace exm::gemini
