// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin adapter delegating wire serialization, HTTPS/TLS transport, retries,
// credential resolution, and zero-thought circuit breaking to `vn::llm_client`,
// while preserving multimodal `inlineData` image blocks for the narrative-CG
// vision scanner (`03_find_narrative_cg`).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "gemini_client.h"

#include "common/util.h"
#include "vn/llm/gemini_wire_codec.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/zero_thought_guard.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
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

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::string_view kGeminiModelPrefix = "gemini-";
constexpr std::string_view kDefaultImageMimeType = "image/png";
constexpr const char* kGeminiApiHost = "generativelanguage.googleapis.com";
constexpr const char* kGeminiApiPort = "443";
constexpr const char* kUserAgentHeader = "snt-pipeline-cpp/1.0";
constexpr const char* kJsonMimeType = "application/json";

constexpr int kHttpVersion11 = 11;
constexpr int kMinTimeoutSeconds = 1;
constexpr int kNoRetriesForInjectedTransport = 0;
constexpr DWORD kCertCloseDefaultFlags = 0;
constexpr HCRYPTPROV_LEGACY kDefaultCryptProv = 0;
constexpr int kOpenSslSuccess = 1;
constexpr std::size_t kZeroCertsLoaded = 0;
constexpr std::uint64_t kBytesPerKib = 1024ULL;
constexpr std::uint64_t kBytesPerMib = kBytesPerKib * kBytesPerKib;
constexpr std::uint64_t kMaxResponseBodyBytes = 64ULL * kBytesPerMib;

struct CertStoreGuard {
    explicit CertStoreGuard(HCERTSTORE h) noexcept : handle(h) {}
    ~CertStoreGuard() {
        if (handle != nullptr) {
            ::CertCloseStore(handle, kCertCloseDefaultFlags);
        }
    }

    CertStoreGuard(const CertStoreGuard&) = delete;
    CertStoreGuard& operator=(const CertStoreGuard&) = delete;

    HCERTSTORE handle = nullptr;
};

struct X509StoreDeleter {
    void operator()(X509_STORE* store) const noexcept {
        if (store != nullptr) {
            X509_STORE_free(store);
        }
    }
};

struct X509CertDeleter {
    void operator()(X509* cert) const noexcept {
        if (cert != nullptr) {
            X509_free(cert);
        }
    }
};

using X509StorePtr = std::unique_ptr<X509_STORE, X509StoreDeleter>;
using X509CertPtr = std::unique_ptr<X509, X509CertDeleter>;

void populate_windows_root_certs(asio::ssl::context& ssl_ctx) {
    const CertStoreGuard win_store(
        ::CertOpenSystemStoreW(kDefaultCryptProv, L"ROOT"));
    if (win_store.handle == nullptr) {
        throw vn::llm::LlmTransientError("CertOpenSystemStoreW(ROOT) failed");
    }

    X509StorePtr x509_store(X509_STORE_new());
    if (!x509_store) {
        throw vn::llm::LlmTransientError("X509_STORE_new failed");
    }

    std::size_t loaded = kZeroCertsLoaded;
    for (PCCERT_CONTEXT cert =
             ::CertEnumCertificatesInStore(win_store.handle, nullptr);
         cert != nullptr;
         cert = ::CertEnumCertificatesInStore(win_store.handle, cert)) {
        const unsigned char* encoded = cert->pbCertEncoded;
        X509CertPtr decoded(d2i_X509(
            nullptr, &encoded, static_cast<long>(cert->cbCertEncoded)));
        if (decoded &&
            X509_STORE_add_cert(x509_store.get(), decoded.get()) ==
                kOpenSslSuccess) {
            ++loaded;
        }
    }

    if (loaded == kZeroCertsLoaded) {
        throw vn::llm::LlmTransientError(
            "Windows ROOT certificate store yielded 0 X.509 certificates");
    }

    SSL_CTX_set_cert_store(ssl_ctx.native_handle(), x509_store.release());
}

