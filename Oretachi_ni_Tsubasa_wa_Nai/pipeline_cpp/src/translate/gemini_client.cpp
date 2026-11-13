// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/gemini_client.h"

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
#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace oretsuba::translate {

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

constexpr const char* kApiHost = "generativelanguage.googleapis.com";
constexpr const char* kApiPort = "443";
constexpr const char* kUserAgent = "oretsuba-pipeline-cpp/1.0";
constexpr std::int64_t kThinkingBudgetTokens = 2048;
constexpr std::int64_t kMaxOutputTokens = 16384;
constexpr std::uint64_t kHttpParserBodyLimitBytes = 64ULL * 1024ULL * 1024ULL;
constexpr int kHttpVersion11 = 11;
constexpr unsigned kHttpStatusOkMin = 200U;
constexpr unsigned kHttpStatusOkMaxExclusive = 300U;
constexpr unsigned kHttpStatusTooManyRequests = 429U;
constexpr std::size_t kMinQuotedKeyLength = 2U;
constexpr std::size_t kBinarySplitDivisor = 2U;
constexpr std::size_t kMaxSubBatchDepth = 2U;

constexpr std::string_view kGeminiKeyEnvPrefix = "GEMINI_API_KEY=";

std::string ReplaceAllSubstring(std::string input,
                                std::string_view from,
                                std::string_view to) {
    if (from.empty()) {
        return input;
    }
    std::string out;
    out.reserve(input.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = input.find(from, pos);
        if (hit == std::string::npos) {
            out.append(input, pos, std::string::npos);
            return out;
        }
        out.append(input, pos, hit - pos);
        out.append(to.data(), to.size());
        pos = hit + from.size();
    }
}

void LoadWindowsRootCerts(asio::ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) {
        return;
    }
    X509_STORE* x509 = X509_STORE_new();
    for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr);
         cert != nullptr;
         cert = CertEnumCertificatesInStore(store, cert)) {
        const unsigned char* encoded = cert->pbCertEncoded;
        X509* parsed_cert =
            d2i_X509(nullptr, &encoded, static_cast<long>(cert->cbCertEncoded));
        if (parsed_cert != nullptr) {
            X509_STORE_add_cert(x509, parsed_cert);
            X509_free(parsed_cert);
        }
    }
    CertCloseStore(store, 0);
    SSL_CTX_set_cert_store(ctx.native_handle(), x509);
}

std::string StripSurroundingQuotes(std::string value) {
    if (value.size() < kMinQuotedKeyLength) {
        return value;
    }
    const bool double_quoted = (value.front() == '"' && value.back() == '"');
    const bool single_quoted = (value.front() == '\'' && value.back() == '\'');
    if (double_quoted || single_quoted) {
        return TrimUnicode(value.substr(1U, value.size() - kMinQuotedKeyLength));
    }
    return value;
}

std::string ReadKeyFromDotenvFile(const fs::path& dotenv_path) {
    std::error_code ec;
    if (!fs::is_regular_file(dotenv_path, ec)) {
        return {};
    }
    std::ifstream file(dotenv_path);
    if (!file) {
        return {};
    }
    std::string line;
    while (std::getline(file, line)) {
        line = TrimUnicode(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.rfind(kGeminiKeyEnvPrefix.data(), 0) == 0) {
            std::string key = StripSurroundingQuotes(
                TrimUnicode(line.substr(kGeminiKeyEnvPrefix.size())));
            if (!key.empty()) {
                return key;
            }
        }
    }
    return {};
}

std::string ReadKeyFromSettingsJson(const fs::path& settings_path) {
    std::error_code ec;
    if (!fs::is_regular_file(settings_path, ec)) {
        return {};
    }
    try {
        const bj::value val = JsonParseFile(settings_path.u8string());
        if (!val.is_object()) {
            return {};
        }
        const auto& obj = val.as_object();
        if (const auto* direct = obj.if_contains("GEMINI_API_KEY");
            direct && direct->is_string()) {
            const std::string key = TrimUnicode(std::string(direct->as_string()));
            if (!key.empty()) {
                return key;
            }
        }
        if (const auto* env_val = obj.if_contains("env");
            env_val && env_val->is_object()) {
            const auto& env_obj = env_val->as_object();
            if (const auto* nested = env_obj.if_contains("GEMINI_API_KEY");
                nested && nested->is_string()) {
                const std::string key = TrimUnicode(std::string(nested->as_string()));
                if (!key.empty()) {
                    return key;
                }
            }
        }
    } catch (...) {
        return {};
    }
    return {};
}

