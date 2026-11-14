// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gemini_client.h"

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace adi::gemini {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* kApiHost = "generativelanguage.googleapis.com";
constexpr const char* kApiPort = "443";
constexpr const char* kDefaultGeminiModel = "gemini-4-argon";
constexpr std::int64_t kDefaultMaxOutputTokens = 16384;
constexpr std::int64_t kMaxOutputTokensCeiling = 65536;
constexpr std::int64_t kDefaultThinkingBudget = 2048;
constexpr int kHttp11Version = 11;
constexpr std::uint64_t kMaxResponseBodyBytes = 64ull * 1024 * 1024;
constexpr unsigned kHttpTooManyRequests = 429;
constexpr unsigned kHttpOkMin = 200;
constexpr unsigned kHttpSuccessUpperBound = 300;
constexpr std::string_view kApiKeyEnvPrefix = "GEMINI_API_KEY=";
constexpr std::size_t kMinQuotedValueLen = 2;

std::string wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return {};
    }
    const int wlen = static_cast<int>(std::wcslen(val));
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, val, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error(
            "WideCharToMultiByte failed converting environment variable to UTF-8");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, val, wlen, out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        throw std::runtime_error(
            "WideCharToMultiByte produced an unexpected UTF-8 byte count");
    }
    return out;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed converting UTF-8 string");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    if (written != needed) {
        throw std::runtime_error("MultiByteToWideChar produced an unexpected unit count");
    }
    return out;
}

// OpenSSL doesn't read the Windows certificate store on its own; import the
// ROOT store so peer verification of generativelanguage.googleapis.com works.
void load_windows_root_certs(asio::ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return;
    X509_STORE* x509 = X509_STORE_new();
    for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr);
         cert != nullptr; cert = CertEnumCertificatesInStore(store, cert)) {
        const unsigned char* p = cert->pbCertEncoded;
        if (X509* x = d2i_X509(nullptr, &p, static_cast<long>(cert->cbCertEncoded))) {
            X509_STORE_add_cert(x509, x);
            X509_free(x);
        }
    }
    CertCloseStore(store, 0);
    SSL_CTX_set_cert_store(ctx.native_handle(), x509);
}

std::pair<std::string, bj::object> build_gemini_request(const bj::value& body) {
    std::string model = kDefaultGeminiModel;
    bj::object req_json;

    if (!body.is_object()) {
        req_json["contents"] = bj::array{};
        req_json["generationConfig"] = bj::object{
            {"maxOutputTokens", kDefaultMaxOutputTokens},
            {"thinkingConfig", bj::object{{"thinkingBudget", kDefaultThinkingBudget}}},
        };
        return {model, req_json};
    }

    const auto& in_obj = body.as_object();
    if (const auto* m = in_obj.if_contains("model"); m && m->is_string()) {
        std::string m_str(m->as_string());
        if (m_str.rfind("gemini-", 0) == 0) {
            model = std::move(m_str);
        }
    }

    if (const auto* sys = in_obj.if_contains("system")) {
        std::string sys_text;
        if (sys->is_string()) {
            sys_text = std::string(sys->as_string());
        } else if (sys->is_array()) {
            for (const auto& item : sys->as_array()) {
                if (item.is_object()) {
                    if (const auto* t = item.as_object().if_contains("text");
                        t && t->is_string()) {
                        if (!sys_text.empty()) sys_text += "\n";
                        sys_text += std::string(t->as_string());
                    }
                }
            }
        }
        if (!sys_text.empty()) {
            req_json["systemInstruction"] = bj::object{
                {"parts", bj::array{bj::object{{"text", std::move(sys_text)}}}}};
        }
    }

    bj::array contents;
    if (const auto* msgs = in_obj.if_contains("messages"); msgs && msgs->is_array()) {
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
                        } else if (btype == "image") {
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
    }
    req_json["contents"] = std::move(contents);

    bj::object gen_cfg;
    std::int64_t max_tokens = kDefaultMaxOutputTokens;
    if (const auto* mt = in_obj.if_contains("max_tokens")) {
        if (mt->is_int64()) {
            max_tokens = mt->as_int64();
        } else if (mt->is_uint64()) {
            max_tokens = static_cast<std::int64_t>(mt->as_uint64());
        }
    }
    if (max_tokens <= 0) max_tokens = kDefaultMaxOutputTokens;
    if (max_tokens > kMaxOutputTokensCeiling) max_tokens = kMaxOutputTokensCeiling;
    gen_cfg["maxOutputTokens"] = max_tokens;

    if (const auto* temp = in_obj.if_contains("temperature")) {
        if (temp->is_double()) {
            gen_cfg["temperature"] = temp->as_double();
        } else if (temp->is_int64()) {
            gen_cfg["temperature"] = temp->as_int64();
        } else if (temp->is_uint64()) {
            gen_cfg["temperature"] = temp->as_uint64();
        }
    }
    gen_cfg["thinkingConfig"] = bj::object{{"thinkingBudget", kDefaultThinkingBudget}};
    req_json["generationConfig"] = std::move(gen_cfg);

    return {model, req_json};
}

