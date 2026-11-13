// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_client.h"
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
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace spot_checker {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* kApiHost = "generativelanguage.googleapis.com";
constexpr const char* kApiPort = "443";
constexpr const char* kDefaultArgonModel = "gemini-4-argon";
constexpr std::int64_t kThinkingBudget = 2048;
constexpr std::int64_t kMaxOutputTokens = 16384;
constexpr std::string_view kGeminiKeyPrefix = "GEMINI_API_KEY=";
constexpr std::size_t kMinQuotedLength = 2;
constexpr int kHttpVersion11 = 11;
constexpr std::uint64_t kMaxResponseBodyBytes = 64ULL * 1024ULL * 1024ULL;
constexpr unsigned kHttpTooManyRequests = 429;
constexpr unsigned kHttpOkMin = 200;
constexpr unsigned kHttpOkExclusiveMax = 300;

void load_windows_root_certs(asio::ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) {
        return;
    }
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

std::string read_key_from_dotenv(const fs::path& dotenv_path) {
    if (!fs::exists(dotenv_path)) {
        return {};
    }
    std::ifstream in(dotenv_path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind(kGeminiKeyPrefix, 0) == 0) {
            std::string key = trim(line.substr(kGeminiKeyPrefix.size()));
            if (key.size() >= kMinQuotedLength &&
                ((key.front() == '"' && key.back() == '"') ||
                 (key.front() == '\'' && key.back() == '\''))) {
                key = key.substr(1, key.size() - kMinQuotedLength);
            }
            if (!key.empty()) {
                return key;
            }
        }
    }
    return {};
}

bool set_wide_env_utf8(const wchar_t* name, std::string_view value_utf8) {
    if (name == nullptr || *name == L'\0') {
        return false;
    }
    if (value_utf8.empty()) {
        return ::_wputenv_s(name, L"") == 0;
    }
    if (value_utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int src_len = static_cast<int>(value_utf8.size());
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value_utf8.data(), src_len, nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    std::wstring wide_value(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value_utf8.data(), src_len,
        wide_value.data(), needed);
    if (written != needed) {
        return false;
    }
    return ::_wputenv_s(name, wide_value.c_str()) == 0;
}

}  // namespace

std::string load_gemini_api_key() {
    try {
        if (const wchar_t* env = _wgetenv(L"GEMINI_API_KEY");
            env != nullptr && *env != L'\0') {
            return wide_to_utf8(env);
        }
        if (const wchar_t* home = _wgetenv(L"USERPROFILE");
            home != nullptr && *home != L'\0') {
            const fs::path cfg = fs::u8path(wide_to_utf8(home)) / ".gemini" / ".env";
            if (std::string key = read_key_from_dotenv(cfg); !key.empty()) {
                if (!set_wide_env_utf8(L"GEMINI_API_KEY", key)) {
                    return {};
                }
                return key;
            }
        }
    } catch (...) {
        return {};
    }
    return {};
}

bj::object build_argon_request_payload(const std::string& system_prompt,
                                       const std::string& user_prompt) {
    bj::object req_json;
    if (!system_prompt.empty()) {
        req_json["systemInstruction"] = bj::object{
            {"parts", bj::array{bj::object{{"text", system_prompt}}}}};
    }
    req_json["contents"] = bj::array{
        bj::object{
            {"role", "user"},
            {"parts", bj::array{bj::object{{"text", user_prompt}}}},
        },
    };
    req_json["generationConfig"] = bj::object{
        {"maxOutputTokens", kMaxOutputTokens},
        {"thinkingConfig", bj::object{{"thinkingBudget", kThinkingBudget}}},
        {"responseMimeType", "application/json"},
    };
    return req_json;
}

