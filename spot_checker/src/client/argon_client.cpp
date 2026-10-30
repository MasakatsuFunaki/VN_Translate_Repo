// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include "common/util.h"

namespace spot_checker {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* API_HOST = "generativelanguage.googleapis.com";
constexpr const char* API_PORT = "443";
constexpr std::int64_t THINKING_BUDGET = 2048;
constexpr std::int64_t MAX_OUTPUT_TOKENS = 16384;

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

std::string read_key_from_dotenv(const fs::path& dotenv_path) {
    if (!fs::exists(dotenv_path)) return {};
    std::ifstream in(dotenv_path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("GEMINI_API_KEY=", 0) == 0) {
            std::string key = trim(line.substr(15));
            if (key.size() >= 2 &&
                ((key.front() == '"' && key.back() == '"') ||
                 (key.front() == '\'' && key.back() == '\''))) {
                key = key.substr(1, key.size() - 2);
            }
            if (!key.empty()) return key;
        }
    }
    return {};
}

}  // namespace

std::string load_gemini_api_key() {
    if (const char* env = std::getenv("GEMINI_API_KEY"); env && *env) {
        return env;
    }
    try {
        if (const char* home = std::getenv("USERPROFILE"); home && *home) {
            const fs::path cfg = fs::u8path(std::string(home) + "\\.gemini\\.env");
            if (std::string key = read_key_from_dotenv(cfg); !key.empty()) {
                _putenv_s("GEMINI_API_KEY", key.c_str());
                return key;
            }
        }
        const fs::path fallback_cfg = fs::u8path("C:\\Users\\btimo\\.gemini\\.env");
        if (std::string key = read_key_from_dotenv(fallback_cfg); !key.empty()) {
            _putenv_s("GEMINI_API_KEY", key.c_str());
            return key;
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
        {"maxOutputTokens", MAX_OUTPUT_TOKENS},
        {"thinkingConfig", bj::object{{"thinkingBudget", THINKING_BUDGET}}},
        {"responseMimeType", "application/json"},
    };
    return req_json;
}

ArgonResponse parse_argon_response_payload(const bj::value& raw) {
    ArgonResponse out;
    if (!raw.is_object()) return out;

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
                    if (!pv.is_object()) continue;
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
        auto get_int = [&](const char* k) -> std::int64_t {
            if (const auto* v = u.if_contains(k)) {
                if (v->is_int64()) return v->as_int64();
                if (v->is_uint64()) return static_cast<std::int64_t>(v->as_uint64());
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
            model = "gemini-4-argon";
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
            if (!SSL_set_tlsext_host_name(stream.native_handle(), API_HOST)) {
                throw ApiError("SNI setup failed");
            }
            SSL_set1_host(stream.native_handle(), API_HOST);

            tcp::resolver resolver(ioc);
            auto endpoints = resolver.resolve(API_HOST, API_PORT);
            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(timeout_sec));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(asio::ssl::stream_base::client);

            http::request<http::string_body> req{http::verb::post, target, 11};
            req.set(http::field::host, API_HOST);
            req.set(http::field::user_agent, "vn-spot-checker/1.0");
            req.set(http::field::content_type, "application/json");
            req.body() = bj::serialize(req_json);
            req.prepare_payload();

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(64ull * 1024 * 1024);
            http::read(stream, buffer, parser);
            http::response<http::string_body> res = parser.release();

            beast::error_code ec;
            stream.shutdown(ec);

            const unsigned status = res.result_int();
            if (status == 429) {
                throw RateLimitError("HTTP 429: " + res.body());
            }
            if (status < 200 || status >= 300) {
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