bj::value adapt_gemini_response(const bj::value& raw) {
    bj::object out;
    std::string text_out;
    std::string stop_reason = "end_turn";

    if (raw.is_object()) {
        const auto& robj = raw.as_object();
        const auto* cands = robj.if_contains("candidates");
        if (cands && cands->is_array() && !cands->as_array().empty() &&
            cands->as_array()[0].is_object()) {
            const auto& cand0 = cands->as_array()[0].as_object();
            if (const auto* content = cand0.if_contains("content");
                content && content->is_object()) {
                if (const auto* parts = content->as_object().if_contains("parts");
                    parts && parts->is_array()) {
                    for (const auto& pv : parts->as_array()) {
                        if (!pv.is_object()) continue;
                        const auto& part = pv.as_object();
                        if (!(!part.if_contains("thought") || !part.at("thought").is_bool() ||
                              !part.at("thought").as_bool())) {
                            continue;
                        }
                        if (const auto* txt = part.if_contains("text");
                            txt && txt->is_string()) {
                            text_out += std::string(txt->as_string());
                        }
                    }
                }
            }
            if (const auto* fr = cand0.if_contains("finishReason");
                fr && fr->is_string()) {
                const std::string reason(fr->as_string());
                if (reason == "STOP") {
                    stop_reason = "end_turn";
                } else if (reason == "MAX_TOKENS") {
                    stop_reason = "max_tokens";
                } else if (reason == "SAFETY" || reason == "RECITATION" ||
                           reason == "PROHIBITED_CONTENT" || reason == "BLOCKLIST" ||
                           reason == "SPII") {
                    stop_reason = "refusal";
                } else {
                    stop_reason = reason;
                }
            }
        } else if (const auto* pf = robj.if_contains("promptFeedback");
                   pf && pf->is_object() && pf->as_object().if_contains("blockReason")) {
            stop_reason = "refusal";
        }

        std::int64_t prompt_tok = 0;
        std::int64_t cand_tok = 0;
        std::int64_t thought_tok = 0;
        std::int64_t cached_tok = 0;
        if (const auto* um = robj.if_contains("usageMetadata");
            um && um->is_object()) {
            const auto& u = um->as_object();
            auto get_int = [&](const char* k) -> std::int64_t {
                if (const auto* v = u.if_contains(k)) {
                    if (v->is_int64()) return v->as_int64();
                    if (v->is_uint64()) return static_cast<std::int64_t>(v->as_uint64());
                }
                return 0;
            };
            prompt_tok = get_int("promptTokenCount");
            cand_tok = get_int("candidatesTokenCount");
            thought_tok = get_int("thoughtsTokenCount");
            cached_tok = get_int("cachedContentTokenCount");
        }
        out["usage"] = bj::object{
            {"input_tokens", prompt_tok},
            {"output_tokens", cand_tok + thought_tok},
            {"cache_read_input_tokens", cached_tok},
            {"cache_creation_input_tokens", std::int64_t{0}},
        };
    }

    out["content"] = bj::array{bj::object{{"type", "text"}, {"text", std::move(text_out)}}};
    out["stop_reason"] = std::move(stop_reason);
    return out;
}

}  // namespace

