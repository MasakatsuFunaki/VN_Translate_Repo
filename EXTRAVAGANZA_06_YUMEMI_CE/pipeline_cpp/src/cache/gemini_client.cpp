// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Thin adapter bridging `exm::gemini` onto `vn::llm_client`.
#include "cache/gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
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
constexpr std::size_t kFirstPartIndex = 0;

std::string extract_first_part_text(const bj::object& container,
                                    std::string_view field_name) {
    const auto* node = container.if_contains(field_name);
    if (node == nullptr) {
        return {};
    }
    const bj::object* msg_obj = nullptr;
    if (node->is_object()) {
        msg_obj = &node->as_object();
    } else if (node->is_array() && !node->as_array().empty() &&
               node->as_array()[kFirstPartIndex].is_object()) {
        msg_obj = &node->as_array()[kFirstPartIndex].as_object();
    }
    if (msg_obj == nullptr) {
        return {};
    }
    const auto* parts = msg_obj->if_contains("parts");
    if (parts == nullptr || !parts->is_array() || parts->as_array().empty()) {
        return {};
    }
    const auto& p0 = parts->as_array()[kFirstPartIndex];
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
    if (!body.is_object()) {
        return req;
    }
    const auto& root = body.as_object();
    req.system_prompt = extract_first_part_text(root, "systemInstruction");
    req.user_prompt = extract_first_part_text(root, "contents");

    if (const auto* safety = root.if_contains("safetySettings")) {
        req.safety_block_none = safety->is_array() && !safety->as_array().empty();
    }
    if (const auto* gen = root.if_contains("generationConfig");
        gen != nullptr && gen->is_object()) {
        const auto& go = gen->as_object();
        if (const auto* max_out = go.if_contains("maxOutputTokens")) {
            if (max_out->is_int64()) {
                req.max_output_tokens = max_out->as_int64();
            } else if (max_out->is_uint64()) {
                req.max_output_tokens =
                    static_cast<std::int64_t>(max_out->as_uint64());
            }
        }
        if (const auto* tc = go.if_contains("thinkingConfig");
            tc != nullptr && tc->is_object()) {
            const auto& tco = tc->as_object();
            if (const auto* tl = tco.if_contains("thinkingLevel");
                tl != nullptr && tl->is_string()) {
                req.thinking_level = std::string(tl->as_string());
            } else if (const auto* tb = tco.if_contains("thinkingBudget");
                       tb != nullptr && tb->is_int64()) {
                req.thinking_level.clear();
                req.thinking_budget = tb->as_int64();
            }
        }
    }
    return req;
}

bj::value llm_response_to_json(const vn::llm::LlmResponse& resp) {
    bj::object root;
    root["usageMetadata"] = bj::object{
        {"promptTokenCount", resp.usage.prompt_tokens},
        {"cachedContentTokenCount", resp.usage.cached_tokens},
        {"candidatesTokenCount", resp.usage.candidate_tokens},
        {"thoughtsTokenCount", resp.usage.thought_tokens},
    };
    if (resp.content_blocked && resp.text.empty()) {
        root["promptFeedback"] = bj::object{
            {"blockReason",
             resp.finish_reason.empty() ? std::string("SAFETY")
                                        : resp.finish_reason},
        };
        return root;
    }
    bj::object cand;
    cand["finishReason"] =
        resp.finish_reason.empty() ? std::string("STOP") : resp.finish_reason;
    cand["content"] = bj::object{
        {"parts", bj::array{bj::object{{"text", resp.text}}}},
    };
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
                "GEMINI_API_KEY is not set (checked environment, ~/.gemini/.env, "
                "and HKCU\\Environment)");
        }
        vn::llm::LlmClientOptions opts;
        opts.api_key = std::move(api_key);
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.log_fn = [](std::string_view msg) {
            log_info("    " + std::string(msg));
        };
        llm_ = vn::llm::create_llm_client(std::move(opts));
    }

    vn::llm::LlmResponse generate_llm(
        const vn::llm::LlmRequest& request) override {
        vn::llm::ensure_supported_model(request.model);
        try {
            return llm_->generate(request);
        } catch (const vn::llm::LlmRateLimitError& ex) {
            throw RateLimitError(ex.what());
        } catch (const vn::llm::LlmError& ex) {
            if (ex.http_status() > 0) {
                throw ApiError(ex.http_status(), ex.what());
            }
            throw;
        }
    }

    bj::value generate_content(const std::string& model,
                               const bj::value& body) override {
        vn::llm::ensure_supported_model(model);
        return llm_response_to_json(
            generate_llm(json_body_to_llm_request(model, body)));
    }

private:
    std::unique_ptr<vn::llm::ILlmClient> llm_;
};

std::string read_claude_settings_api_key(const std::string& project_dir) {
    std::vector<fs::path> settings_files;
    if (!project_dir.empty()) {
        settings_files.push_back(
            fs::u8path(project_dir).parent_path() / ".claude" / "settings.json");
    }
    if (const wchar_t* home = ::_wgetenv(L"USERPROFILE");
        home != nullptr && *home != L'\0') {
        settings_files.push_back(fs::path(home) / ".claude_vn" / "settings.json");
    }
    for (const auto& p : settings_files) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec) {
            continue;
        }
        try {
            const bj::value v = json_parse_file(p.u8string());
            if (!v.is_object()) {
                continue;
            }
            const auto* env = v.as_object().if_contains("env");
            if (env == nullptr || !env->is_object()) {
                continue;
            }
            const auto* key = env->as_object().if_contains("GEMINI_API_KEY");
            if (key != nullptr && key->is_string() && !key->as_string().empty()) {
                return std::string(key->as_string());
            }
        } catch (const std::exception&) {
        }
    }
    return {};
}

}  // namespace

Client::~Client() = default;

vn::llm::LlmResponse Client::generate_llm(const vn::llm::LlmRequest& request) {
    vn::llm::ensure_supported_model(request.model);
    const std::string raw_req = vn::llm::build_gemini_request_json(request);
    const bj::value req_body = bj::parse(raw_req);
    const std::string active_model =
        request.model.empty() ? std::string(vn::llm::kDefaultModel)
                              : request.model;
    const bj::value resp_body = generate_content(active_model, req_body);
    const std::string raw_resp = bj::serialize(resp_body);
    try {
        return vn::llm::parse_gemini_response_json(raw_resp, kHttpOkStatus);
    } catch (const vn::llm::LlmError&) {
        vn::llm::LlmResponse fallback;
        if (resp_body.is_object()) {
            bj::object placeholder = resp_body.as_object();
            placeholder["candidates"] = bj::array{
                bj::object{
                    {"content",
                     bj::object{
                         {"parts", bj::array{bj::object{{"text", "x"}}}},
                     }},
                },
            };
            try {
                fallback.usage = vn::llm::parse_gemini_response_json(
                                     bj::serialize(placeholder), kHttpOkStatus)
                                     .usage;
            } catch (const vn::llm::LlmError&) {
            }
        }
        return fallback;
    }
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
    if (const char* existing = std::getenv("GEMINI_API_KEY");
        existing != nullptr && *existing != '\0') {
        return;
    }
    std::string key = vn::llm::resolve_gemini_api_key();
    if (key.empty()) {
        key = read_claude_settings_api_key(project_dir);
    }
    if (!key.empty()) {
        _putenv_s("GEMINI_API_KEY", key.c_str());
    }
}

}  // namespace exm::gemini