// Live HTTPS transport used only when a multimodal request (`"type": "image"`)
// runs without an injected test `IRawHttpTransport`.
class DirectHttpsTransport final : public vn::llm::IRawHttpTransport {
public:
    vn::llm::RawHttpResponse post_json(std::string_view target_path,
                                       std::string_view json_body,
                                       int connect_timeout_sec,
                                       int read_timeout_sec) override {
        if (!ssl_ctx_.has_value()) {
            asio::ssl::context ctx(asio::ssl::context::tls_client);
            ctx.set_verify_mode(asio::ssl::verify_peer);
            populate_windows_root_certs(ctx);
            ssl_ctx_.emplace(std::move(ctx));
        }

        const int effective_connect_s =
            std::max(kMinTimeoutSeconds, connect_timeout_sec);
        const int effective_read_s =
            std::max(kMinTimeoutSeconds, read_timeout_sec);

        asio::io_context ioc;
        beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx_);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), kGeminiApiHost)) {
            throw vn::llm::LlmTransientError("TLS SNI host setup failed");
        }
        if (SSL_set1_host(stream.native_handle(), kGeminiApiHost) !=
            kOpenSslSuccess) {
            throw vn::llm::LlmTransientError(
                "TLS hostname verification setup failed");
        }

        tcp::resolver resolver(ioc);
        const auto endpoints = resolver.resolve(kGeminiApiHost, kGeminiApiPort);
        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(effective_connect_s));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);

        http::request<http::string_body> req{
            http::verb::post,
            beast::string_view(target_path.data(), target_path.size()),
            kHttpVersion11};
        req.set(http::field::host, kGeminiApiHost);
        req.set(http::field::user_agent, kUserAgentHeader);
        req.set(http::field::content_type, kJsonMimeType);
        req.body().assign(json_body.data(), json_body.size());
        req.prepare_payload();

        http::write(stream, req);

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(effective_read_s));

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(kMaxResponseBodyBytes);
        http::read(stream, buffer, parser);
        http::response<http::string_body> res = parser.release();

        beast::error_code shutdown_ec;
        stream.shutdown(shutdown_ec);

        return vn::llm::RawHttpResponse{
            static_cast<int>(res.result_int()),
            std::move(res.body()),
        };
    }

private:
    std::optional<asio::ssl::context> ssl_ctx_;
};

// Transport proxy splicing structured multimodal `contents` (`inlineData` +
// `text` parts) into the `vn::llm`-built request JSON while keeping `vn::llm`
// retries, URL construction, secret redaction, and response parsing intact.
class MultimodalPayloadProxy final : public vn::llm::IRawHttpTransport {
public:
    MultimodalPayloadProxy(vn::llm::IRawHttpTransport& backing,
                           const bj::array& multimodal_contents,
                           std::string_view base_user_prompt) noexcept
        : backing_(backing),
          multimodal_contents_(multimodal_contents),
          base_user_prompt_(base_user_prompt) {}

    vn::llm::RawHttpResponse post_json(std::string_view target_path,
                                       std::string_view json_body,
                                       int connect_timeout_sec,
                                       int read_timeout_sec) override {
        bj::object wire = bj::parse(json_body).as_object();
        std::string retry_suffix;
        if (const auto* c_v = wire.if_contains("contents");
            c_v != nullptr && c_v->is_array() && !c_v->as_array().empty()) {
            const bj::object& turn0 = c_v->as_array()[0].as_object();
            if (const auto* p_v = turn0.if_contains("parts");
                p_v != nullptr && p_v->is_array() && !p_v->as_array().empty()) {
                const bj::object& part0 = p_v->as_array()[0].as_object();
                if (const auto* t_v = part0.if_contains("text");
                    t_v != nullptr && t_v->is_string()) {
                    const std::string_view attempt_text(
                        t_v->as_string().data(), t_v->as_string().size());
                    if (attempt_text.size() > base_user_prompt_.size() &&
                        attempt_text.substr(0, base_user_prompt_.size()) ==
                            base_user_prompt_) {
                        retry_suffix.assign(
                            attempt_text.substr(base_user_prompt_.size()));
                    }
                }
            }
        }

        bj::array patched_contents = multimodal_contents_;
        if (!retry_suffix.empty() && !patched_contents.empty()) {
            bj::array& last_parts =
                patched_contents.back().as_object().at("parts").as_array();
            bool appended = false;
            for (std::size_t i = last_parts.size(); i-- > 0;) {
                bj::object& part = last_parts[i].as_object();
                if (auto* txt = part.if_contains("text");
                    txt != nullptr && txt->is_string()) {
                    std::string combined(txt->as_string());
                    combined += retry_suffix;
                    part["text"] = std::move(combined);
                    appended = true;
                    break;
                }
            }
            if (!appended) {
                last_parts.push_back(bj::object{{"text", std::move(retry_suffix)}});
            }
        }
        wire["contents"] = std::move(patched_contents);

        const std::string serialized = bj::serialize(wire);
        return backing_.post_json(
            target_path, serialized, connect_timeout_sec, read_timeout_sec);
    }

private:
    vn::llm::IRawHttpTransport& backing_;
    const bj::array& multimodal_contents_;
    std::string_view base_user_prompt_;
};

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

