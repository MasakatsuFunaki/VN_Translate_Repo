// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin adapter delegating wire serialization, HTTPS/TLS transport, retries,
// credential resolution, and zero-thought circuit breaking to `vn::llm_client`.
#include "gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/zero_thought_guard.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace snt::gemini {

namespace bj = boost::json;

namespace {

constexpr std::string_view kGeminiModelPrefix = "gemini-";

std::string extract_system_prompt(const bj::object& in_obj) {
    const auto* sys_v = in_obj.if_contains("system");
    if (sys_v == nullptr) {
        return {};
    }
    if (sys_v->is_string()) {
        return std::string(sys_v->get_string());
    }
    if (!sys_v->is_array()) {
        return {};
    }
    std::string sys_text;
    for (const auto& item : sys_v->get_array()) {
        if (!item.is_object()) {
            continue;
        }
        const auto* txt = item.get_object().if_contains("text");
        if (txt == nullptr || !txt->is_string()) {
            continue;
        }
        if (!sys_text.empty()) {
            sys_text += '\n';
        }
        sys_text += std::string(txt->get_string());
    }
    return sys_text;
}

std::string extract_user_prompt(const bj::object& in_obj) {
    const auto* msgs_v = in_obj.if_contains("messages");
    if (msgs_v == nullptr || !msgs_v->is_array()) {
        return {};
    }
    std::string user_text;
    for (const auto& msg_v : msgs_v->get_array()) {
        if (!msg_v.is_object()) {
            continue;
        }
        const auto* c_v = msg_v.get_object().if_contains("content");
        if (c_v == nullptr) {
            continue;
        }
        if (c_v->is_string()) {
            if (!user_text.empty()) {
                user_text += '\n';
            }
            user_text += std::string(c_v->get_string());
            continue;
        }
        if (!c_v->is_array()) {
            continue;
        }
        for (const auto& block_v : c_v->get_array()) {
            if (!block_v.is_object()) {
                continue;
            }
            const bj::object& block = block_v.get_object();
            const auto* bt = block.if_contains("type");
            if (bt == nullptr || !bt->is_string() || bt->get_string() != "text") {
                continue;
            }
            const auto* txt = block.if_contains("text");
            if (txt == nullptr || !txt->is_string()) {
                continue;
            }
            if (!user_text.empty()) {
                user_text += '\n';
            }
            user_text += std::string(txt->get_string());
        }
    }
    return user_text;
}

vn::llm::LlmRequest to_llm_request(const bj::object& in_obj) {
    vn::llm::LlmRequest req;
    req.model = std::string(vn::llm::kDefaultModel);
    if (const auto* m = in_obj.if_contains("model"); m != nullptr && m->is_string()) {
        const std::string_view requested(m->get_string().data(), m->get_string().size());
        vn::llm::ensure_supported_model(requested);
        if (requested.substr(0, kGeminiModelPrefix.size()) == kGeminiModelPrefix) {
            req.model.assign(requested.data(), requested.size());
        }
    }

    req.system_prompt = extract_system_prompt(in_obj);
    req.user_prompt = extract_user_prompt(in_obj);

    req.max_output_tokens = vn::llm::kMinOutputTokensFloor;
    if (const auto* mt = in_obj.if_contains("max_tokens"); mt != nullptr) {
        if (mt->is_int64()) {
            req.max_output_tokens = mt->get_int64();
        } else if (mt->is_uint64()) {
            req.max_output_tokens = static_cast<std::int64_t>(mt->get_uint64());
        }
    }
    req.enforce_min_output_floor = true;
    req.safety_block_none = true;

    req.thinking_level = std::string(vn::llm::kDefaultThinkingLevel);
    if (const auto* tc = in_obj.if_contains("thinkingConfig");
        tc != nullptr && tc->is_object()) {
        if (const auto* tl = tc->get_object().if_contains("thinkingLevel");
            tl != nullptr && tl->is_string() && !tl->get_string().empty()) {
            req.thinking_level = std::string(tl->get_string());
        }
    } else if (const auto* tc_snake = in_obj.if_contains("thinking_config");
               tc_snake != nullptr && tc_snake->is_object()) {
        if (const auto* tl = tc_snake->get_object().if_contains("thinking_level");
            tl != nullptr && tl->is_string() && !tl->get_string().empty()) {
            req.thinking_level = std::string(tl->get_string());
        }
    }

    return req;
}

std::size_t extract_slice_size(const bj::object& in_obj) noexcept {
    const auto* s = in_obj.if_contains("slice_size");
    if (s == nullptr) {
        return 0;
    }
    if (s->is_int64() && s->get_int64() > 0) {
        return static_cast<std::size_t>(s->get_int64());
    }
    if (s->is_uint64()) {
        return static_cast<std::size_t>(s->get_uint64());
    }
    return 0;
}

bj::value response_to_messages_json(const vn::llm::LlmResponse& resp) {
    std::string stop_reason = "end_turn";
    if (resp.content_blocked) {
        stop_reason = "refusal";
    } else if (resp.finish_reason == "STOP" || resp.finish_reason.empty()) {
        stop_reason = "end_turn";
    } else if (resp.finish_reason == "MAX_TOKENS") {
        stop_reason = "max_tokens";
    } else {
        stop_reason = resp.finish_reason;
    }

    bj::object out;
    out["content"] = bj::array{
        bj::object{{"type", "text"}, {"text", resp.text}}};
    out["stop_reason"] = std::move(stop_reason);
    out["usage"] = bj::object{
        {"input_tokens", resp.usage.prompt_tokens},
        {"candidates_tokens", resp.usage.candidate_tokens},
        {"thought_tokens", resp.usage.thought_tokens},
        {"output_tokens", resp.usage.total_output_tokens()},
        {"cache_read_input_tokens", resp.usage.cached_tokens},
        {"cache_creation_input_tokens", std::int64_t{0}},
    };
    return out;
}

}  // namespace

