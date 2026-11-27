// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate/gemini_client.h"

#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

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
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace crc::translate {

ITranslationTransport::~ITranslationTransport() = default;

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

constexpr const char* kApiHost = "generativelanguage.googleapis.com";
constexpr const char* kApiPort = "443";
constexpr const char* kApiBasePath = "/v1beta/models";
constexpr const char* kUserAgentHeader = "crc-pipeline-cpp/1.0";
constexpr const char* kJsonMimeType = "application/json";
constexpr const char* kGeminiEnvVarName = "GEMINI_API_KEY";

constexpr int kHttpVersion11 = 11;
constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpServerErrorMin = 500;
constexpr int kHttpServerErrorMax = 599;
constexpr int kBackoffMultiplier = 2;
constexpr std::int64_t kMinOutputTokens = 1;
constexpr std::size_t kFirstCandidateIndex = 0;
constexpr std::uint64_t kMaxResponseBodyBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kRegistryKeyBufferChars = 512;

constexpr std::string_view kResourceExhaustedStatus = "RESOURCE_EXHAUSTED";
constexpr const char* kSafetyThresholdBlockNone = "BLOCK_NONE";
constexpr std::string_view kExportPrefix = "export";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

constexpr std::array<const char*, 5> kConfigurableHarmCategories = {
    "HARM_CATEGORY_HARASSMENT",
    "HARM_CATEGORY_HATE_SPEECH",
    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
    "HARM_CATEGORY_DANGEROUS_CONTENT",
    "HARM_CATEGORY_CIVIC_INTEGRITY",
};

constexpr std::array<std::string_view, 6> kBlockedFinishReasons = {
    "SAFETY",
    "PROHIBITED_CONTENT",
    "BLOCKLIST",
    "RECITATION",
    "SPII",
    "IMAGE_SAFETY",
};

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

bool is_ascii_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view trim_ascii_view(std::string_view s) noexcept {
    std::size_t first = 0;
    while (first < s.size() && is_ascii_space(s[first])) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && is_ascii_space(s[last - 1U])) {
        --last;
    }
    return s.substr(first, last - first);
}

std::int64_t extract_json_int64(const bj::object& obj, const char* key) noexcept {
    const auto* val = obj.if_contains(key);
    if (val == nullptr) {
        return 0;
    }
    if (val->is_int64()) {
        return val->as_int64();
    }
    if (val->is_uint64()) {
        const std::uint64_t u = val->as_uint64();
        constexpr auto kMaxInt64 =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return static_cast<std::int64_t>(u > kMaxInt64 ? kMaxInt64 : u);
    }
    return 0;
}

void populate_usage_metadata(const bj::object& root_obj,
                             GeminiResponse& out) noexcept {
    const auto* usage = root_obj.if_contains("usageMetadata");
    if (usage == nullptr || !usage->is_object()) {
        return;
    }
    const auto& u = usage->as_object();
    out.prompt_tokens = extract_json_int64(u, "promptTokenCount");
    out.cached_tokens = extract_json_int64(u, "cachedContentTokenCount");
    out.output_tokens = extract_json_int64(u, "candidatesTokenCount");
    out.thought_tokens = extract_json_int64(u, "thoughtsTokenCount");
}

std::string extract_prompt_block_reason(const bj::object& root_obj) {
    const auto* feedback = root_obj.if_contains("promptFeedback");
    if (feedback == nullptr || !feedback->is_object()) {
        return {};
    }
    const auto* reason = feedback->as_object().if_contains("blockReason");
    if (reason == nullptr || !reason->is_string()) {
        return {};
    }
    const std::string_view sv(reason->as_string().data(),
                              reason->as_string().size());
    if (trim_ascii_view(sv).empty()) {
        return {};
    }
    return std::string(sv);
}

bool is_blocked_finish_reason(std::string_view reason) noexcept {
    return std::any_of(kBlockedFinishReasons.begin(),
                       kBlockedFinishReasons.end(),
                       [reason](std::string_view candidate) noexcept {
                           return candidate == reason;
                       });
}

std::string parse_double_quoted_dotenv(std::string_view rhs) {
    std::string out;
    out.reserve(rhs.size());
    for (std::size_t i = 1; i < rhs.size(); ++i) {
        const char ch = rhs[i];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && i + 1U < rhs.size()) {
            const char next = rhs[++i];
            switch (next) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default:
                out += '\\';
                out += next;
                break;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

std::string parse_dotenv_rhs(std::string_view rhs) {
    rhs = trim_ascii_view(rhs);
    if (rhs.empty()) {
        return {};
    }
    if (rhs.front() == '\'') {
        const std::size_t close_quote = rhs.find('\'', 1);
        if (close_quote != std::string_view::npos) {
            return std::string(rhs.substr(1, close_quote - 1U));
        }
        return std::string(rhs.substr(1));
    }
    if (rhs.front() == '"') {
        return parse_double_quoted_dotenv(rhs);
    }

    const std::size_t hash_pos = rhs.find('#');
    if (hash_pos != std::string_view::npos) {
        rhs = rhs.substr(0, hash_pos);
    }
    return std::string(trim_ascii_view(rhs));
}

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int src_len = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), src_len,
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return out;
}