std::map<int, std::string> SubBatchTranslateRecursive(
    ITranslationBackend& backend,
    const std::vector<std::string>& prompt_lines,
    std::size_t depth) {
    std::map<int, std::string> combined;
    if (prompt_lines.empty()) {
        return combined;
    }

    const std::size_t chunk_size =
        (prompt_lines.size() > kFallbackSubBatchSize)
            ? kFallbackSubBatchSize
            : std::max<std::size_t>(1U, prompt_lines.size() / kBinarySplitDivisor);

    for (std::size_t start = 0; start < prompt_lines.size(); start += chunk_size) {
        const std::size_t end = std::min(prompt_lines.size(), start + chunk_size);
        const std::vector<std::string> sub_lines(
            prompt_lines.begin() + static_cast<std::ptrdiff_t>(start),
            prompt_lines.begin() + static_cast<std::ptrdiff_t>(end));

        auto sub_res = backend.TranslateBatch(sub_lines, {}, {}, true);
        if (sub_res.empty() && sub_lines.size() > 1U && depth < kMaxSubBatchDepth) {
            sub_res = SubBatchTranslateRecursive(backend, sub_lines, depth + 1U);
        }
        for (const auto& [sub_idx, text] : sub_res) {
            if (sub_idx >= 1 && static_cast<std::size_t>(sub_idx) <= sub_lines.size()) {
                combined[static_cast<int>(start) + sub_idx] = text;
            }
        }
    }
    return combined;
}

std::string WideEnvToUtf8(const wchar_t* var_name) {
    const wchar_t* wide_val = ::_wgetenv(var_name);
    if (wide_val == nullptr || *wide_val == L'\0') {
        return {};
    }
    const std::wstring_view wide_view(wide_val);
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(), static_cast<int>(wide_view.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(), static_cast<int>(wide_view.size()),
        utf8.data(), required, nullptr, nullptr);
    if (written != required) {
        return {};
    }
    return utf8;
}

void CacheApiKeyInProcessEnv(std::string_view utf8_key) {
    if (utf8_key.empty()) {
        _wputenv_s(L"GEMINI_API_KEY", L"");
        return;
    }
    const int wide_len = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_key.data(),
        static_cast<int>(utf8_key.size()), nullptr, 0);
    if (wide_len <= 0) {
        return;
    }
    std::wstring wide_key(static_cast<std::size_t>(wide_len), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_key.data(),
        static_cast<int>(utf8_key.size()), wide_key.data(), wide_len);
    if (written != wide_len) {
        return;
    }
    _wputenv_s(L"GEMINI_API_KEY", wide_key.c_str());
}

}  // namespace

ITranslationBackend::~ITranslationBackend() = default;

std::string LoadGeminiApiKey(const std::string& project_dir) {
    if (const std::string env_key = WideEnvToUtf8(L"GEMINI_API_KEY"); !env_key.empty()) {
        return env_key;
    }

    std::vector<fs::path> settings_candidates;
    if (!project_dir.empty()) {
        settings_candidates.push_back(fs::u8path(project_dir) / ".claude" / "settings.json");
        settings_candidates.push_back(
            fs::u8path(project_dir).parent_path() / ".claude" / "settings.json");
    }
    settings_candidates.push_back(fs::u8path(".claude/settings.json"));

    const std::string home = WideEnvToUtf8(L"USERPROFILE");
    if (!home.empty()) {
        settings_candidates.push_back(
            fs::u8path(home) / ".claude_vn" / "settings.json");
        settings_candidates.push_back(
            fs::u8path(home) / ".claude" / "settings.json");
    }

    for (const auto& candidate : settings_candidates) {
        const std::string key = ReadKeyFromSettingsJson(candidate);
        if (!key.empty()) {
            CacheApiKeyInProcessEnv(key);
            return key;
        }
    }

    if (!home.empty()) {
        const fs::path dotenv_path = fs::u8path(home) / ".gemini" / ".env";
        const std::string key = ReadKeyFromDotenvFile(dotenv_path);
        if (!key.empty()) {
            CacheApiKeyInProcessEnv(key);
            return key;
        }
    }
    return {};
}