bool contains_image_content_block(const bj::object& in_obj) noexcept {
    const auto* msgs_v = in_obj.if_contains("messages");
    if (msgs_v == nullptr || !msgs_v->is_array()) {
        return false;
    }
    for (const auto& msg_v : msgs_v->get_array()) {
        if (!msg_v.is_object()) {
            continue;
        }
        const auto* c_v = msg_v.get_object().if_contains("content");
        if (c_v == nullptr || !c_v->is_array()) {
            continue;
        }
        for (const auto& block_v : c_v->get_array()) {
            if (!block_v.is_object()) {
                continue;
            }
            const auto* bt = block_v.get_object().if_contains("type");
            if (bt != nullptr && bt->is_string() && bt->get_string() == "image") {
                return true;
            }
        }
    }
    return false;
}

bj::object build_inline_data_part(const bj::object& block) {
    const auto* src_v = block.if_contains("source");
    if (src_v == nullptr || !src_v->is_object()) {
        throw ApiError("image content block missing source object");
    }
    const bj::object& src = src_v->get_object();
    if (const auto* st = src.if_contains("type");
        st != nullptr && st->is_string() && st->get_string() != "base64") {
        throw ApiError("image content block requires base64 source type");
    }

    std::string mime(kDefaultImageMimeType);
    if (const auto* mt = src.if_contains("media_type");
        mt != nullptr && mt->is_string() && !mt->get_string().empty()) {
        mime.assign(mt->get_string().data(), mt->get_string().size());
    }

    const auto* data_v = src.if_contains("data");
    if (data_v == nullptr || !data_v->is_string() || data_v->get_string().empty()) {
        throw ApiError("image content block requires non-empty base64 data");
    }
    std::string data(data_v->get_string().data(), data_v->get_string().size());

    return bj::object{
        {"inlineData",
         bj::object{{"mimeType", std::move(mime)}, {"data", std::move(data)}}}};
}

