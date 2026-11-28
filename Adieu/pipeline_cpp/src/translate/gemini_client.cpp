// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin Object Adapter bridging Adieu's Boost.JSON `IMessageClient` contract onto
// the shared `vn::llm_client` library (`create_llm_client`, `gemini_wire_codec`,
// `resolve_gemini_api_key`, and `ZeroThoughtCircuitBreaker`).
#include "translate/gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace adi::gemini {

namespace bj = boost::json;

namespace {

constexpr int kHttpOkStatus = 200;
constexpr std::size_t kFallbackSliceSize = 1u;
constexpr std::string_view kGeminiModelPrefix = "gemini-";
constexpr std::string_view kLinesOpenTag = "<lines_to_translate>\n";
constexpr std::string_view kLinesCloseTag = "</lines_to_translate>";

std::string resolve_gemini_model(const bj::object& in_obj) {
    if (const auto* m = in_obj.if_contains("model"); m && m->is_string()) {
        const std::string_view raw_model(m->as_string().data(), m->as_string().size());
        vn::llm::ensure_supported_model(raw_model);
        if (raw_model.substr(0, kGeminiModelPrefix.size()) == kGeminiModelPrefix) {
            return std::string(raw_model);
        }
    }
    return std::string(vn::llm::kDefaultModel);
}

std::string extract_system_prompt(const bj::object& in_obj) {
    const auto* sys = in_obj.if_contains("system");
    if (!sys) {
        return {};
    }
    if (sys->is_string()) {
        return std::string(sys->as_string());
    }
    if (!sys->is_array()) {
        return {};
    }
    std::string sys_text;
    for (const auto& item : sys->as_array()) {
        if (!item.is_object()) continue;
        const auto* t = item.as_object().if_contains("text");
        if (!t || !t->is_string()) continue;
        if (!sys_text.empty()) sys_text += "\n";
        sys_text += std::string(t->as_string());
    }
    return sys_text;
}

std::int64_t extract_requested_max_tokens(const bj::object& in_obj) {
    const auto* mt = in_obj.if_contains("max_tokens");
    if (!mt) {
        return vn::llm::kMinOutputTokensFloor;
    }
    if (mt->is_int64()) {
        return mt->as_int64();
    }
    if (mt->is_uint64()) {
        const std::uint64_t raw_u = mt->as_uint64();
        return raw_u > static_cast<std::uint64_t>(vn::llm::kMaxOutputTokensCeiling)
                   ? vn::llm::kMaxOutputTokensCeiling
                   : static_cast<std::int64_t>(raw_u);
    }
    return vn::llm::kMinOutputTokensFloor;
}

std::optional<std::string> try_extract_single_user_text(const bj::object& in_obj) {
    const auto* msgs = in_obj.if_contains("messages");
    if (!msgs || !msgs->is_array() || msgs->as_array().size() != 1u) {
        return std::nullopt;
    }
    const auto& first = msgs->as_array()[0];
    if (!first.is_object()) {
        return std::nullopt;
    }
    const auto& msg_obj = first.as_object();
    if (const auto* r = msg_obj.if_contains("role"); r && r->is_string()) {
        if (r->as_string() == "assistant") {
            return std::nullopt;
        }
    }
    const auto* c = msg_obj.if_contains("content");
    if (!c || !c->is_string()) {
        return std::nullopt;
    }
    return std::string(c->as_string());
}

bj::array build_custom_contents(const bj::object& in_obj) {
    bj::array contents;
    const auto* msgs = in_obj.if_contains("messages");
    if (!msgs || !msgs->is_array()) {
        return contents;
    }
    for (const auto& msg_val : msgs->as_array()) {
        if (!msg_val.is_object()) continue;
        const auto& msg_obj = msg_val.as_object();
        std::string role = "user";
        if (const auto* r = msg_obj.if_contains("role"); r && r->is_string()) {
            if (r->as_string() == "assistant") role = "model";
        }

        bj::array parts;
        if (const auto* c = msg_obj.if_contains("content")) {
            if (c->is_string()) {
                parts.push_back(bj::object{{"text", std::string(c->as_string())}});
            } else if (c->is_array()) {
                for (const auto& block_val : c->as_array()) {
                    if (!block_val.is_object()) continue;
                    const auto& block = block_val.as_object();
                    std::string btype;
                    if (const auto* bt = block.if_contains("type"); bt && bt->is_string()) {
                        btype = std::string(bt->as_string());
                    }
                    if (btype == "text") {
                        if (const auto* txt = block.if_contains("text");
                            txt && txt->is_string()) {
                            parts.push_back(
                                bj::object{{"text", std::string(txt->as_string())}});
                        }
                        continue;
                    }
                    if (btype == "image") {
                        if (const auto* src = block.if_contains("source");
                            src && src->is_object()) {
                            const auto& src_obj = src->as_object();
                            const auto* mt = src_obj.if_contains("media_type");
                            const auto* dt = src_obj.if_contains("data");
                            if (mt && mt->is_string() && dt && dt->is_string()) {
                                parts.push_back(bj::object{
                                    {"inlineData",
                                     bj::object{
                                         {"mimeType", std::string(mt->as_string())},
                                         {"data", std::string(dt->as_string())},
                                     }}});
                            }
                        }
                    }
                }
            }
        }
        contents.push_back(bj::object{{"role", role}, {"parts", std::move(parts)}});
    }
    return contents;
}

vn::llm::LlmRequest to_llm_request(const bj::value& body) {
    vn::llm::LlmRequest req;
    req.model = std::string(vn::llm::kDefaultModel);
    req.thinking_level = std::string(vn::llm::kDefaultThinkingLevel);
    req.max_output_tokens = vn::llm::kMinOutputTokensFloor;
    req.enforce_min_output_floor = true;
    req.safety_block_none = true;

    if (!body.is_object()) {
        return req;
    }
    const auto& in_obj = body.as_object();
    req.model = resolve_gemini_model(in_obj);
    req.system_prompt = extract_system_prompt(in_obj);
    req.max_output_tokens = extract_requested_max_tokens(in_obj);
    if (auto single_user = try_extract_single_user_text(in_obj)) {
        req.user_prompt = std::move(*single_user);
    }
    return req;
}

std::string map_stop_reason(const vn::llm::LlmResponse& resp) {
    if (resp.content_blocked) {
        return "refusal";
    }
    if (resp.finish_reason.empty() || resp.finish_reason == "STOP") {
        return "end_turn";
    }
    if (resp.finish_reason == "MAX_TOKENS") {
        return "max_tokens";
    }
    return resp.finish_reason;
}

bj::value to_message_envelope(const vn::llm::LlmResponse& resp) {
    bj::object out;
    out["usage"] = bj::object{
        {"input_tokens", resp.usage.prompt_tokens},
        {"output_tokens", resp.usage.total_output_tokens()},
        {"thought_tokens", resp.usage.thought_tokens},
        {"cache_read_input_tokens", resp.usage.cached_tokens},
        {"cache_creation_input_tokens", std::int64_t{0}},
    };
    out["content"] = bj::array{bj::object{{"type", "text"}, {"text", resp.text}}};
    out["stop_reason"] = map_stop_reason(resp);
    return out;
}

std::size_t count_batch_slice_lines(std::string_view user_prompt) noexcept {
    const std::size_t start = user_prompt.find(kLinesOpenTag);
    if (start == std::string_view::npos) {
        return kFallbackSliceSize;
    }
    const std::size_t content_start = start + kLinesOpenTag.size();
    const std::size_t end = user_prompt.find(kLinesCloseTag, content_start);
    if (end == std::string_view::npos) {
        return kFallbackSliceSize;
    }
    const std::string_view block =
        user_prompt.substr(content_start, end - content_start);
    std::size_t count = 0u;
    for (char ch : block) {
        if (ch == '\n') {
            ++count;
        }
    }
    return count > 0u ? count : kFallbackSliceSize;
}

std::unique_ptr<vn::llm::ILlmClient> make_shared_client(const ClientOptions& client_opts,
                                                        int read_timeout_sec) {
    vn::llm::LlmClientOptions opts;
    opts.provider = vn::llm::LlmProvider::Gemini;
    opts.api_key = client_opts.api_key;
    opts.default_model = std::string(vn::llm::kDefaultModel);
    opts.max_retries = client_opts.max_retries;
    opts.zero_thought_max_retries = client_opts.zero_thought_max_retries;
    opts.initial_backoff = client_opts.initial_backoff;
    opts.read_timeout_seconds =
        read_timeout_sec > 0 ? read_timeout_sec : kDefaultRequestTimeoutSec;
    opts.sleep_fn = client_opts.sleep_fn;
    opts.log_fn = [](std::string_view msg) { log_info(std::string(msg)); };
    opts.raw_transport = client_opts.raw_transport;
    return vn::llm::create_llm_client(std::move(opts));
}

}  // namespace

