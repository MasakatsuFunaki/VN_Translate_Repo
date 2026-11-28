// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "transport/beast_tls_transport.h"

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
#include <memory>
#include <optional>
#include <utility>

namespace vn::llm::detail {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr const char* kGeminiApiHost = "generativelanguage.googleapis.com";
constexpr const char* kGeminiApiPort = "443";
constexpr const char* kUserAgentHeader = "vn-llm-client/1.0";
constexpr const char* kJsonMimeType = "application/json";

constexpr int kHttpVersion11 = 11;
constexpr int kMinTimeoutSeconds = 1;
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

static void populate_windows_root_certs(asio::ssl::context& ssl_ctx) {
    const CertStoreGuard win_store(
        ::CertOpenSystemStoreW(kDefaultCryptProv, L"ROOT"));
    if (win_store.handle == nullptr) {
        throw LlmTransientError("CertOpenSystemStoreW(ROOT) failed");
    }

    X509StorePtr x509_store(X509_STORE_new());
    if (!x509_store) {
        throw LlmTransientError("X509_STORE_new failed");
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
        throw LlmTransientError(
            "Windows ROOT certificate store yielded 0 X.509 certificates");
    }

    SSL_CTX_set_cert_store(ssl_ctx.native_handle(), x509_store.release());
}

}  // namespace

struct BeastTlsHttpTransport::Impl {
    std::optional<asio::ssl::context> ssl_ctx;

    void ensure_ssl_context() {
        if (ssl_ctx.has_value()) {
            return;
        }
        asio::ssl::context ctx(asio::ssl::context::tls_client);
        ctx.set_verify_mode(asio::ssl::verify_peer);
        populate_windows_root_certs(ctx);
        ssl_ctx.emplace(std::move(ctx));
    }

    RawHttpResponse post_json(std::string_view target_path,
                              std::string_view json_body,
                              int connect_timeout_sec,
                              int read_timeout_sec) {
        ensure_ssl_context();

        const int effective_connect_s =
            std::max(kMinTimeoutSeconds, connect_timeout_sec);
        const int effective_read_s =
            std::max(kMinTimeoutSeconds, read_timeout_sec);

        asio::io_context ioc;
        beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), kGeminiApiHost)) {
            throw LlmTransientError("TLS SNI host setup failed");
        }
        if (SSL_set1_host(stream.native_handle(), kGeminiApiHost) !=
            kOpenSslSuccess) {
            throw LlmTransientError("TLS hostname verification setup failed");
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

        return RawHttpResponse{
            static_cast<int>(res.result_int()),
            std::move(res.body()),
        };
    }
};

BeastTlsHttpTransport::BeastTlsHttpTransport()
    : impl_(std::make_unique<Impl>()) {}

BeastTlsHttpTransport::~BeastTlsHttpTransport() = default;
BeastTlsHttpTransport::BeastTlsHttpTransport(BeastTlsHttpTransport&&) noexcept =
    default;
BeastTlsHttpTransport& BeastTlsHttpTransport::operator=(
    BeastTlsHttpTransport&&) noexcept = default;

RawHttpResponse BeastTlsHttpTransport::post_json(
    std::string_view target_path,
    std::string_view json_body,
    int connect_timeout_sec,
    int read_timeout_sec) {
    return impl_->post_json(target_path, json_body,
                            connect_timeout_sec, read_timeout_sec);
}

}  // namespace vn::llm::detail