std::string SanitizeAscii(std::string_view text) {
    std::string s(text);
    s = ReplaceAllSubstring(std::move(s), "—", "--");
    s = ReplaceAllSubstring(std::move(s), "–", "--");
    s = ReplaceAllSubstring(std::move(s), "―", "--");
    s = ReplaceAllSubstring(std::move(s), "─", "--");
    s = ReplaceAllSubstring(std::move(s), "…", "...");
    s = ReplaceAllSubstring(std::move(s), "⋯", "...");
    s = ReplaceAllSubstring(std::move(s), "‘", "'");
    s = ReplaceAllSubstring(std::move(s), "’", "'");
    s = ReplaceAllSubstring(std::move(s), "“", "\"");
    s = ReplaceAllSubstring(std::move(s), "”", "\"");
    s = ReplaceAllSubstring(std::move(s), "「", "\"");
    s = ReplaceAllSubstring(std::move(s), "」", "\"");
    s = ReplaceAllSubstring(std::move(s), "『", "'");
    s = ReplaceAllSubstring(std::move(s), "』", "'");
    s = ReplaceAllSubstring(std::move(s), "≪", "\"");
    s = ReplaceAllSubstring(std::move(s), "≫", "\"");
    s = ReplaceAllSubstring(std::move(s), "　", " ");
    s = ReplaceAllSubstring(std::move(s), "〜", "~");
    s = ReplaceAllSubstring(std::move(s), "～", "~");
    s = ReplaceAllSubstring(std::move(s), "é", "e");
    s = ReplaceAllSubstring(std::move(s), "ê", "e");
    s = ReplaceAllSubstring(std::move(s), "è", "e");

    static const boost::regex kSquareRubyRe(R"re(\[([^|\]]+)\|([^\]]+)\])re");
    s = boost::regex_replace(s, kSquareRubyRe, "$2");

    static const boost::regex kAngleRubyRe(R"re(<"([^"|]+)\|([^"]+)">)re");
    s = boost::regex_replace(s, kAngleRubyRe, "$1");

    return s;
}

std::string StripSpeakerTags(std::string_view text) {
    static const boost::regex kSpeakerTagRe(
        R"re(^(?:\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_/-]+)\]\s*)+)re");
    const std::string raw(text);
    return TrimUnicode(SanitizeAscii(boost::regex_replace(raw, kSpeakerTagRe, "")));
}

std::string BuildUserPrompt(const std::vector<std::string>& batch_lines,
                            const std::vector<std::string>& previous_context,
                            const std::vector<std::string>& context_after) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t start =
            previous_context.size() > kContextWindowLines
                ? previous_context.size() - kContextWindowLines
                : 0U;
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            user_prompt += previous_context[i] + "\n";
        }
        user_prompt += "</previous_context>\n\n";
    }

    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        user_prompt += std::to_string(i + 1U) + ". " + batch_lines[i] + "\n";
    }
    user_prompt += "</lines_to_translate>\n\n";

    if (!context_after.empty()) {
        user_prompt += "<context_after>\n";
        const std::size_t take = std::min(context_after.size(), kContextAfterLines);
        for (std::size_t i = 0; i < take; ++i) {
            user_prompt += context_after[i] + "\n";
        }
        user_prompt += "</context_after>\n\n";
    }

    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use <previous_context> (if present) for scene continuity, and "
        "<context_after> (if present) for lookahead context, but do NOT translate them. "
        "Never include [Speaker] tags in your output. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line.";
    return user_prompt;
}

std::map<int, std::string> ParseNumberedResponse(std::string_view response_text) {
    static const boost::regex kNumberedLineRe(R"(^(\d+)[.\):\s]+\s*(.*))");
    std::map<int, std::string> translations;
    int current_num = -1;
    std::vector<std::string> current_parts;

    auto flush_current = [&]() {
        if (current_num < 0) {
            return;
        }
        std::string joined;
        for (std::size_t i = 0; i < current_parts.size(); ++i) {
            if (i > 0U) {
                joined += ' ';
            }
            joined += current_parts[i];
        }
        translations[current_num] = StripSpeakerTags(TrimUnicode(joined));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        const std::string_view slice =
            (nl == std::string_view::npos)
                ? response_text.substr(pos)
                : response_text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? response_text.size() + 1U : nl + 1U;

        const std::string line = TrimUnicode(slice);
        if (line.empty()) {
            continue;
        }
        boost::smatch match;
        if (boost::regex_match(line, match, kNumberedLineRe)) {
            flush_current();
            current_num = std::stoi(match[1].str());
            const std::string clean_part = StripSpeakerTags(TrimUnicode(match[2].str()));
            current_parts.clear();
            if (!clean_part.empty()) {
                current_parts.push_back(clean_part);
            }
            continue;
        }
        if (current_num >= 0) {
            current_parts.push_back(line);
        }
    }
    flush_current();
    return translations;
}