std::string load_api_key() {
    try {
        if (const std::string env = wide_env_to_utf8(L"GEMINI_API_KEY"); !env.empty()) {
            return env;
        }
        const std::string home = wide_env_to_utf8(L"USERPROFILE");
        if (home.empty()) return {};
        const std::filesystem::path cfg =
            std::filesystem::u8path(home + "\\.gemini\\.env");
        if (!std::filesystem::exists(cfg)) return {};
        std::ifstream in(cfg);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind(kApiKeyEnvPrefix.data(), 0) == 0) {
                std::string key = trim(line.substr(kApiKeyEnvPrefix.size()));
                if (key.size() >= kMinQuotedValueLen &&
                    ((key.front() == '"' && key.back() == '"') ||
                     (key.front() == '\'' && key.back() == '\''))) {
                    key = key.substr(1, key.size() - kMinQuotedValueLen);
                }
                if (!key.empty()) {
                    const std::wstring wide_key = utf8_to_wide_strict(key);
                    _wputenv_s(L"GEMINI_API_KEY", wide_key.c_str());
                    log_info("[OK] Loaded GEMINI_API_KEY from ~/.gemini/.env");
                    return key;
                }
            }
        }
        return {};
    } catch (...) {
        return {};
    }
}

IMessageClient::~IMessageClient() = default;

struct Client::Impl {
    std::string api_key;
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
    if (!impl_->client) {
        if (load_api_key().empty()) {
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        }
        impl_->client.emplace();
    }
    return *impl_->client;
}

boost::json::value LazyClient::messages(const bj::value& body, int timeout_sec) {
    return get().messages(body, timeout_sec);
}

Client::Client(std::string api_key) : impl_(std::make_unique<Impl>()) {
    impl_->api_key = std::move(api_key);
    if (impl_->api_key.empty()) {
        impl_->api_key = load_api_key();
    }
    if (impl_->api_key.empty()) {
        throw ApiError("GEMINI_API_KEY not set");
    }
}

Client::Client(Client&& other) noexcept
    : IMessageClient(), impl_(std::move(other.impl_)) {}
Client& Client::operator=(Client&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}
Client::~Client() = default;

boost::json::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        auto [model, req_json] = build_gemini_request(body);
        const std::string target =
            "/v1beta/models/" + model + ":generateContent?key=" + impl_->api_key;

        asio::io_context ioc;
        asio::ssl::context ssl_ctx(asio::ssl::context::tls_client);
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
        load_windows_root_certs(ssl_ctx);

        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), kApiHost)) {
            throw ApiError("SNI setup failed");
        }
        if (SSL_set1_host(stream.native_handle(), kApiHost) != 1) {
            throw ApiError("TLS hostname verification setup failed");
        }

        tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(kApiHost, kApiPort);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_sec));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, target, kHttp11Version};
        req.set(http::field::host, kApiHost);
        req.set(http::field::user_agent, "adi-pipeline-cpp/1.0");
        req.set(http::field::content_type, "application/json");
        req.body() = bj::serialize(req_json);
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(kMaxResponseBodyBytes);
        http::read(stream, buffer, parser);
        http::response<http::string_body> res = parser.release();

        beast::error_code ec;
        stream.shutdown(ec);

        const unsigned status = res.result_int();
        if (status == kHttpTooManyRequests) {
            throw RateLimitError("HTTP 429: " + res.body());
        }
        if (status < kHttpOkMin || status >= kHttpSuccessUpperBound) {
            throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
        }
        return adapt_gemini_response(bj::parse(res.body()));
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace adi::gemini