std::pair<std::string, bj::object> build_gemini_request(const bj::value& body) {
    try {
        const vn::llm::LlmRequest llm_req = to_llm_request(body);
        bj::object req_json =
            bj::parse(vn::llm::build_gemini_request_json(llm_req)).as_object();

        if (!body.is_object()) {
            req_json["contents"] = bj::array{};
            return {llm_req.model, req_json};
        }

        const auto& in_obj = body.as_object();
        if (!try_extract_single_user_text(in_obj).has_value()) {
            req_json["contents"] = build_custom_contents(in_obj);
        }

        if (const auto* temp = in_obj.if_contains("temperature")) {
            auto& gen_cfg = req_json.at("generationConfig").as_object();
            if (temp->is_double()) {
                gen_cfg["temperature"] = temp->as_double();
            } else if (temp->is_int64()) {
                gen_cfg["temperature"] = temp->as_int64();
            } else if (temp->is_uint64()) {
                gen_cfg["temperature"] = temp->as_uint64();
            }
        }

        return {llm_req.model, req_json};
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

bj::value adapt_gemini_response(const bj::value& raw) {
    if (!raw.is_object()) {
        return to_message_envelope(vn::llm::LlmResponse{});
    }

    const std::string serialized = bj::serialize(raw);
    try {
        return to_message_envelope(
            vn::llm::parse_gemini_response_json(serialized, kHttpOkStatus));
    } catch (const vn::llm::LlmError&) {
        // `vn::llm::parse_gemini_response_json` rejects unblocked empty-text
        // responses in live traffic; for offline callers passing metadata-only,
        // error-only, or thought-only payloads, strip `"error"` and inject a
        // sentinel candidate to reuse `usageMetadata` and `finishReason` extraction.
        bj::object synthetic = raw.as_object();
        synthetic.erase("error");
        bj::object sentinel_part{{"text", "x"}};
        bj::object sentinel_content{{"parts", bj::array{std::move(sentinel_part)}}};
        if (auto* cands = synthetic.if_contains("candidates");
            cands && cands->is_array() && !cands->as_array().empty() &&
            cands->as_array()[0].is_object()) {
            cands->as_array()[0].as_object()["content"] = std::move(sentinel_content);
        } else {
            synthetic["candidates"] =
                bj::array{bj::object{{"content", std::move(sentinel_content)}}};
        }
        try {
            vn::llm::LlmResponse fallback =
                vn::llm::parse_gemini_response_json(bj::serialize(synthetic), kHttpOkStatus);
            fallback.text.clear();
            return to_message_envelope(fallback);
        } catch (const vn::llm::LlmError&) {
            return to_message_envelope(vn::llm::LlmResponse{});
        }
    }
}

std::string load_api_key() {
    return vn::llm::resolve_gemini_api_key();
}

IMessageClient::~IMessageClient() = default;

struct Client::Impl {
    ClientOptions options;
    int configured_timeout_sec = kDefaultRequestTimeoutSec;
    std::unique_ptr<vn::llm::ILlmClient> llm_client;
    vn::llm::ZeroThoughtCircuitBreaker thought_guard;
};

struct LazyClient::Impl {
    std::optional<Client> client;
};

LazyClient::LazyClient() : impl_(std::make_unique<Impl>()) {}
LazyClient::LazyClient(LazyClient&& other) noexcept
    : IMessageClient(), impl_(std::move(other.impl_)) {}
LazyClient& LazyClient::operator=(LazyClient&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}
LazyClient::~LazyClient() = default;

Client& LazyClient::get() {
    if (!impl_) {
        throw ApiError("LazyClient used after move");
    }
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

bj::value LazyClient::messages(const bj::value& body, int timeout_sec) {
    return get().messages(body, timeout_sec);
}

Client::Client(std::string api_key)
    : Client(ClientOptions{std::move(api_key)}) {}

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    if (impl_->options.api_key.empty()) {
        impl_->options.api_key = load_api_key();
    }
    if (impl_->options.api_key.empty()) {
        throw ApiError("GEMINI_API_KEY not set");
    }
    impl_->configured_timeout_sec =
        impl_->options.request_timeout_sec > 0 ? impl_->options.request_timeout_sec
                                               : kDefaultRequestTimeoutSec;
    impl_->llm_client =
        make_shared_client(impl_->options, impl_->configured_timeout_sec);
}

Client::Client(Client&& other) noexcept
    : IMessageClient(), impl_(std::move(other.impl_)) {}
Client& Client::operator=(Client&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}
Client::~Client() = default;

bj::value Client::messages(const bj::value& body, int timeout_sec) {
    if (!impl_) {
        throw ApiError("Client used after move");
    }
    try {
        vn::llm::LlmRequest llm_req = to_llm_request(body);
        if (body.is_object() && !try_extract_single_user_text(body.as_object()).has_value()) {
            // Flatten any multi-block text parts into `user_prompt` for `ILlmClient`.
            const bj::array contents = build_custom_contents(body.as_object());
            std::string combined;
            for (const auto& turn : contents) {
                if (!turn.is_object()) continue;
                const auto* parts = turn.as_object().if_contains("parts");
                if (!parts || !parts->is_array()) continue;
                for (const auto& part : parts->as_array()) {
                    if (!part.is_object()) continue;
                    if (const auto* t = part.as_object().if_contains("text");
                        t && t->is_string()) {
                        if (!combined.empty()) combined += "\n";
                        combined += std::string(t->as_string());
                    }
                }
            }
            llm_req.user_prompt = std::move(combined);
        }

        const int effective_timeout =
            timeout_sec > 0 ? timeout_sec : kDefaultRequestTimeoutSec;
        if (effective_timeout != impl_->configured_timeout_sec) {
            auto updated_client = make_shared_client(impl_->options, effective_timeout);
            impl_->llm_client = std::move(updated_client);
            impl_->configured_timeout_sec = effective_timeout;
        }

        const std::size_t slice_size = count_batch_slice_lines(llm_req.user_prompt);
        const vn::llm::LlmResponse resp = impl_->llm_client->generate(llm_req);

        impl_->thought_guard.check_response(llm_req.model,
                                            llm_req.thinking_level,
                                            /*is_top_level=*/true,
                                            slice_size,
                                            resp.content_blocked,
                                            resp.usage.thought_tokens);
        impl_->thought_guard.record_top_level_batch_completed();

        return to_message_envelope(resp);
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

}  // namespace adi::gemini