ArgonResponse parse_argon_response_payload(const bj::value& raw) {
    ArgonResponse out;
    if (!raw.is_object()) {
        return out;
    }

    const auto& robj = raw.as_object();
    if (const auto* cands = robj.if_contains("candidates");
        cands && cands->is_array() && !cands->as_array().empty() &&
        cands->as_array()[0].is_object()) {
        const auto& cand0 = cands->as_array()[0].as_object();
        if (const auto* content = cand0.if_contains("content");
            content && content->is_object()) {
            if (const auto* parts = content->as_object().if_contains("parts");
                parts && parts->is_array()) {
                for (const auto& pv : parts->as_array()) {
                    if (!pv.is_object()) {
                        continue;
                    }
                    const auto& part = pv.as_object();
                    if (const auto* th = part.if_contains("thought");
                        th && th->is_bool() && th->as_bool()) {
                        continue;
                    }
                    if (const auto* txt = part.if_contains("text");
                        txt && txt->is_string()) {
                        out.text += std::string(txt->as_string());
                    }
                }
            }
        }
        if (const auto* fr = cand0.if_contains("finishReason"); fr && fr->is_string()) {
            out.finish_reason = std::string(fr->as_string());
        }
    }

    if (const auto* um = robj.if_contains("usageMetadata"); um && um->is_object()) {
        const auto& u = um->as_object();
        const auto get_int = [&](const char* k) -> std::int64_t {
            if (const auto* v = u.if_contains(k)) {
                if (v->is_int64()) {
                    return v->as_int64();
                }
                if (v->is_uint64()) {
                    return static_cast<std::int64_t>(v->as_uint64());
                }
            }
            return 0;
        };
        out.prompt_tokens = get_int("promptTokenCount");
        out.output_tokens = get_int("candidatesTokenCount");
        out.thought_tokens = get_int("thoughtsTokenCount");
    }

    return out;
}

struct ArgonClient::Impl {
    std::string api_key;
    std::string model;

    Impl(std::string key, std::string mdl)
        : api_key(std::move(key)), model(std::move(mdl)) {
        if (api_key.empty()) {
            api_key = load_gemini_api_key();
        }
        if (api_key.empty()) {
            throw ApiError("GEMINI_API_KEY not set in environment or ~/.gemini/.env");
        }
        if (model.empty()) {
            model = kDefaultArgonModel;
        }
    }

    ArgonResponse generate(const std::string& system_prompt,
                           const std::string& user_prompt,
                           int timeout_sec) {
        try {
            const bj::object req_json =
                build_argon_request_payload(system_prompt, user_prompt);
            const std::string target =
                "/v1beta/models/" + model + ":generateContent?key=" + api_key;

            asio::io_context ioc;
            asio::ssl::context ssl_ctx(asio::ssl::context::tls_client);
            ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
            load_windows_root_certs(ssl_ctx);

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), kApiHost)) {
                throw ApiError("SNI setup failed");
            }
            SSL_set1_host(stream.native_handle(), kApiHost);

            tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(kApiHost, kApiPort);
            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(timeout_sec));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(asio::ssl::stream_base::client);

            http::request<http::string_body> req{http::verb::post, target, kHttpVersion11};
            req.set(http::field::host, kApiHost);
            req.set(http::field::user_agent, "vn-spot-checker/1.0");
            req.set(http::field::content_type, "application/json");
            req.body() = bj::serialize(req_json);
            req.prepare_payload();

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(kMaxResponseBodyBytes);
            http::read(stream, buffer, parser);
            const http::response<http::string_body> res = parser.release();

            beast::error_code ec;
            stream.shutdown(ec);

            const unsigned status = res.result_int();
            if (status == kHttpTooManyRequests) {
                throw RateLimitError("HTTP 429: " + res.body());
            }
            if (status < kHttpOkMin || status >= kHttpOkExclusiveMax) {
                throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
            }
            return parse_argon_response_payload(bj::parse(res.body()));
        } catch (const RateLimitError&) {
            throw;
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& e) {
            throw ApiError(std::string("transport error: ") + e.what());
        }
    }
};

ArgonClient::ArgonClient(std::string api_key, std::string model)
    : impl_(std::make_unique<Impl>(std::move(api_key), std::move(model))) {}

ArgonClient::~ArgonClient() = default;
ArgonClient::ArgonClient(ArgonClient&& other) noexcept = default;
ArgonClient& ArgonClient::operator=(ArgonClient&& other) noexcept = default;

ArgonResponse ArgonClient::generate(const std::string& system_prompt,
                                    const std::string& user_prompt,
                                    int timeout_sec) {
    return impl_->generate(system_prompt, user_prompt, timeout_sec);
}

const std::string& ArgonClient::model() const {
    return impl_->model;
}

}  // namespace spot_checker