std::string read_dotenv_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void populate_windows_root_certs(asio::ssl::context& ssl_ctx) {
    const CertStoreGuard win_store(::CertOpenSystemStoreW(0, L"ROOT"));
    if (win_store.handle == nullptr) {
        throw TransientApiError("CertOpenSystemStoreW(ROOT) failed");
    }

    X509StorePtr x509_store(X509_STORE_new());
    if (!x509_store) {
        throw TransientApiError("X509_STORE_new failed");
    }

    std::size_t loaded = 0;
    for (PCCERT_CONTEXT cert =
             ::CertEnumCertificatesInStore(win_store.handle, nullptr);
         cert != nullptr;
         cert = ::CertEnumCertificatesInStore(win_store.handle, cert)) {
        const unsigned char* encoded = cert->pbCertEncoded;
        X509CertPtr decoded(d2i_X509(
            nullptr, &encoded, static_cast<long>(cert->cbCertEncoded)));
        if (decoded && X509_STORE_add_cert(x509_store.get(), decoded.get()) == 1) {
            ++loaded;
        }
    }

    if (loaded == 0) {
        throw TransientApiError(
            "Windows ROOT certificate store yielded 0 X.509 certificates");
    }

    SSL_CTX_set_cert_store(ssl_ctx.native_handle(), x509_store.release());
}

}  // namespace

std::string build_gemini_request_json(const GeminiRequest& request) {
    bj::object root;
    if (!request.system_prompt.empty()) {
        root["systemInstruction"] = bj::object{
            {"parts", bj::array{bj::object{{"text", request.system_prompt}}}},
        };
    }

    root["contents"] = bj::array{
        bj::object{
            {"role", "user"},
            {"parts", bj::array{bj::object{{"text", request.user_prompt}}}},
        },
    };

    if (request.safety_block_none) {
        bj::array safety_settings;
        safety_settings.reserve(kConfigurableHarmCategories.size());
        for (const char* category : kConfigurableHarmCategories) {
            safety_settings.emplace_back(bj::object{
                {"category", category},
                {"threshold", kSafetyThresholdBlockNone},
            });
        }
        root["safetySettings"] = std::move(safety_settings);
    }

    bj::object gen_cfg;
    const std::int64_t clamped_max = std::clamp<std::int64_t>(
        request.max_output_tokens, kMinOutputTokens, kGeminiMaxOutputTokens);
    gen_cfg["maxOutputTokens"] = clamped_max;

    const std::string_view level = trim_ascii_view(request.thinking_level);
    if (!level.empty()) {
        gen_cfg["thinkingConfig"] = bj::object{
            {"thinkingLevel", std::string(level)},
        };
    } else if (request.thinking_budget >= 0) {
        gen_cfg["thinkingConfig"] = bj::object{
            {"thinkingBudget", request.thinking_budget},
        };
    }

    root["generationConfig"] = std::move(gen_cfg);
    return bj::serialize(root);
}