std::map<int, std::string> TranslateBatchWithFallback(
    ITranslationBackend& backend,
    const std::vector<std::string>& prompt_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after) {
    if (prompt_lines.empty()) {
        return {};
    }

    auto translations =
        backend.TranslateBatch(prompt_lines, previous_context, context_after, false);
    if (!translations.empty()) {
        return translations;
    }

    if (!previous_context.empty() || !context_after.empty()) {
        LogInfo("    Empty response with context window -- retrying batch without context");
        translations = backend.TranslateBatch(prompt_lines, {}, {}, false);
        if (!translations.empty()) {
            return translations;
        }
    }

    LogInfo("    Retrying batch with neutral literary prompt...");
    translations = backend.TranslateBatch(prompt_lines, {}, {}, true);
    if (!translations.empty() || prompt_lines.size() <= 1U) {
        return translations;
    }

    LogInfo("    Batch refused or empty -- splitting into sub-batches...");
    return SubBatchTranslateRecursive(backend, prompt_lines, 0U);
}

struct GeminiClient::Impl {
    std::string api_key;
    std::string model;
    Glossary glossary;

    Impl(std::string key, std::string mdl, const std::string& project_dir)
        : api_key(std::move(key)),
          model(std::move(mdl)) {
        if (!project_dir.empty()) {
            const fs::path tone_path = fs::u8path(project_dir) / "TONE_OF_NOVEL.md";
            glossary.LoadFromToneFile(tone_path.u8string());
        }
        if (api_key.empty()) {
            api_key = LoadGeminiApiKey(project_dir);
        }
        if (api_key.empty()) {
            throw ApiError("GEMINI_API_KEY not set in environment, .claude/settings.json, or ~/.gemini/.env");
        }
        if (model.empty()) {
            model = kDefaultModel;
        }
    }

