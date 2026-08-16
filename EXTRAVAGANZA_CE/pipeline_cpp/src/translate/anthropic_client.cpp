// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "anthropic_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include "common/util.h"

namespace exc::anthropic {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* API_HOST = "api.anthropic.com";
constexpr const char* API_PORT = "443";
constexpr const char* API_PATH = "/v1/messages";
constexpr const char* API_VERSION = "2023-06-01";

// OpenSSL doesn't read the Windows certificate store on its own; import the
// ROOT store so peer verification of api.anthropic.com actually works.
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

}  // namespace

std::string load_api_key() {
    if (const char* env = std::getenv("ANTHROPIC_API_KEY"); env && *env)
        return env;
    try {
        const char* home = std::getenv("USERPROFILE");
        if (!home) return {};
        std::string cfg = std::string(home) + "\\.claude_vn\\settings.json";
        if (!std::filesystem::exists(std::filesystem::u8path(cfg))) return {};
        bj::value root = json_parse_file(cfg);
        std::string key;
        if (auto* k = root.get_object().if_contains("primaryApiKey"))
            if (k->is_string()) key = std::string(k->get_string());
        if (key.rfind("echo ", 0) == 0) key = key.substr(5);
        if (!key.empty()) {
            _putenv_s("ANTHROPIC_API_KEY", key.c_str());
            log_info("[OK] Loaded API key from local settings.json");
        }
        return key;
    } catch (...) {
        return {};
    }
}

Client& LazyClient::get() {
    if (!client_) {
        if (load_api_key().empty())
            throw ApiError("ANTHROPIC_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        client_.emplace();
    }
    return *client_;
}

Client::Client(std::string api_key) : api_key_(std::move(api_key)) {
    if (api_key_.empty())
        if (const char* env = std::getenv("ANTHROPIC_API_KEY"); env && *env)
            api_key_ = env;
    if (api_key_.empty())
        throw ApiError("ANTHROPIC_API_KEY not set");
}

boost::json::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        asio::io_context ioc;
        asio::ssl::context ssl_ctx(asio::ssl::context::tls_client);
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
        load_windows_root_certs(ssl_ctx);

        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), API_HOST))
            throw ApiError("SNI setup failed");
        SSL_set1_host(stream.native_handle(), API_HOST);

        tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(API_HOST, API_PORT);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_sec));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::post, API_PATH, 11};
        req.set(http::field::host, API_HOST);
        req.set(http::field::user_agent, "exc-pipeline-cpp/1.0");
        req.set(http::field::content_type, "application/json");
        req.set("x-api-key", api_key_);
        req.set("anthropic-version", API_VERSION);
        req.body() = bj::serialize(body);
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(64ull * 1024 * 1024);
        http::read(stream, buffer, parser);
        http::response<http::string_body> res = parser.release();

        beast::error_code ec;
        stream.shutdown(ec);  // best-effort; many servers just close

        const unsigned status = res.result_int();
        if (status == 429)
            throw RateLimitError("HTTP 429: " + res.body());
        if (status < 200 || status >= 300)
            throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
        return bj::parse(res.body());
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace exc::anthropic