GeminiResponse parse_gemini_response_json(std::string_view raw_json,
                                          int http_status) {
    boost::system::error_code parse_ec;
    bj::value parsed;
    if (!raw_json.empty()) {
        parsed = bj::parse(bj::string_view(raw_json.data(), raw_json.size()),
                           parse_ec);
    }

    std::string api_err_status;
    std::string api_err_message;
    if (!parse_ec && parsed.is_object()) {
        if (const auto* err_val = parsed.as_object().if_contains("error");
            err_val != nullptr && err_val->is_object()) {
            const auto& err_obj = err_val->as_object();
            if (const auto* st = err_obj.if_contains("status");
                st != nullptr && st->is_string()) {
                api_err_status = std::string(st->as_string());
            }
            if (const auto* msg = err_obj.if_contains("message");
                msg != nullptr && msg->is_string()) {
                api_err_message = std::string(msg->as_string());
            }
        }
    }

    if (http_status == kHttpTooManyRequests ||
        api_err_status == kResourceExhaustedStatus) {
        const std::string detail =
            !api_err_message.empty()
                ? api_err_message
                : ("rate limited (HTTP " + std::to_string(http_status) + ")");
        throw RateLimitError(detail, http_status);
    }

    if (http_status >= kHttpServerErrorMin && http_status <= kHttpServerErrorMax) {
        const std::string detail =
            !api_err_message.empty()
                ? ("HTTP " + std::to_string(http_status) + ": " + api_err_message)
                : ("HTTP " + std::to_string(http_status));
        throw TransientApiError(detail, http_status);
    }

    if (http_status < kHttpOkMin || http_status > kHttpOkMax) {
        const std::string detail =
            !api_err_message.empty()
                ? ("HTTP " + std::to_string(http_status) + ": " + api_err_message)
                : ("HTTP " + std::to_string(http_status));
        throw ApiError(detail, http_status);
    }

    if (raw_json.empty() || parse_ec || !parsed.is_object()) {
        throw ApiError(
            parse_ec ? ("malformed response JSON: " + parse_ec.message())
                     : "response body is empty or not a JSON object",
            http_status);
    }

    const auto& root_obj = parsed.as_object();
    GeminiResponse out;
    populate_usage_metadata(root_obj, out);

    const auto* cands = root_obj.if_contains("candidates");
    if (cands == nullptr || !cands->is_array() || cands->as_array().empty() ||
        !cands->as_array()[kFirstCandidateIndex].is_object()) {
        std::string block_reason = extract_prompt_block_reason(root_obj);
        if (!block_reason.empty()) {
            out.finish_reason = std::move(block_reason);
            out.content_blocked = true;
            return out;
        }
        throw ApiError("response JSON missing non-empty candidates array",
                       http_status);
    }

    const auto& cand0 = cands->as_array()[kFirstCandidateIndex].as_object();
    if (const auto* content = cand0.if_contains("content");
        content != nullptr && content->is_object()) {
        if (const auto* parts = content->as_object().if_contains("parts");
            parts != nullptr && parts->is_array()) {
            for (const auto& part_val : parts->as_array()) {
                if (!part_val.is_object()) {
                    continue;
                }
                const auto& part_obj = part_val.as_object();
                if (const auto* thought = part_obj.if_contains("thought");
                    thought != nullptr && thought->is_bool() &&
                    thought->as_bool()) {
                    continue;
                }
                if (const auto* txt = part_obj.if_contains("text");
                    txt != nullptr && txt->is_string()) {
                    out.text.append(txt->as_string().data(),
                                    txt->as_string().size());
                }
            }
        }
    }

    if (const auto* fr = cand0.if_contains("finishReason");
        fr != nullptr && fr->is_string()) {
        out.finish_reason = std::string(fr->as_string());
    }

    if (is_blocked_finish_reason(out.finish_reason)) {
        out.content_blocked = true;
        return out;
    }

    if (trim_ascii_view(out.text).empty()) {
        std::string block_reason = extract_prompt_block_reason(root_obj);
        if (!block_reason.empty()) {
            out.finish_reason = std::move(block_reason);
            out.content_blocked = true;
            return out;
        }
        throw ApiError("response candidate contains no non-thought text output",
                       http_status);
    }

    return out;
}

std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name) {
    key_name = trim_ascii_view(key_name);
    if (key_name.empty()) {
        return {};
    }

    if (dotenv_contents.size() >= kUtf8Bom.size() &&
        dotenv_contents.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        dotenv_contents.remove_prefix(kUtf8Bom.size());
    }

    std::string resolved;
    std::size_t pos = 0;
    while (pos <= dotenv_contents.size()) {
        const std::size_t nl = dotenv_contents.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos)
                ? dotenv_contents.substr(pos)
                : dotenv_contents.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? (dotenv_contents.size() + 1U)
                                             : (nl + 1U);

        line = trim_ascii_view(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.size() > kExportPrefix.size() &&
            line.substr(0, kExportPrefix.size()) == kExportPrefix &&
            (line[kExportPrefix.size()] == ' ' ||
             line[kExportPrefix.size()] == '\t')) {
            line = trim_ascii_view(line.substr(kExportPrefix.size()));
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view lhs = trim_ascii_view(line.substr(0, eq));
        if (lhs != key_name) {
            continue;
        }
        resolved = parse_dotenv_rhs(line.substr(eq + 1U));
    }
    return resolved;
}

struct GeminiClient::Impl {
    GeminiClientOptions options;
    std::optional<asio::ssl::context> ssl_ctx;

    explicit Impl(GeminiClientOptions opts) : options(std::move(opts)) {
        if (options.api_key.empty()) {
            options.api_key = GeminiClient::resolve_gemini_api_key();
        }
        if (options.model.empty()) {
            options.model = std::string(kDefaultGeminiModel);
        }
        if (options.timeout_seconds <= 0) {
            options.timeout_seconds = kDefaultRequestTimeoutSeconds;
        }
    }

