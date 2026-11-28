// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fav::gemini {

namespace bj = boost::json;

namespace {

constexpr std::int64_t kZeroTokens = 0;
constexpr int kHttpOkStatus = 200;
constexpr std::string_view kModelsPathPrefix = "/v1beta/models/";
constexpr std::string_view kGenerateActionSuffix = ":generateContent";

std::int64_t read_json_int64(const bj::object& obj, const char* key) noexcept {
    const auto* v = obj.if_contains(key);
    if (v == nullptr) {
        return kZeroTokens;
    }
    if (v->is_int64()) {
        return v->get_int64();
    }
    if (v->is_uint64()) {
        const std::uint64_t raw = v->get_uint64();
        constexpr auto kMaxInt64AsUint =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (raw > kMaxInt64AsUint) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(raw);
    }
    return kZeroTokens;
}

std::string extract_system_prompt(const bj::object& in) {
    const auto* sys = in.if_contains("system");
    if (sys == nullptr) {
        return {};
    }
    if (sys->is_string()) {
        return std::string(sys->get_string());
    }
    if (!sys->is_array()) {
        return {};
    }
    std::string sys_text;
    for (const auto& item : sys->get_array()) {
        if (!item.is_object()) {
            continue;
        }
        const auto* t = item.get_object().if_contains("text");
        if (t == nullptr || !t->is_string()) {
            continue;
        }
        if (!sys_text.empty()) {
            sys_text += "\n";
        }
        sys_text += std::string(t->get_string());
    }
    return sys_text;
}

std::string extract_user_prompt(const bj::object& in) {
    const auto* msgs = in.if_contains("messages");
    if (msgs == nullptr || !msgs->is_array()) {
        return {};
    }
    std::string user_text;
    for (const auto& mv : msgs->get_array()) {
        if (!mv.is_object()) {
            continue;
        }
        const auto* c = mv.get_object().if_contains("content");
        if (c == nullptr) {
            continue;
        }
        if (c->is_string()) {
            if (!user_text.empty()) {
                user_text += "\n";
            }
            user_text += std::string(c->get_string());
            continue;
        }
        if (!c->is_array()) {
            continue;
        }
        for (const auto& bv : c->get_array()) {
            if (!bv.is_object()) {
                continue;
            }
            const auto& block = bv.get_object();
            const auto* txt = block.if_contains("text");
            if (txt != nullptr && txt->is_string()) {
                if (!user_text.empty()) {
                    user_text += "\n";
                }
                user_text += std::string(txt->get_string());
            }
        }
    }
    return user_text;
}

std::int64_t extract_requested_max_tokens(const bj::object& in) noexcept {
    const auto* mt = in.if_contains("max_tokens");
    if (mt == nullptr) {
        return kMinOutputTokens;
    }
    if (mt->is_int64()) {
        return mt->get_int64();
    }
    if (mt->is_uint64()) {
        const std::uint64_t raw = mt->get_uint64();
        if (raw >= static_cast<std::uint64_t>(kMaxOutputTokens)) {
            return kMaxOutputTokens;
        }
        return static_cast<std::int64_t>(raw);
    }
    return kMinOutputTokens;
}

vn::llm::LlmRequest to_llm_request(const bj::value& body) {
    const bj::object empty_obj;
    const bj::object& in = body.is_object() ? body.get_object() : empty_obj;

    vn::llm::LlmRequest req;
    req.model = resolve_request_model(body);
    req.system_prompt = extract_system_prompt(in);
    req.user_prompt = extract_user_prompt(in);
    req.max_output_tokens = extract_requested_max_tokens(in);
    req.thinking_level = resolve_thinking_level(body);
    req.enforce_min_output_floor = true;
    req.safety_block_none = true;
    if (const auto* rzt = in.if_contains("retry_on_zero_thoughts");
        rzt != nullptr && rzt->is_bool()) {
        req.retry_on_zero_thoughts = rzt->get_bool();
    }
    return req;
}

std::string map_stop_reason(const vn::llm::LlmResponse& parsed) {
    if (parsed.content_blocked) {
        return "refusal";
    }
    if (parsed.finish_reason.empty() || parsed.finish_reason == "STOP") {
        return "end_turn";
    }
    if (parsed.finish_reason == "MAX_TOKENS") {
        return "max_tokens";
    }
    return parsed.finish_reason;
}

bj::object to_normalized_response_json(const vn::llm::LlmResponse& parsed) {
    bj::object out;
    out["content"] = bj::array{
        bj::object{{"type", "text"}, {"text", parsed.text}}
    };
    out["stop_reason"] = map_stop_reason(parsed);
    out["usage"] = bj::object{
        {"input_tokens", parsed.usage.prompt_tokens},
        {"output_tokens", parsed.usage.total_output_tokens()},
        {"thought_tokens", parsed.usage.thought_tokens},
        {"cache_read_input_tokens", parsed.usage.cached_tokens},
        {"cache_creation_input_tokens", kZeroTokens}
    };
    return out;
}

std::string parse_model_from_target_path(std::string_view target_path) {
    const std::size_t prefix_pos = target_path.find(kModelsPathPrefix);
    const std::size_t start =
        prefix_pos == std::string_view::npos
            ? 0u
            : prefix_pos + kModelsPathPrefix.size();
    const std::size_t end = target_path.find(kGenerateActionSuffix, start);
    if (end == std::string_view::npos || end <= start) {
        return std::string(vn::llm::kDefaultModel);
    }
    return std::string(target_path.substr(start, end - start));
}

class EndpointRawTransportAdapter final : public vn::llm::IRawHttpTransport {
public:
    explicit EndpointRawTransportAdapter(Endpoint* endpoint) noexcept
        : endpoint_(endpoint) {}

