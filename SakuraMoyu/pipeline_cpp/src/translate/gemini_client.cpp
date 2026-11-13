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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fav::gemini {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* kApiHost = "generativelanguage.googleapis.com";
constexpr const char* kApiPort = "443";
constexpr const char* kDefaultModel = "gemini-4-argon";
constexpr std::int64_t kThinkingBudget = 2048;
constexpr std::int64_t kMaxOutputTokens = 16384;
constexpr std::size_t kMinQuotedValueSize = 2;
constexpr std::size_t kLeadingQuoteChars = 1;
constexpr std::size_t kQuotePairCount = 2;
constexpr int kHttpVersion11 = 11;
constexpr std::uint64_t kBytesPerKilobyte = 1024ull;
constexpr std::uint64_t kMaxResponseBodyMegabytes = 64ull;
constexpr std::uint64_t kMaxResponseBodyBytes =
    kMaxResponseBodyMegabytes * kBytesPerKilobyte * kBytesPerKilobyte;
constexpr unsigned int kHttpStatusOkMin = 200u;
constexpr unsigned int kHttpStatusOkEnd = 300u;
constexpr unsigned int kHttpStatusTooManyRequests = 429u;
constexpr std::size_t kFirstCandidateIndex = 0;
constexpr std::int64_t kZeroTokens = 0;

std::string wide_to_utf8_strict(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-16 sequence in environment variable");
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        utf8.data(),
        needed,
        nullptr,
        nullptr);
    if (written != needed) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return utf8;
}

std::wstring utf8_to_wide_strict(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (needed <= 0) {
        throw std::runtime_error("Invalid UTF-8 sequence in environment value");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8.data(),
        static_cast<int>(utf8.size()),
        wide.data(),
        needed);
    if (written != needed) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return wide;
}

// OpenSSL doesn't read the Windows certificate store on its own; import the
// ROOT store so peer verification of generativelanguage.googleapis.com works.
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

}  // namespace

std::string load_api_key() {
    if (const wchar_t* env = _wgetenv(L"GEMINI_API_KEY");
        env != nullptr && *env != L'\0') {
        return wide_to_utf8_strict(env);
    }
    try {
        const wchar_t* whome = _wgetenv(L"USERPROFILE");
        if (whome == nullptr || *whome == L'\0') {
            return {};
        }
        const std::string home_utf8 = wide_to_utf8_strict(whome);
        const std::filesystem::path cfg =
            std::filesystem::u8path(home_utf8) / ".gemini" / ".env";
        if (!std::filesystem::exists(cfg)) {
            return {};
        }
        std::ifstream f(cfg);
        if (!f) {
            return {};
        }
        std::string line;
        constexpr std::string_view kKeyPrefix = "GEMINI_API_KEY=";
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.rfind(kKeyPrefix.data(), 0) != 0) {
                continue;
            }
            std::string key = trim(line.substr(kKeyPrefix.size()));
            if (key.size() >= kMinQuotedValueSize &&
                ((key.front() == '"' && key.back() == '"') ||
                 (key.front() == '\'' && key.back() == '\''))) {
                key = trim(key.substr(kLeadingQuoteChars, key.size() - kQuotePairCount));
            }
            if (!key.empty()) {
                const std::wstring wkey = utf8_to_wide_strict(key);
                _wputenv_s(L"GEMINI_API_KEY", wkey.c_str());
                log_info("[OK] Loaded GEMINI_API_KEY from ~/.gemini/.env");
                return key;
            }
        }
        return {};
    } catch (...) {
        return {};
    }
}

Client& LazyClient::get() {
    if (!client_) {
        if (load_api_key().empty()) {
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        }
        client_.emplace();
    }
    return *client_;
}

struct Client::Impl {
    std::string api_key;

    explicit Impl(std::string key) : api_key(std::move(key)) {
        if (api_key.empty()) {
            api_key = load_api_key();
        }
        if (api_key.empty()) {
            throw ApiError("GEMINI_API_KEY not set");
        }
    }
};

Client::Client(std::string api_key)
    : impl_(std::make_unique<Impl>(std::move(api_key))) {}

Client::~Client() = default;
Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

