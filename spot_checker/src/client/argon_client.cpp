// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_client.h"

#include "client/argon_wire_codec.h"
#include "common/utf8_text.h"
#include "ports/file_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

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

#ifdef X509_NAME
#undef X509_NAME
#endif
#ifdef X509_EXTENSIONS
#undef X509_EXTENSIONS
#endif
#ifdef X509_CERT_PAIR
#undef X509_CERT_PAIR
#endif
#ifdef PKCS7_SIGNER_INFO
#undef PKCS7_SIGNER_INFO
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace spot_checker {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

constexpr int kHttpVersion11 = 11;
constexpr int kRequestTimeoutSeconds = 180;
constexpr int kServerErrorMin = 500;
constexpr int kServerErrorMax = 599;
constexpr int kBackoffMultiplier = 2;
constexpr std::uint64_t kMaxResponseBodyBytes = 64ULL * 1024ULL * 1024ULL;
constexpr const char* kUserAgentHeader = "vn-spot-checker/1.0";
constexpr const char* kJsonMimeType = "application/json";
constexpr const char* kGeminiEnvVarName = "GEMINI_API_KEY";

struct CertStoreGuard {
    explicit CertStoreGuard(HCERTSTORE h) noexcept : handle(h) {}
    ~CertStoreGuard() {
        if (handle != nullptr) {
            ::CertCloseStore(handle, 0);
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

Status populate_windows_root_certs(asio::ssl::context& ssl_ctx) {
    const CertStoreGuard win_store(::CertOpenSystemStoreW(0, L"ROOT"));
    if (win_store.handle == nullptr) {
        return Status::error(ErrorCode::NetworkError,
                             "CertOpenSystemStoreW(ROOT) failed");
    }

    X509StorePtr x509_store(X509_STORE_new());
    if (!x509_store) {
        return Status::error(ErrorCode::NetworkError, "X509_STORE_new failed");
    }

    std::size_t loaded_count = 0;
    for (PCCERT_CONTEXT cert =
             ::CertEnumCertificatesInStore(win_store.handle, nullptr);
         cert != nullptr;
         cert = ::CertEnumCertificatesInStore(win_store.handle, cert)) {
        const unsigned char* encoded = cert->pbCertEncoded;
        X509CertPtr decoded(d2i_X509(
            nullptr, &encoded, static_cast<long>(cert->cbCertEncoded)));
        if (decoded) {
            if (X509_STORE_add_cert(x509_store.get(), decoded.get()) == 1) {
                ++loaded_count;
            }
        }
    }

    if (loaded_count == 0) {
        return Status::error(
            ErrorCode::NetworkError,
            "Windows ROOT certificate store yielded 0 X.509 certificates");
    }

    SSL_CTX_set_cert_store(ssl_ctx.native_handle(), x509_store.release());
    return Status::ok();
}

bool is_retryable_failure(const Status& status) noexcept {
    if (status.code == ErrorCode::RateLimited ||
        status.code == ErrorCode::NetworkError) {
        return true;
    }
    return status.code == ErrorCode::HttpError &&
           status.http_status >= kServerErrorMin &&
           status.http_status <= kServerErrorMax;
}

}  // namespace

struct ArgonClient::Impl {
    ArgonClientOptions options;
    SystemClock default_clock;
    NullLogSink default_log;
    std::optional<asio::ssl::context> ssl_ctx;

    explicit Impl(ArgonClientOptions opts) : options(std::move(opts)) {
        if (options.api_key.empty()) {
            options.api_key = ArgonClient::resolve_gemini_api_key();
        }
        if (options.model.empty()) {
            options.model = std::string(kDefaultArgonModel);
        }
    }

    IClock& clock() noexcept {
        return options.clock != nullptr ? *options.clock : default_clock;
    }

    ILogSink& log() noexcept {
        return options.log_sink != nullptr ? *options.log_sink : default_log;
    }

    Status ensure_ssl_context() {
        if (ssl_ctx.has_value()) {
            return Status::ok();
        }
        try {
            asio::ssl::context ctx(asio::ssl::context::tls_client);
            ctx.set_verify_mode(asio::ssl::verify_peer);
            const Status cert_status = populate_windows_root_certs(ctx);
            if (!cert_status.is_ok()) {
                return cert_status;
            }
            ssl_ctx.emplace(std::move(ctx));
            return Status::ok();
        } catch (const std::exception& ex) {
            return Status::error(
                ErrorCode::NetworkError,
                std::string("TLS context initialization failed: ") + ex.what());
        }
    }

    Result<LlmResponse> send_once(const std::string& request_body) {
        const Status tls_status = ensure_ssl_context();
        if (!tls_status.is_ok()) {
            return Result<LlmResponse>::failure(tls_status);
        }

        const std::string host(kDefaultArgonHost);
        const std::string port(kDefaultArgonPort);
        const std::string target = std::string(kDefaultArgonBasePath) + "/" +
                                   options.model +
                                   ":generateContent?key=" + options.api_key;

        try {
            asio::io_context ioc;
            beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                return Result<LlmResponse>::failure(
                    ErrorCode::NetworkError, "TLS SNI host setup failed");
            }
            if (SSL_set1_host(stream.native_handle(), host.c_str()) != 1) {
                return Result<LlmResponse>::failure(
                    ErrorCode::NetworkError,
                    "TLS hostname verification setup failed");
            }

            tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(host, port);
            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(kRequestTimeoutSeconds));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(asio::ssl::stream_base::client);

            http::request<http::string_body> req{http::verb::post, target,
                                                 kHttpVersion11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, kUserAgentHeader);
            req.set(http::field::content_type, kJsonMimeType);
            req.body() = request_body;
            req.prepare_payload();

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(kMaxResponseBodyBytes);
            http::read(stream, buffer, parser);
            const http::response<http::string_body> res = parser.release();

            beast::error_code shutdown_ec;
            stream.shutdown(shutdown_ec);

            return parse_argon_response_json(
                res.body(), static_cast<int>(res.result_int()));
        } catch (const std::exception& ex) {
            return Result<LlmResponse>::failure(
                ErrorCode::NetworkError,
                std::string("HTTPS transport error: ") + ex.what());
        }
    }

    Result<LlmResponse> generate(const LlmRequest& request) {
        if (options.api_key.empty()) {
            return Result<LlmResponse>::failure(
                ErrorCode::MissingApiKey,
                "GEMINI_API_KEY is not set in environment or ~/.gemini/.env");
        }

        const std::string request_body = build_argon_request_json(request);
        const int max_retries = std::max(0, options.max_retries);
        std::chrono::milliseconds backoff = options.initial_backoff;

        for (int attempt = 0;; ++attempt) {
            auto res = send_once(request_body);
            if (res.is_ok()) {
                return res;
            }
            if (attempt >= max_retries || !is_retryable_failure(res.status())) {
                return res;
            }
            log().warn("Argon API transient error (" +
                       std::string(to_string(res.error_code())) +
                       "), retrying after " +
                       std::to_string(backoff.count()) + "ms");
            clock().sleep_for(backoff);
            backoff *= kBackoffMultiplier;
        }
    }
};

ArgonClient::ArgonClient(ArgonClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ArgonClient::ArgonClient(std::string api_key, std::string model)
    : ArgonClient(ArgonClientOptions{std::move(api_key), std::move(model)}) {}

ArgonClient::~ArgonClient() = default;
ArgonClient::ArgonClient(ArgonClient&&) noexcept = default;
ArgonClient& ArgonClient::operator=(ArgonClient&&) noexcept = default;

Result<LlmResponse> ArgonClient::generate(const LlmRequest& request) {
    return impl_->generate(request);
}

std::string ArgonClient::resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override) {
    if (!dotenv_override.empty()) {
        const DiskFileStore store;
        const auto text_res = store.read_text(dotenv_override);
        if (!text_res.is_ok()) {
            return {};
        }
        return parse_dotenv_value(text_res.value(), kGeminiEnvVarName);
    }

    if (const wchar_t* env_val = ::_wgetenv(L"GEMINI_API_KEY");
        env_val != nullptr && *env_val != L'\0') {
        const std::string trimmed = trim_ascii(wide_to_utf8(env_val));
        if (!trimmed.empty()) {
            return trimmed;
        }
    }

    fs::path dotenv_path;
    if (const wchar_t* home = ::_wgetenv(L"USERPROFILE");
        home != nullptr && *home != L'\0') {
        dotenv_path = fs::path(home) / ".gemini" / ".env";
    } else if (const wchar_t* posix_home = ::_wgetenv(L"HOME");
               posix_home != nullptr && *posix_home != L'\0') {
        dotenv_path = fs::path(posix_home) / ".gemini" / ".env";
    }
    if (dotenv_path.empty()) {
        return {};
    }

    const DiskFileStore store;
    const auto text_res = store.read_text(dotenv_path);
    if (!text_res.is_ok()) {
        return {};
    }
    return parse_dotenv_value(text_res.value(), kGeminiEnvVarName);
}

}  // namespace spot_checker