    vn::llm::RawHttpResponse post_json(std::string_view target_path,
                                       std::string_view json_body,
                                       int /*connect_timeout_sec*/,
                                       int read_timeout_sec) override {
        const std::string model = parse_model_from_target_path(target_path);
        const bj::object wire_req = bj::parse(json_body).as_object();
        const bj::value wire_res =
            endpoint_->generate_content(model, wire_req, read_timeout_sec);
        vn::llm::RawHttpResponse raw;
        raw.status_code = kHttpOkStatus;
        raw.body = bj::serialize(wire_res);
        return raw;
    }

private:
    Endpoint* endpoint_ = nullptr;
};

}  // namespace

Endpoint::~Endpoint() = default;

std::string resolve_request_model(const bj::value& body) {
    if (!body.is_object()) {
        return std::string(vn::llm::kDefaultModel);
    }
    const auto* m = body.get_object().if_contains("model");
    if (m != nullptr && m->is_string()) {
        const std::string_view raw_model(m->get_string().data(),
                                         m->get_string().size());
        try {
            vn::llm::ensure_supported_model(raw_model);
        } catch (const vn::llm::LlmError& e) {
            throw ApiError(e.what());
        }
        if (!raw_model.empty()) {
            return std::string(raw_model);
        }
    }
    return std::string(vn::llm::kDefaultModel);
}

std::string resolve_thinking_level(const bj::value& body) {
    if (!body.is_object()) {
        return std::string(vn::llm::kDefaultThinkingLevel);
    }
    const auto* tl = body.get_object().if_contains("thinking_level");
    if (tl != nullptr && tl->is_string() && !tl->get_string().empty()) {
        return std::string(tl->get_string());
    }
    return std::string(vn::llm::kDefaultThinkingLevel);
}