    void sleep_for(std::chrono::milliseconds dur) const {
        if (options.sleep_fn) {
            options.sleep_fn(dur);
            return;
        }
        std::this_thread::sleep_for(dur);
    }

    void ensure_ssl_context() {
        if (ssl_ctx.has_value()) {
            return;
        }
        asio::ssl::context ctx(asio::ssl::context::tls_client);
        ctx.set_verify_mode(asio::ssl::verify_peer);
        populate_windows_root_certs(ctx);
        ssl_ctx.emplace(std::move(ctx));
    }

    GeminiResponse send_once(const std::string& request_body) {
        ensure_ssl_context();

        const std::string target = std::string(kApiBasePath) + "/" +
                                   options.model +
                                   ":generateContent?key=" + options.api_key;
        try {
            asio::io_context ioc;
            beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), kApiHost)) {
                throw TransientApiError("TLS SNI host setup failed");
            }
            if (SSL_set1_host(stream.native_handle(), kApiHost) != 1) {
                throw TransientApiError("TLS hostname verification setup failed");
            }

            tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(kApiHost, kApiPort);
            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(options.timeout_seconds));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(asio::ssl::stream_base::client);

            http::request<http::string_body> req{http::verb::post, target,
                                                 kHttpVersion11};
            req.set(http::field::host, kApiHost);
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

            return parse_gemini_response_json(
                res.body(), static_cast<int>(res.result_int()));
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            if (!options.api_key.empty()) {
                for (std::size_t pos = 0;
                     (pos = msg.find(options.api_key, pos)) != std::string::npos;) {
                    msg.replace(pos, options.api_key.size(), "<redacted>");
                }
            }
            throw TransientApiError("HTTPS transport error: " + msg);
        }
    }

    GeminiResponse generate(const GeminiRequest& request) {
        if (options.api_key.empty()) {
            throw ApiError(
                "GEMINI_API_KEY is not set in environment, ~/.gemini/.env, or HKCU\\Environment");
        }

        const std::string request_body = build_gemini_request_json(request);
        const int max_retries = std::max(0, options.max_retries);
        std::chrono::milliseconds backoff =
            std::max(std::chrono::milliseconds{0}, options.initial_backoff);

        for (int attempt = 0;; ++attempt) {
            try {
                return send_once(request_body);
            } catch (const RateLimitError& ex) {
                if (attempt >= max_retries) {
                    throw;
                }
                log_info("    Rate limited (" + std::string(ex.what()) +
                         "), retrying in " + std::to_string(backoff.count()) + "ms...");
                sleep_for(backoff);
                backoff *= kBackoffMultiplier;
            } catch (const TransientApiError& ex) {
                if (attempt >= max_retries) {
                    throw;
                }
                log_info("    Transient API error (" + std::string(ex.what()) +
                         "), retrying in " + std::to_string(backoff.count()) + "ms...");
                sleep_for(backoff);
                backoff *= kBackoffMultiplier;
            }
        }
    }
};

GeminiClient::GeminiClient(GeminiClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

GeminiClient::~GeminiClient() = default;
GeminiClient::GeminiClient(GeminiClient&&) noexcept = default;
GeminiClient& GeminiClient::operator=(GeminiClient&&) noexcept = default;

GeminiResponse GeminiClient::generate(const GeminiRequest& request) {
    return impl_->generate(request);
}

std::string GeminiClient::resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override) {
    if (!dotenv_override.empty()) {
        return parse_dotenv_value(read_dotenv_file(dotenv_override),
                                  kGeminiEnvVarName);
    }

    if (const wchar_t* env_val = ::_wgetenv(L"GEMINI_API_KEY");
        env_val != nullptr && *env_val != L'\0') {
        const std::string val = trim(wide_to_utf8_strict(env_val));
        if (!val.empty()) {
            return val;
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
    if (!dotenv_path.empty()) {
        const std::string key = parse_dotenv_value(read_dotenv_file(dotenv_path),
                                                   kGeminiEnvVarName);
        if (!key.empty()) {
            return key;
        }
    }

    wchar_t reg_buf[kRegistryKeyBufferChars]{};
    DWORD reg_sz = sizeof(reg_buf);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"GEMINI_API_KEY",
                       RRF_RT_REG_SZ, nullptr, reg_buf, &reg_sz) == ERROR_SUCCESS) {
        const std::string key = trim(wide_to_utf8_strict(reg_buf));
        if (!key.empty()) {
            return key;
        }
    }
    return {};
}

}  // namespace crc::translate