std::optional<bj::array> extract_multimodal_contents(const bj::object& in_obj) {
    if (!contains_image_content_block(in_obj)) {
        return std::nullopt;
    }

    const auto* msgs_v = in_obj.if_contains("messages");
    bj::array contents;
    for (const auto& msg_v : msgs_v->get_array()) {
        if (!msg_v.is_object()) {
            continue;
        }
        const bj::object& msg_obj = msg_v.get_object();
        std::string role = "user";
        if (const auto* r = msg_obj.if_contains("role");
            r != nullptr && r->is_string() &&
            (r->get_string() == "assistant" || r->get_string() == "model")) {
            role = "model";
        }

        bj::array parts;
        const auto* c_v = msg_obj.if_contains("content");
        if (c_v != nullptr && c_v->is_string()) {
            parts.push_back(bj::object{{"text", std::string(c_v->get_string())}});
        } else if (c_v != nullptr && c_v->is_array()) {
            for (const auto& block_v : c_v->get_array()) {
                if (!block_v.is_object()) {
                    continue;
                }
                const bj::object& block = block_v.get_object();
                const auto* bt = block.if_contains("type");
                if (bt == nullptr || !bt->is_string()) {
                    continue;
                }
                if (bt->get_string() == "text") {
                    if (const auto* txt = block.if_contains("text");
                        txt != nullptr && txt->is_string()) {
                        parts.push_back(
                            bj::object{{"text", std::string(txt->get_string())}});
                    }
                    continue;
                }
                if (bt->get_string() == "image") {
                    parts.push_back(build_inline_data_part(block));
                }
            }
        }
        if (!parts.empty()) {
            contents.push_back(bj::object{
                {"role", std::move(role)},
                {"parts", std::move(parts)},
            });
        }
    }

    if (contents.empty()) {
        throw ApiError("multimodal request produced empty contents");
    }
    return contents;
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
        const std::optional<bj::array> multimodal_contents =
            extract_multimodal_contents(in_obj);
        bj::object req_obj =
            bj::parse(vn::llm::build_gemini_request_json(llm_req)).as_object();
        if (multimodal_contents.has_value()) {
            req_obj["contents"] = *multimodal_contents;
        }
        if (out_model != nullptr) {
            *out_model = llm_req.model;
        }
        return req_obj;
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

bj::value adapt_gemini_response(const bj::value& gem_val) {
    try {
        const vn::llm::LlmResponse parsed =
            vn::llm::parse_gemini_response_json(bj::serialize(gem_val));
        return response_to_messages_json(parsed);
    } catch (const vn::llm::LlmRateLimitError& e) {
        throw RateLimitError(e.what());
    } catch (const vn::llm::LlmError& e) {
        throw ApiError(e.what());
    }
}

std::string load_api_key() {
    return vn::llm::resolve_gemini_api_key();
}

struct Client::Impl {
    std::string api_key;
    vn::llm::IRawHttpTransport* raw_transport = nullptr;
    DirectHttpsTransport live_multimodal_transport;
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
            opts.max_retries = kNoRetriesForInjectedTransport;
        }
        llm_client = vn::llm::create_llm_client(std::move(opts));
    }

    vn::llm::ILlmClient& client_for_timeout(int timeout_sec) {
        if (!llm_client || timeout_sec != configured_timeout_sec) {
            rebuild_client(timeout_sec);
        }
        return *llm_client;
    }

    vn::llm::LlmResponse generate_multimodal(const vn::llm::LlmRequest& llm_req,
                                             const bj::array& multimodal_contents,
                                             int timeout_sec) {
        vn::llm::IRawHttpTransport& backing =
            raw_transport != nullptr ? *raw_transport : live_multimodal_transport;
        MultimodalPayloadProxy proxy(
            backing, multimodal_contents, llm_req.user_prompt);

        vn::llm::LlmClientOptions opts;
        opts.api_key = api_key;
        opts.default_model = std::string(vn::llm::kDefaultModel);
        opts.read_timeout_seconds = timeout_sec;
        opts.raw_transport = &proxy;
        opts.log_fn = [](std::string_view msg) { log_info(std::string(msg)); };
        if (raw_transport != nullptr) {
            opts.max_retries = kNoRetriesForInjectedTransport;
            opts.zero_thought_max_retries = kNoRetriesForInjectedTransport;
        } else {
            opts.max_retries = vn::llm::kDefaultMaxRetries;
            opts.zero_thought_max_retries = vn::llm::kDefaultZeroThoughtRetries;
        }
        return vn::llm::create_llm_client(std::move(opts))->generate(llm_req);
    }
};

Client::Client(std::string api_key, vn::llm::IRawHttpTransport* raw_transport)
    : impl_(std::make_unique<Impl>(std::move(api_key), raw_transport)) {}
Client::~Client() = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

struct LazyClient::Impl {
    vn::llm::IRawHttpTransport* raw_transport = nullptr;
    std::optional<Client> client;
};

LazyClient::LazyClient(vn::llm::IRawHttpTransport* raw_transport)
    : impl_(std::make_unique<Impl>()) {
    impl_->raw_transport = raw_transport;
}
LazyClient::~LazyClient() = default;
LazyClient::LazyClient(LazyClient&& other) noexcept = default;
LazyClient& LazyClient::operator=(LazyClient&& other) noexcept = default;

Client& LazyClient::get() {
    if (!impl_->client) {
        std::string key = load_api_key();
        if (key.empty() && impl_->raw_transport != nullptr) {
            key = "offline-raw-transport";
        }
        if (key.empty()) {
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        }
        impl_->client.emplace(std::move(key), impl_->raw_transport);
    }
    return *impl_->client;
}

bj::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        const bj::object empty_obj;
        const bj::object& in_obj = body.is_object() ? body.get_object() : empty_obj;
        const vn::llm::LlmRequest llm_req = to_llm_request(in_obj);
        const std::optional<bj::array> multimodal_contents =
            extract_multimodal_contents(in_obj);
        const std::size_t slice_size = extract_slice_size(in_obj);

        const vn::llm::LlmResponse resp =
            multimodal_contents.has_value()
                ? impl_->generate_multimodal(
                      llm_req, *multimodal_contents, timeout_sec)
                : impl_->client_for_timeout(timeout_sec).generate(llm_req);

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