bj::object build_generate_content_request(const bj::value& body) {
    try {
        const vn::llm::LlmRequest req = to_llm_request(body);
        const std::string wire_str = vn::llm::build_gemini_request_json(req);
        bj::object wire_obj = bj::parse(wire_str).as_object();
        if (body.is_object()) {
            if (const auto* temp = body.get_object().if_contains("temperature")) {
                wire_obj["generationConfig"].as_object()["temperature"] = *temp;
            }
        }
        return wire_obj;
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

bj::object parse_generate_content_response(const bj::value& raw_response) {
    if (!raw_response.is_object()) {
        throw ApiError("Gemini response root must be a JSON object");
    }
    try {
        const std::string raw_str = bj::serialize(raw_response);
        const vn::llm::LlmResponse parsed =
            vn::llm::parse_gemini_response_json(raw_str, kHttpOkStatus);
        return to_normalized_response_json(parsed);
    } catch (const vn::llm::LlmRateLimitError& e) {
        throw RateLimitError(e.what());
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

Usage extract_usage(const bj::value& response) {
    Usage usage;
    if (!response.is_object()) {
        return usage;
    }
    const auto* u_val = response.get_object().if_contains("usage");
    if (u_val == nullptr || !u_val->is_object()) {
        return usage;
    }
    const auto& u = u_val->get_object();
    usage.input_tokens = read_json_int64(u, "input_tokens");
    usage.output_tokens = read_json_int64(u, "output_tokens");
    usage.thought_tokens = read_json_int64(u, "thought_tokens");
    usage.cache_read_input_tokens = read_json_int64(u, "cache_read_input_tokens");
    usage.cache_creation_input_tokens =
        read_json_int64(u, "cache_creation_input_tokens");
    return usage;
}

std::string load_api_key() {
    return vn::llm::resolve_gemini_api_key();
}

LazyClient::LazyClient(Endpoint* endpoint) noexcept : endpoint_(endpoint) {}

Client& LazyClient::get() {
    if (!client_) {
        if (endpoint_ != nullptr) {
            client_.emplace(endpoint_);
            return *client_;
        }
        const std::string key = load_api_key();
        if (key.empty()) {
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        }
        client_.emplace(key);
    }
    return *client_;
}

struct Client::Impl {
    std::string api_key;
    std::unique_ptr<EndpointRawTransportAdapter> endpoint_adapter;
    int configured_timeout_sec = vn::llm::kDefaultReadTimeoutSeconds;
    std::unique_ptr<vn::llm::ILlmClient> llm_client;

    explicit Impl(std::string key) : api_key(std::move(key)) {
        if (api_key.empty()) {
            api_key = load_api_key();
        }
        if (api_key.empty()) {
            throw ApiError("GEMINI_API_KEY not set");
        }
        rebuild_client(vn::llm::kDefaultReadTimeoutSeconds);
    }

    explicit Impl(Endpoint* endpoint) {
        if (endpoint == nullptr) {
            throw ApiError("Gemini Endpoint must not be null");
        }
        api_key = "offline-endpoint";
        endpoint_adapter = std::make_unique<EndpointRawTransportAdapter>(endpoint);
        rebuild_client(vn::llm::kDefaultReadTimeoutSeconds);
    }

    void rebuild_client(int timeout_sec) {
        vn::llm::LlmClientOptions opts;
        opts.provider = vn::llm::LlmProvider::Gemini;
        opts.api_key = api_key;
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.read_timeout_seconds = timeout_sec;
        if (endpoint_adapter != nullptr) {
            opts.max_retries = 0;
            opts.raw_transport = endpoint_adapter.get();
        } else {
            opts.log_fn = [](std::string_view msg) {
                log_info(std::string(msg));
            };
        }
        configured_timeout_sec = timeout_sec;
        llm_client = vn::llm::create_llm_client(std::move(opts));
    }
};

Client::Client(std::string api_key)
    : impl_(std::make_unique<Impl>(std::move(api_key))) {}

Client::Client(Endpoint* endpoint)
    : impl_(std::make_unique<Impl>(endpoint)) {}

Client::~Client() = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

bj::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        if (timeout_sec != impl_->configured_timeout_sec) {
            impl_->rebuild_client(timeout_sec);
        }
        const vn::llm::LlmRequest req = to_llm_request(body);
        const vn::llm::LlmResponse parsed = impl_->llm_client->generate(req);
        return to_normalized_response_json(parsed);
    } catch (const vn::llm::LlmRateLimitError& e) {
        throw RateLimitError(e.what());
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace fav::gemini