bj::object build_gemini_request(const bj::object& in_obj, std::string* out_model) {
    try {
        const vn::llm::LlmRequest llm_req = to_llm_request(in_obj);
        if (out_model != nullptr) {
            *out_model = llm_req.model;
        }
        return bj::parse(vn::llm::build_gemini_request_json(llm_req)).as_object();
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

bj::value adapt_gemini_response(const bj::value& gem_val) {
    const vn::llm::LlmResponse parsed =
        vn::llm::parse_gemini_response_json(bj::serialize(gem_val));
    return response_to_messages_json(parsed);
}

std::string load_api_key() {
    return vn::llm::resolve_gemini_api_key();
}

struct Client::Impl {
    std::string api_key;
    vn::llm::IRawHttpTransport* raw_transport = nullptr;
    int configured_timeout_sec = kDefaultTimeoutSec;
    std::unique_ptr<vn::llm::ILlmClient> llm_client;
    vn::llm::ZeroThoughtCircuitBreaker zero_thought_guard;

    Impl(std::string key, vn::llm::IRawHttpTransport* transport)
        : api_key(std::move(key)), raw_transport(transport) {
        if (api_key.empty()) {
            api_key = load_api_key();
        }
        if (api_key.empty()) {
            throw ApiError("GEMINI_API_KEY not set");
        }
        rebuild_client(kDefaultTimeoutSec);
    }

    void rebuild_client(int timeout_sec) {
        configured_timeout_sec = timeout_sec;
        vn::llm::LlmClientOptions opts;
        opts.api_key = api_key;
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.read_timeout_seconds = timeout_sec;
        opts.raw_transport = raw_transport;
        opts.log_fn = [](std::string_view msg) { log_info(std::string(msg)); };
        // When an injected offline transport is active, fail deterministically
        // without sleeping across retries.
        if (raw_transport != nullptr) {
            opts.max_retries = 0;
        }
        llm_client = vn::llm::create_llm_client(std::move(opts));
    }

    vn::llm::ILlmClient& client_for_timeout(int timeout_sec) {
        if (!llm_client || timeout_sec != configured_timeout_sec) {
            rebuild_client(timeout_sec);
        }
        return *llm_client;
    }
};

Client::Client(std::string api_key, vn::llm::IRawHttpTransport* raw_transport)
    : impl_(std::make_unique<Impl>(std::move(api_key), raw_transport)) {}
Client::~Client() = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

struct LazyClient::Impl {
    std::optional<Client> client;
};

LazyClient::LazyClient() : impl_(std::make_unique<Impl>()) {}
LazyClient::~LazyClient() = default;
LazyClient::LazyClient(LazyClient&& other) noexcept = default;
LazyClient& LazyClient::operator=(LazyClient&& other) noexcept = default;

Client& LazyClient::get() {
    if (!impl_->client) {
        std::string key = load_api_key();
        if (key.empty()) {
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        }
        impl_->client.emplace(std::move(key));
    }
    return *impl_->client;
}

bj::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        const bj::object empty_obj;
        const bj::object& in_obj = body.is_object() ? body.get_object() : empty_obj;
        const vn::llm::LlmRequest llm_req = to_llm_request(in_obj);
        const std::size_t slice_size = extract_slice_size(in_obj);

        const vn::llm::LlmResponse resp =
            impl_->client_for_timeout(timeout_sec).generate(llm_req);

        impl_->zero_thought_guard.check_response(
            llm_req.model,
            llm_req.thinking_level,
            /*is_top_level=*/slice_size > 0,
            slice_size,
            resp.content_blocked,
            resp.usage.thought_tokens);
        if (slice_size > 0 && !resp.content_blocked) {
            impl_->zero_thought_guard.record_top_level_batch_completed();
        }

        return response_to_messages_json(resp);
    } catch (const vn::llm::LlmZeroThoughtError& e) {
        throw ZeroThoughtError(e.what());
    } catch (const vn::llm::LlmRateLimitError& e) {
        throw RateLimitError(e.what());
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    } catch (const ZeroThoughtError&) {
        throw;
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace snt::gemini