    bj::value MessagesImpl(const bj::value& body, int timeout_sec) {
        try {
            const bj::object empty_obj;
            const bj::object& in_obj = body.is_object() ? body.get_object() : empty_obj;

            bj::object req_json;
            if (const auto* sys = in_obj.if_contains("system")) {
                std::string sys_text;
                if (sys->is_string()) {
                    sys_text = std::string(sys->get_string());
                }
                if (sys->is_array()) {
                    for (const auto& item : sys->get_array()) {
                        if (!item.is_object()) {
                            continue;
                        }
                        if (const auto* txt = item.get_object().if_contains("text");
                            txt && txt->is_string()) {
                            if (!sys_text.empty()) {
                                sys_text += "\n";
                            }
                            sys_text += std::string(txt->get_string());
                        }
                    }
                }
                if (!sys_text.empty()) {
                    req_json["systemInstruction"] = bj::object{
                        {"parts", bj::array{bj::object{{"text", sys_text}}}},
                    };
                }
            }

            bj::array contents;
            if (const auto* msgs = in_obj.if_contains("messages");
                msgs && msgs->is_array()) {
                for (const auto& mv : msgs->get_array()) {
                    if (!mv.is_object()) {
                        continue;
                    }
                    const auto& mobj = mv.get_object();
                    std::string role = "user";
                    if (const auto* role_v = mobj.if_contains("role");
                        role_v && role_v->is_string() && role_v->get_string() == "assistant") {
                        role = "model";
                    }

                    bj::array parts;
                    if (const auto* content_v = mobj.if_contains("content")) {
                        if (content_v->is_string()) {
                            parts.push_back(bj::object{
                                {"text", std::string(content_v->get_string())},
                            });
                        }
                        if (content_v->is_array()) {
                            for (const auto& bv : content_v->get_array()) {
                                if (!bv.is_object()) {
                                    continue;
                                }
                                const auto& block = bv.get_object();
                                if (const auto* txt = block.if_contains("text");
                                    txt && txt->is_string()) {
                                    parts.push_back(bj::object{
                                        {"text", std::string(txt->get_string())},
                                    });
                                }
                            }
                        }
                    }
                    contents.push_back(bj::object{
                        {"role", role},
                        {"parts", std::move(parts)},
                    });
                }
            }
            req_json["contents"] = std::move(contents);

            std::int64_t max_tokens = kMaxOutputTokens;
            if (const auto* mt = in_obj.if_contains("max_tokens")) {
                if (mt->is_int64()) {
                    max_tokens = mt->get_int64();
                }
                if (mt->is_uint64()) {
                    max_tokens = static_cast<std::int64_t>(mt->get_uint64());
                }
            }
            max_tokens = std::min<std::int64_t>(max_tokens, kMaxOutputTokens);

            bj::object gen_cfg;
            gen_cfg["maxOutputTokens"] = max_tokens;
            if (const auto* temp = in_obj.if_contains("temperature")) {
                gen_cfg["temperature"] = *temp;
            }
            gen_cfg["thinkingConfig"] = bj::object{
                {"thinkingBudget", kThinkingBudgetTokens},
            };
            req_json["generationConfig"] = std::move(gen_cfg);

            bj::array safety_settings;
            for (const char* cat : {"HARM_CATEGORY_HARASSMENT",
                                    "HARM_CATEGORY_HATE_SPEECH",
                                    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
                                    "HARM_CATEGORY_DANGEROUS_CONTENT",
                                    "HARM_CATEGORY_CIVIC_INTEGRITY"}) {
                safety_settings.push_back(bj::object{
                    {"category", cat},
                    {"threshold", "BLOCK_NONE"},
                });
            }
            req_json["safetySettings"] = std::move(safety_settings);

            asio::io_context ioc;
            asio::ssl::context ssl_ctx(asio::ssl::context::tls_client);
            ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
            LoadWindowsRootCerts(ssl_ctx);

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), kApiHost)) {
                throw ApiError("SNI setup failed");
            }
            SSL_set1_host(stream.native_handle(), kApiHost);

            tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(kApiHost, kApiPort);
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_sec));
            beast::get_lowest_layer(stream).connect(endpoints);
            stream.handshake(asio::ssl::stream_base::client);

            const std::string target =
                "/v1beta/models/" + model + ":generateContent?key=" + api_key;
            http::request<http::string_body> req{http::verb::post, target, kHttpVersion11};
            req.set(http::field::host, kApiHost);
            req.set(http::field::user_agent, kUserAgent);
            req.set(http::field::content_type, "application/json");
            req.body() = bj::serialize(req_json);
            req.prepare_payload();

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(kHttpParserBodyLimitBytes);
            http::read(stream, buffer, parser);
            const http::response<http::string_body> res = parser.release();

            beast::error_code ec;
            stream.shutdown(ec);

            const unsigned status = res.result_int();
            if (status == kHttpStatusTooManyRequests) {
                throw RateLimitError("HTTP 429: " + res.body());
            }
            if (status < kHttpStatusOkMin || status >= kHttpStatusOkMaxExclusive) {
                throw ApiError("HTTP " + std::to_string(status) + ": " + res.body());
            }

            const bj::value gem_val = bj::parse(res.body());
            const bj::object& gem = gem_val.as_object();

            std::string text_out;
            std::string stop_reason = "end_turn";
            if (const auto* cands = gem.if_contains("candidates");
                cands && cands->is_array() && !cands->get_array().empty()) {
                const auto& cand0 = cands->get_array()[0].as_object();
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
                    std::string reason(fr->get_string());
                    if (reason == "STOP") {
                        stop_reason = "end_turn";
                    } else if (reason == "MAX_TOKENS") {
                        stop_reason = "max_tokens";
                    } else if (reason == "SAFETY" || reason == "RECITATION" ||
                               reason == "PROHIBITED_CONTENT" || reason == "BLOCKLIST" ||
                               reason == "SPII") {
                        stop_reason = "refusal";
                    } else {
                        stop_reason = std::move(reason);
                    }
                }
            } else if (const auto* pf = gem.if_contains("promptFeedback");
                       pf && pf->is_object() && pf->get_object().if_contains("blockReason")) {
                stop_reason = "refusal";
            }

            std::int64_t prompt_tok = 0;
            std::int64_t cand_tok = 0;
            std::int64_t thought_tok = 0;
            std::int64_t cached_tok = 0;
            if (const auto* um = gem.if_contains("usageMetadata");
                um && um->is_object()) {
                const auto& u = um->get_object();
                auto read_token_count = [&](const char* key) -> std::int64_t {
                    if (const auto* v = u.if_contains(key)) {
                        if (v->is_int64()) {
                            return v->get_int64();
                        }
                        if (v->is_uint64()) {
                            return static_cast<std::int64_t>(v->get_uint64());
                        }
                    }
                    return 0;
                };
                prompt_tok = read_token_count("promptTokenCount");
                cand_tok = read_token_count("candidatesTokenCount");
                thought_tok = read_token_count("thoughtsTokenCount");
                cached_tok = read_token_count("cachedContentTokenCount");
            }

            bj::object out;
            out["content"] = bj::array{
                bj::object{{"type", "text"}, {"text", std::move(text_out)}},
            };
            out["stop_reason"] = std::move(stop_reason);
            out["usage"] = bj::object{
                {"input_tokens", prompt_tok},
                {"output_tokens", cand_tok + thought_tok},
                {"cache_read_input_tokens", cached_tok},
                {"cache_creation_input_tokens", 0},
            };
            return out;
        } catch (const RateLimitError&) {
            throw;
        } catch (const ApiError&) {
            throw;
        } catch (const std::exception& ex) {
            throw ApiError(std::string("transport error: ") + ex.what());
        }
    }

    std::map<int, std::string> TranslateBatchImpl(
        const std::vector<std::string>& prompt_lines,
        const std::vector<std::string>& previous_context,
        const std::vector<std::string>& context_after,
        bool use_neutral_prompt) {
        bj::object body;
        body["model"] = model;
        body["max_tokens"] = kMaxOutputTokens;

        bj::object sys_block;
        sys_block["type"] = "text";
        sys_block["text"] = use_neutral_prompt
                                ? glossary.NeutralSystemPrompt()
                                : glossary.SystemPrompt();
        sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
        body["system"] = bj::array{std::move(sys_block)};

        bj::object msg;
        msg["role"] = "user";
        msg["content"] = BuildUserPrompt(prompt_lines, previous_context, context_after);
        body["messages"] = bj::array{std::move(msg)};

        const bj::value response = MessagesImpl(bj::value(std::move(body)), kDefaultTimeoutSec);

        std::string response_text;
        if (const auto* content = response.get_object().if_contains("content");
            content && content->is_array()) {
            for (const auto& block : content->get_array()) {
                if (!block.is_object()) {
                    continue;
                }
                if (const auto* t = block.get_object().if_contains("type");
                    t && t->is_string() && t->get_string() == "text") {
                    if (const auto* txt = block.get_object().if_contains("text");
                        txt && txt->is_string()) {
                        response_text = TrimUnicode(std::string(txt->get_string()));
                        break;
                    }
                }
            }
        }

        std::int64_t in_tok = 0;
        std::int64_t out_tok = 0;
        std::int64_t cache_read = 0;
        if (const auto* usage = response.get_object().if_contains("usage");
            usage && usage->is_object()) {
            const auto& u = usage->get_object();
            auto read_int = [&](const char* key) -> std::int64_t {
                if (const auto* v = u.if_contains(key); v && v->is_int64()) {
                    return v->get_int64();
                }
                return 0;
            };
            in_tok = read_int("input_tokens");
            out_tok = read_int("output_tokens");
            cache_read = read_int("cache_read_input_tokens");
        }
        LogInfo("    Tokens: in=" + std::to_string(in_tok) +
                " out=" + std::to_string(out_tok) +
                " cache_read=" + std::to_string(cache_read));

        return ParseNumberedResponse(response_text);
    }
};

GeminiClient::GeminiClient(std::string api_key,
                           std::string model,
                           std::string project_dir)
    : impl_(std::make_unique<Impl>(std::move(api_key),
                                   std::move(model),
                                   project_dir)) {}

GeminiClient::GeminiClient(GeminiClient&& other) noexcept = default;
GeminiClient& GeminiClient::operator=(GeminiClient&& other) noexcept = default;
GeminiClient::~GeminiClient() = default;

std::map<int, std::string> GeminiClient::TranslateBatch(
    const std::vector<std::string>& prompt_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after,
    bool use_neutral_prompt) {
    return impl_->TranslateBatchImpl(
        prompt_lines, previous_context, context_after, use_neutral_prompt);
}

boost::json::value GeminiClient::Messages(const boost::json::value& body, int timeout_sec) {
    return impl_->MessagesImpl(body, timeout_sec);
}

const std::string& GeminiClient::Model() const {
    return impl_->model;
}

}  // namespace oretsuba::translate
