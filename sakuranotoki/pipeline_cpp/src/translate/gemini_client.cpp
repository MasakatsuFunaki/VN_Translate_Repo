// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gemini_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include "common/util.h"

namespace snt::gemini {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* API_HOST = "generativelanguage.googleapis.com";
constexpr const char* API_PORT = "443";
constexpr const char* DEFAULT_MODEL = "gemini-flash-latest";
constexpr std::int64_t DEFAULT_MAX_OUTPUT_TOKENS = 16384;
constexpr std::int64_t GEMINI_MAX_OUTPUT_TOKENS = 65536;
constexpr std::int64_t DEFAULT_THINKING_BUDGET = 2048;

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

bj::object build_gemini_request(const bj::object& in_obj, std::string* out_model) {
    std::string model = DEFAULT_MODEL;
    if (const auto* m = in_obj.if_contains("model"); m && m->is_string()) {
        const std::string requested(m->get_string());
        if (requested.rfind("gemini-", 0) == 0)
            model = requested;
    }
    if (out_model) *out_model = model;

    bj::object req_json;

    if (const auto* sys_v = in_obj.if_contains("system")) {
        std::string sys_text;
        if (sys_v->is_string()) {
            sys_text = std::string(sys_v->get_string());
        } else if (sys_v->is_array()) {
            for (const auto& item : sys_v->get_array()) {
                if (!item.is_object()) continue;
                if (const auto* t = item.get_object().if_contains("text");
                    t && t->is_string()) {
                    if (!sys_text.empty()) sys_text += "\n";
                    sys_text += std::string(t->get_string());
                }
            }
        }
        if (!sys_text.empty()) {
            req_json["systemInstruction"] = bj::object{
                {"parts", bj::array{bj::object{{"text", std::move(sys_text)}}}}};
        }
    }

    bj::array contents;
    if (const auto* msgs_v = in_obj.if_contains("messages");
        msgs_v && msgs_v->is_array()) {
        for (const auto& msg_v : msgs_v->get_array()) {
            if (!msg_v.is_object()) continue;
            const bj::object& msg_obj = msg_v.get_object();
            std::string role = "user";
            if (const auto* r = msg_obj.if_contains("role");
                r && r->is_string() && r->get_string() == "assistant") {
                role = "model";
            }

            bj::array parts;
            if (const auto* c_v = msg_obj.if_contains("content")) {
                if (c_v->is_string()) {
                    parts.push_back(
                        bj::object{{"text", std::string(c_v->get_string())}});
                } else if (c_v->is_array()) {
                    for (const auto& block_v : c_v->get_array()) {
                        if (!block_v.is_object()) continue;
                        const bj::object& block = block_v.get_object();
                        std::string btype;
                        if (const auto* bt = block.if_contains("type");
                            bt && bt->is_string()) {
                            btype = std::string(bt->get_string());
                        }
                        if (btype == "text") {
                            if (const auto* txt = block.if_contains("text");
                                txt && txt->is_string()) {
                                parts.push_back(bj::object{
                                    {"text", std::string(txt->get_string())}});
                            }
                        } else if (btype == "image") {
                            if (const auto* src_v = block.if_contains("source");
                                src_v && src_v->is_object()) {
                                const bj::object& src = src_v->get_object();
                                std::string mime = "image/png";
                                if (const auto* mt = src.if_contains("media_type");
                                    mt && mt->is_string()) {
                                    mime = std::string(mt->get_string());
                                }
                                std::string data;
                                if (const auto* d = src.if_contains("data");
                                    d && d->is_string()) {
                                    data = std::string(d->get_string());
                                }
                                parts.push_back(bj::object{
                                    {"inlineData",
                                     bj::object{{"mimeType", std::move(mime)},
                                                {"data", std::move(data)}}}});
                            }
                        }
                    }
                }
            }
            if (!parts.empty()) {
                contents.push_back(bj::object{{"role", std::move(role)},
                                              {"parts", std::move(parts)}});
            }
        }
    }
    req_json["contents"] = std::move(contents);

    bj::object gen_cfg;
    std::int64_t max_tokens = DEFAULT_MAX_OUTPUT_TOKENS;
    if (const auto* mt = in_obj.if_contains("max_tokens")) {
        if (mt->is_int64())
            max_tokens = mt->get_int64();
        else if (mt->is_uint64())
            max_tokens = static_cast<std::int64_t>(mt->get_uint64());
    }
    max_tokens = std::clamp<std::int64_t>(max_tokens, 1, GEMINI_MAX_OUTPUT_TOKENS);
    gen_cfg["maxOutputTokens"] = max_tokens;

    if (const auto* temp = in_obj.if_contains("temperature")) {
        if (temp->is_double())
            gen_cfg["temperature"] = temp->get_double();
        else if (temp->is_int64())
            gen_cfg["temperature"] = temp->get_int64();
    }
    gen_cfg["thinkingConfig"] =
        bj::object{{"thinkingBudget", DEFAULT_THINKING_BUDGET}};
    req_json["generationConfig"] = std::move(gen_cfg);

    return req_json;
}

bj::value adapt_gemini_response(const bj::value& gem_val) {
    const bj::object& gem = gem_val.get_object();
    std::string text_out;
    std::string stop_reason = "end_turn";

    if (const auto* cands = gem.if_contains("candidates");
        cands && cands->is_array() && !cands->get_array().empty()) {
        const bj::object& cand0 = cands->get_array()[0].get_object();
        if (const auto* content = cand0.if_contains("content");
            content && content->is_object()) {
            if (const auto* parts = content->get_object().if_contains("parts");
                parts && parts->is_array()) {
                for (const auto& pv : parts->get_array()) {
                    if (!pv.is_object()) continue;
                    const bj::object& part = pv.get_object();
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
            const std::string reason(fr->get_string());
            if (reason == "STOP") {
                stop_reason = "end_turn";
            } else if (reason == "MAX_TOKENS") {
                stop_reason = "max_tokens";
            } else if (reason == "SAFETY" || reason == "RECITATION" ||
                       reason == "PROHIBITED_CONTENT" ||
                       reason == "BLOCKLIST" || reason == "SPII") {
                stop_reason = "refusal";
            } else {
                stop_reason = reason;
            }
        }
    } else if (const auto* pf = gem.if_contains("promptFeedback");
               pf && pf->is_object() &&
               pf->get_object().if_contains("blockReason")) {
        stop_reason = "refusal";
    }

    std::int64_t in_tok = 0;
    std::int64_t out_tok = 0;
    std::int64_t cache_read = 0;
    if (const auto* um = gem.if_contains("usageMetadata");
        um && um->is_object()) {
        const bj::object& u = um->get_object();
        auto geti = [&](const char* k) -> std::int64_t {
            if (const auto* v = u.if_contains(k)) {
                if (v->is_int64()) return v->get_int64();
                if (v->is_uint64()) return static_cast<std::int64_t>(v->get_uint64());
            }
            return 0;
        };
        in_tok = geti("promptTokenCount");
        out_tok = geti("candidatesTokenCount") + geti("thoughtsTokenCount");
        cache_read = geti("cachedContentTokenCount");
    }

    bj::object out;
    out["content"] = bj::array{
        bj::object{{"type", "text"}, {"text", std::move(text_out)}}};
    out["stop_reason"] = std::move(stop_reason);
    out["usage"] = bj::object{
        {"input_tokens", in_tok},
        {"output_tokens", out_tok},
        {"cache_read_input_tokens", cache_read},
        {"cache_creation_input_tokens", std::int64_t{0}},
    };
    return out;
}

}  // namespace

std::string load_api_key() {
    if (const char* env = std::getenv("GEMINI_API_KEY"); env && *env)
        return env;
    try {
        if (const char* home = std::getenv("USERPROFILE"); home && *home) {
            const std::string cfg = std::string(home) + "\\.gemini\\.env";
            if (std::filesystem::exists(std::filesystem::u8path(cfg))) {
                std::ifstream in(std::filesystem::u8path(cfg));
                std::string line;
                while (std::getline(in, line)) {
                    std::string s = trim(line);
                    if (s.rfind("export ", 0) == 0)
                        s = trim(s.substr(7));
                    if (s.rfind("GEMINI_API_KEY=", 0) == 0) {
                        std::string key =
                            trim(s.substr(std::strlen("GEMINI_API_KEY=")));
                        if (key.size() >= 2 &&
                            ((key.front() == '"' && key.back() == '"') ||
                             (key.front() == '\'' && key.back() == '\''))) {
                            key = key.substr(1, key.size() - 2);
                        }
                        if (!key.empty()) {
                            _putenv_s("GEMINI_API_KEY", key.c_str());
                            log_info("[OK] Loaded GEMINI_API_KEY from ~/.gemini/.env");
                            return key;
                        }
                    }
                }
            }
        }
        char reg_buf[512] = {};
        DWORD reg_sz = sizeof(reg_buf);
        if (RegGetValueA(HKEY_CURRENT_USER, "Environment", "GEMINI_API_KEY",
                         RRF_RT_REG_SZ, nullptr, reg_buf, &reg_sz) == ERROR_SUCCESS) {
            std::string key = trim(std::string(reg_buf));
            if (!key.empty()) {
                _putenv_s("GEMINI_API_KEY", key.c_str());
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
        if (load_api_key().empty())
            throw ApiError("GEMINI_API_KEY not set -- this run has work "
                           "left to do and needs the API");
        client_.emplace();
    }
    return *client_;
}

Client::Client(std::string api_key) : api_key_(std::move(api_key)) {
    if (api_key_.empty())
        api_key_ = load_api_key();
    if (api_key_.empty())
        throw ApiError("GEMINI_API_KEY not set");
}

boost::json::value Client::messages(const bj::value& body, int timeout_sec) {
    try {
        std::string model = DEFAULT_MODEL;
        const bj::object req_json =
            body.is_object() ? build_gemini_request(body.get_object(), &model)
                             : build_gemini_request(bj::object{}, &model);
        const std::string target =
            "/v1beta/models/" + model + ":generateContent?key=" + api_key_;

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

        http::request<http::string_body> req{http::verb::post, target, 11};
        req.set(http::field::host, API_HOST);
        req.set(http::field::user_agent, "snt-pipeline-cpp/1.0");
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
        stream.shutdown(ec);  // best-effort; many servers just close

        const unsigned status = res.result_int();
        if (status == 429)
            throw RateLimitError("HTTP 429: " + res.body());
        if (status < 200 || status >= 300)
            throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
        return adapt_gemini_response(bj::parse(res.body()));
    } catch (const RateLimitError&) {
        throw;
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception& e) {
        throw ApiError(std::string("transport error: ") + e.what());
    }
}

}  // namespace snt::gemini