boost::json::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        const bj::object empty_obj;
        const bj::object& in = body.is_object() ? body.get_object() : empty_obj;

        const std::string model = kDefaultModel;

        bj::object req_json;

        if (const auto* sys = in.if_contains("system")) {
            std::string sys_text;
            if (sys->is_string()) {
                sys_text = std::string(sys->get_string());
            } else if (sys->is_array()) {
                for (const auto& item : sys->get_array()) {
                    if (!item.is_object()) {
                        continue;
                    }
                    if (const auto* t = item.get_object().if_contains("text");
                        t && t->is_string()) {
                        if (!sys_text.empty()) {
                            sys_text += "\n";
                        }
                        sys_text += std::string(t->get_string());
                    }
                }
            }
            if (!sys_text.empty()) {
                req_json["systemInstruction"] = bj::object{
                    {"parts", bj::array{bj::object{{"text", sys_text}}}}
                };
            }
        }

        bj::array contents;
        if (const auto* msgs = in.if_contains("messages"); msgs && msgs->is_array()) {
            for (const auto& mv : msgs->get_array()) {
                if (!mv.is_object()) {
                    continue;
                }
                const auto& mobj = mv.get_object();
                std::string role = "user";
                if (const auto* r = mobj.if_contains("role");
                    r && r->is_string() && r->get_string() == "assistant") {
                    role = "model";
                }

                bj::array parts;
                if (const auto* c = mobj.if_contains("content")) {
                    if (c->is_string()) {
                        parts.push_back(bj::object{{"text", std::string(c->get_string())}});
                    } else if (c->is_array()) {
                        for (const auto& bv : c->get_array()) {
                            if (!bv.is_object()) {
                                continue;
                            }
                            const auto& block = bv.get_object();
                            std::string btype;
                            if (const auto* bt = block.if_contains("type");
                                bt && bt->is_string()) {
                                btype = std::string(bt->get_string());
                            }
                            if (btype == "text") {
                                if (const auto* txt = block.if_contains("text");
                                    txt && txt->is_string()) {
                                    parts.push_back(
                                        bj::object{{"text", std::string(txt->get_string())}});
                                }
                            } else if (btype == "image") {
                                if (const auto* src = block.if_contains("source");
                                    src && src->is_object()) {
                                    const auto& src_obj = src->get_object();
                                    const auto* mt = src_obj.if_contains("media_type");
                                    const auto* dt = src_obj.if_contains("data");
                                    if (mt && mt->is_string() && dt && dt->is_string()) {
                                        parts.push_back(bj::object{
                                            {"inlineData",
                                             bj::object{
                                                 {"mimeType", std::string(mt->get_string())},
                                                 {"data", std::string(dt->get_string())}
                                             }}
                                        });
                                    }
                                }
                            }
                        }
                    }
                }
                contents.push_back(bj::object{
                    {"role", role},
                    {"parts", std::move(parts)}
                });
            }
        }
        req_json["contents"] = std::move(contents);

        bj::object gen_cfg;
        std::int64_t max_tokens = kMaxOutputTokens;
        if (const auto* mt = in.if_contains("max_tokens")) {
            if (mt->is_int64()) {
                max_tokens = mt->get_int64();
            } else if (mt->is_uint64()) {
                max_tokens = static_cast<std::int64_t>(mt->get_uint64());
            }
        }
        max_tokens = std::min<std::int64_t>(max_tokens, kMaxOutputTokens);
        gen_cfg["maxOutputTokens"] = max_tokens;
        if (const auto* temp = in.if_contains("temperature")) {
            gen_cfg["temperature"] = *temp;
        }
        gen_cfg["thinkingConfig"] = bj::object{{"thinkingBudget", kThinkingBudget}};
        req_json["generationConfig"] = std::move(gen_cfg);

        bj::array safety_settings;
        for (const char* cat : {"HARM_CATEGORY_HARASSMENT",
                                "HARM_CATEGORY_HATE_SPEECH",
                                "HARM_CATEGORY_SEXUALLY_EXPLICIT",
                                "HARM_CATEGORY_DANGEROUS_CONTENT",
                                "HARM_CATEGORY_CIVIC_INTEGRITY"}) {
            safety_settings.push_back(bj::object{
                {"category", cat},
                {"threshold", "BLOCK_NONE"}
            });
        }
        req_json["safetySettings"] = std::move(safety_settings);

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
        auto endpoints = resolver.resolve(kApiHost, kApiPort);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_sec));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);

        const std::string target =
            "/v1beta/models/" + model + ":generateContent?key=" + impl_->api_key;
        http::request<http::string_body> req{http::verb::post, target, kHttpVersion11};
        req.set(http::field::host, kApiHost);
        req.set(http::field::user_agent, "fav-pipeline-cpp/1.0");
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
        stream.shutdown(ec);  // best-effort; many servers just close

        const unsigned int status = res.result_int();
        if (status == kHttpStatusTooManyRequests) {
            throw RateLimitError("HTTP 429: " + res.body());
        }
        if (status < kHttpStatusOkMin || status >= kHttpStatusOkEnd) {
            throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
        }

        bj::value gem_val = bj::parse(res.body());
        const bj::object& gem = gem_val.as_object();

        std::string text_out;
        std::string stop_reason = "end_turn";
        if (const auto* cands = gem.if_contains("candidates");
            cands && cands->is_array() && !cands->get_array().empty()) {
            const auto& cand0 = cands->get_array()[kFirstCandidateIndex].as_object();
            if (const auto* content = cand0.if_contains("content");
                content && content->is_object()) {
                if (const auto* parts = content->get_object().if_contains("parts");
                    parts && parts->is_array()) {
                    for (const auto& pv : parts->get_array()) {
                        if (!pv.is_object()) {
                            continue;
                        }
                        const auto& part = pv.get_object();
                        if (const auto* th = part.if_contains("thought");
                            th && th->is_bool() && th->get_bool()) {
                            continue;
                        }
                        if (const auto* txt = part.if_contains("text");
                            txt && txt->is_string()) {
                            text_out += std::string(txt->get_string());
                        }
                    }
                }
            }
            if (const auto* fr = cand0.if_contains("finishReason");
                fr && fr->is_string()) {
                std::string r(fr->get_string());
                if (r == "STOP") {
                    stop_reason = "end_turn";
                } else if (r == "MAX_TOKENS") {
                    stop_reason = "max_tokens";
                } else if (r == "SAFETY" || r == "RECITATION" ||
                           r == "PROHIBITED_CONTENT" || r == "BLOCKLIST" ||
                           r == "SPII") {
                    stop_reason = "refusal";
                } else {
                    stop_reason = std::move(r);
                }
            }
        } else if (const auto* pf = gem.if_contains("promptFeedback");
                   pf && pf->is_object()) {
            if (pf->get_object().if_contains("blockReason")) {
                stop_reason = "refusal";
            }
        }

        std::int64_t prompt_tok = kZeroTokens;
        std::int64_t cand_tok = kZeroTokens;
        std::int64_t thought_tok = kZeroTokens;
        std::int64_t cached_tok = kZeroTokens;
        if (const auto* um = gem.if_contains("usageMetadata");
            um && um->is_object()) {
            const auto& u = um->get_object();
            auto geti = [&](const char* k) -> std::int64_t {
                if (const auto* v = u.if_contains(k)) {
                    if (v->is_int64()) {
                        return v->get_int64();
                    }
                    if (v->is_uint64()) {
                        return static_cast<std::int64_t>(v->get_uint64());
                    }
                }
                return kZeroTokens;
            };
            prompt_tok = geti("promptTokenCount");
            cand_tok = geti("candidatesTokenCount");
            thought_tok = geti("thoughtsTokenCount");
            cached_tok = geti("cachedContentTokenCount");
        }

        bj::object out;
        out["content"] = bj::array{
            bj::object{{"type", "text"}, {"text", std::move(text_out)}}
        };
        out["stop_reason"] = std::move(stop_reason);
        out["usage"] = bj::object{
            {"input_tokens", prompt_tok},
            {"output_tokens", cand_tok + thought_tok},
            {"cache_read_input_tokens", cached_tok},
            {"cache_creation_input_tokens", kZeroTokens}
        };
        return out;
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace fav::gemini
