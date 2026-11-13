// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// HTTPS POST /v1beta/models/...:generateContent over Boost.Beast + OpenSSL.
//
// OpenSSL on Windows does not read the Windows root certificate store by
// default, so SSL_CTX_set_default_verify_paths leaves an empty trust list and
// every handshake fails. load_windows_root_certs walks the system "ROOT" store
// via CryptoAPI and feeds each DER certificate into OpenSSL's X509_STORE.
#include "cache/gemini_client.h"

#include "common/util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <windows.h>
#include <wincrypt.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exm::gemini {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = boost::asio::ip::tcp;

namespace {

constexpr const char* kGeminiHost = "generativelanguage.googleapis.com";
constexpr const char* kGeminiPort = "443";
constexpr int kHttpVersion11 = 11;
constexpr int kHttpOkStatus = 200;
constexpr int kHttpRateLimitStatus = 429;
constexpr std::size_t kGeminiEnvPrefixLen = 15;
constexpr std::size_t kMinQuotedValueLen = 2;
constexpr std::size_t kFirstUnquotedOffset = 1;
constexpr std::size_t kSurroundingQuoteChars = 2;

std::optional<std::string> wide_env_to_utf8(const wchar_t* name) {
    const wchar_t* val = _wgetenv(name);
    if (val == nullptr || *val == L'\0') {
        return std::nullopt;
    }
    const std::wstring_view wide(val);
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return std::nullopt;
    }
    return utf8;
}

class ScopedCertStore {
public:
    explicit ScopedCertStore(HCERTSTORE h) noexcept : handle_(h) {}
    ~ScopedCertStore() {
        if (handle_ != nullptr) {
            CertCloseStore(handle_, 0);
        }
    }

    ScopedCertStore(const ScopedCertStore&) = delete;
    ScopedCertStore& operator=(const ScopedCertStore&) = delete;

    HCERTSTORE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HCERTSTORE handle_;
};

struct X509Deleter {
    void operator()(X509* p) const noexcept {
        if (p != nullptr) {
            X509_free(p);
        }
    }
};
using ScopedX509 = std::unique_ptr<X509, X509Deleter>;

void load_windows_root_certs(ssl::context& ctx) {
    const ScopedCertStore store(CertOpenSystemStoreW(0, L"ROOT"));
    if (!store) {
        return;
    }

    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
    if (x509_store == nullptr) {
        return;
    }

    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store.get(), cert)) != nullptr) {
        const unsigned char* der = cert->pbCertEncoded;
        ScopedX509 x509(
            d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded)));
        if (x509) {
            X509_STORE_add_cert(x509_store, x509.get());
        }
    }
}

class HttpsClient final : public Client {
public:
    explicit HttpsClient(std::string api_key) : key_(std::move(api_key)) {
        if (key_.empty()) {
            if (const char* env = std::getenv("GEMINI_API_KEY")) {
                key_ = env;
            }
        }
        if (key_.empty()) {
            throw std::runtime_error(
                "GEMINI_API_KEY is not set (checked environment, .claude/settings.json, and ~/.gemini/.env)");
        }
    }

    bj::value generate_content(const std::string& model, const bj::value& body) override {
        net::io_context ioc;
        ssl::context sctx(ssl::context::tls_client);
        load_windows_root_certs(sctx);
        sctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, sctx);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), kGeminiHost)) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()),
                                 net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }
        SSL_set1_host(stream.native_handle(), kGeminiHost);

        const auto results = resolver.resolve(kGeminiHost, kGeminiPort);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        const std::string payload = bj::serialize(body);
        const std::string target =
            "/v1beta/models/" + model + ":generateContent?key=" + key_;

        http::request<http::string_body> req{http::verb::post, target, kHttpVersion11};
        req.set(http::field::host, kGeminiHost);
        req.set(http::field::user_agent, "extra_va_mizuna_ce_pipeline_cpp/1.0");
        req.set(http::field::content_type, "application/json");
        req.body() = payload;
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
        http::read(stream, buffer, parser);
        auto res = parser.release();

        beast::error_code ec;
        stream.shutdown(ec);

        const int status = static_cast<int>(res.result_int());
        if (status == kHttpRateLimitStatus) {
            throw RateLimitError(res.body());
        }
        if (status != kHttpOkStatus) {
            throw ApiError(status, res.body());
        }

        return bj::parse(res.body());
    }

private:
    std::string key_;
};

}  // namespace

Client::~Client() = default;

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
    if (const char* existing = std::getenv("GEMINI_API_KEY")) {
        if (*existing != '\0') {
            return;
        }
    }
    std::vector<fs::path> candidates = {
        fs::u8path(project_dir).parent_path() / ".claude" / "settings.json",
    };
    std::optional<std::string> home = wide_env_to_utf8(L"USERPROFILE");
    if (!home) {
        home = wide_env_to_utf8(L"HOME");
    }
    if (home) {
        const fs::path home_path = fs::u8path(*home);
        candidates.push_back(home_path / ".claude_vn" / "settings.json");
        candidates.push_back(home_path / ".gemini" / ".env");
    }

    for (const auto& p : candidates) {
        if (!fs::exists(p)) {
            continue;
        }
        try {
            if (p.filename() == ".env") {
                std::ifstream f(p);
                std::string line;
                while (std::getline(f, line)) {
                    line = trim(line);
                    if (line.rfind("GEMINI_API_KEY=", 0) == 0) {
                        std::string k = trim(line.substr(kGeminiEnvPrefixLen));
                        if (k.size() >= kMinQuotedValueLen &&
                            ((k.front() == '"' && k.back() == '"') ||
                             (k.front() == '\'' && k.back() == '\''))) {
                            k = k.substr(kFirstUnquotedOffset,
                                         k.size() - kSurroundingQuoteChars);
                        }
                        if (!k.empty()) {
                            _putenv_s("GEMINI_API_KEY", k.c_str());
                            return;
                        }
                    }
                }
                continue;
            }
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
                _putenv_s("GEMINI_API_KEY", std::string(key->get_string()).c_str());
                return;
            }
        } catch (const std::exception&) {
        }
    }
}

}  // namespace exm::gemini
