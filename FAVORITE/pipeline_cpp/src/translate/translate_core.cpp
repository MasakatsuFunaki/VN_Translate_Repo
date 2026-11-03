// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <boost/regex.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

namespace fav::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size < 1)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < 0)
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    // A negative cap produces budget < 0, which reads as "no limit" and starts
    // an unbounded run -- the opposite of what the flag is for.
    if (max_batches < 0)
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    return std::nullopt;
}

bool needs_translation(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty() || t.front() == '_' || t.find('/') != std::string::npos) return false;
    return is_jp(t);
}

std::string strip(const std::string& s) { return trim(s); }

namespace {

std::string replace_all_str(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

}  // namespace

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    s = replace_all_str(s, "\xE2\x80\x94", "--");   // U+2014 em dash
    s = replace_all_str(s, "\xE2\x80\x93", "--");   // U+2013 en dash
    s = replace_all_str(s, "\xE2\x80\x95", "--");   // U+2015 horizontal bar
    s = replace_all_str(s, "\xE2\x80\xA6", "...");  // U+2026 horizontal ellipsis
    s = replace_all_str(s, "\xE2\x80\x98", "'");    // U+2018 left single quote
    s = replace_all_str(s, "\xE2\x80\x99", "'");    // U+2019 right single quote
    s = replace_all_str(s, "\xE2\x80\x9C", "\"");   // U+201C left double quote
    s = replace_all_str(s, "\xE2\x80\x9D", "\"");   // U+201D right double quote
    s = replace_all_str(s, "\xE3\x80\x80", " ");    // U+3000 ideographic space
    static const boost::regex ruby_re(R"(\[([^|\]]+)\|([^\]]+)\])");
    s = boost::regex_replace(s, ruby_re, "$2");
    return s;
}

std::string strip_speaker_tags(const std::string& text) {
    static const boost::regex re(R"(^(?:\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_-]+)\]\s*)+)");
    return strip(sanitize_ascii(boost::regex_replace(text, re, "")));
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) return cache;
    bj::value root = json_parse_file(cache_file);
    // A cache is a JSON object of jp -> en.  get_object() rejects any other
    // shape without saying which file it read, and the caller is often about
    // to delete that file.
    if (!root.is_object())
        throw std::runtime_error("cannot parse " + cache_file +
                                 ": a translation cache must be a JSON object");
    for (const auto& kv : root.get_object())
        if (kv.value().is_string())
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
    return cache;
}

std::size_t cache_entry_count(const std::string& cache_file) {
    return load_cache(cache_file).size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD)
        return std::nullopt;
    return flag + " deletes the translation cache, which holds " +
           std::to_string(cache_entries) +
           " lines that were paid for. Nothing in this pipeline can rebuild "
           "them.\n"
           "        To cap a run and keep them, use --max-batches N instead.\n"
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items())
        o[jp] = en;
    write_file_text(cache_file, json_pretty(bj::value(std::move(o)), 2));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    const std::string t = trim(jp);
    if (!t.empty() && (t.front() == '_' || t.find('/') != std::string::npos)) return true;
    if (jp != en) return false;
    for (char c : jp)
        if (static_cast<unsigned char>(c) >= 0x80) return true;
    return false;
}

std::size_t purge_failed_entries(Cache& cache) {
    std::size_t removed = 0;
    Cache kept;
    for (const auto& [jp, en] : cache.items()) {
        if (is_failed_entry(jp, en)) ++removed;
        else kept.set(jp, en);
    }
    if (removed) cache = std::move(kept);
    return removed;
}

void apply_line_result(Cache& cache, std::vector<std::string>& previous_context,
                       const std::string& jp, const std::string& en) {
    // An answer that never arrived is not a translation.  Caching the Japanese
    // as its own English would mark the line done, and no later run would ask
    // for it again.  The context window still gets the Japanese, so the next
    // request reads in sequence.
    if (en.empty()) {
        previous_context.push_back(jp);
        return;
    }
    cache.set(jp, en);
    previous_context.push_back(en);
}

// --------------------------------------------------------------------------- //
// API call
// --------------------------------------------------------------------------- //

namespace {

void print_context_window(const std::vector<std::string>& previous_context) {
    std::size_t n = std::min<std::size_t>(previous_context.size(), CONTEXT_WINDOW);
    log_info("\n=== ROLLING CONTEXT WINDOW (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of file)");
    } else {
        for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i) {
            const std::string& txt = previous_context[i];
            std::string flat;
            for (std::size_t k = 0; k < txt.size(); ++k) {
                if (txt.compare(k, 2, "\r\n") == 0) { flat += '|'; ++k; }
                else if (txt[k] == '\n') flat += '|';
                else flat += txt[k];
            }
            if (flat.size() > 120) flat.resize(120);
            log_info("  " + flat);
        }
    }
    log_info("=== END CONTEXT ===\n");
}

}  // namespace

std::string build_user_prompt(const std::vector<std::string>& batch_lines,
                              const std::vector<std::string>& previous_context,
                              const std::vector<std::string>& context_after) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                ? previous_context.size() - CONTEXT_WINDOW
                                : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i)
            user_prompt += previous_context[i] + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i)
        user_prompt += std::to_string(i + 1) + ". " + batch_lines[i] + "\n";
    user_prompt += "</lines_to_translate>\n\n";
    if (!context_after.empty()) {
        user_prompt += "<context_after>\n";
        const std::size_t take =
            std::min<std::size_t>(context_after.size(), static_cast<std::size_t>(CONTEXT_AFTER));
        for (std::size_t i = 0; i < take; ++i)
            user_prompt += context_after[i] + "\n";
        user_prompt += "</context_after>\n\n";
    }
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use <previous_context> (if present) for scene continuity, and <context_after> (if present) for lookahead context, but do NOT translate them. "
        "Never include [Speaker] tags in your output. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line.";
    return user_prompt;
}

std::map<int, std::string> parse_numbered_response(const std::string& response_text) {
    static const boost::regex line_re(R"(^(\d+)[.\):\s]+\s*(.*))");
    std::map<int, std::string> translations;
    int current_num = -1;
    std::vector<std::string> current_parts;

    auto flush = [&] {
        if (current_num < 0) return;
        std::string joined;
        for (std::size_t i = 0; i < current_parts.size(); ++i) {
            if (i) joined += ' ';
            joined += current_parts[i];
        }
        translations[current_num] = strip_speaker_tags(strip(joined));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = strip(line);
        if (line.empty()) continue;
        boost::smatch m;
        if (boost::regex_match(line, m, line_re)) {
            flush();
            current_num = std::stoi(m[1].str());
            std::string text = strip_speaker_tags(strip(m[2].str()));
            current_parts.clear();
            if (!text.empty()) current_parts.push_back(text);
        } else if (current_num >= 0) {
            current_parts.push_back(line);
        }
    }
    flush();
    return translations;
}

std::map<int, std::string> call_gemini(
    gemini::Client& client, const std::vector<std::string>& batch_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after) {
    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = 16384;
    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] = SYSTEM_PROMPT;
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    body["system"] = bj::array{std::move(sys_block)};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context, context_after);
    body["messages"] = bj::array{std::move(msg)};

    bj::value response = client.messages(bj::value(std::move(body)));

    // With adaptive/extended thinking the first content block is thinking;
    // translations live in the first block whose type == "text".
    std::string response_text;
    if (auto* content = response.get_object().if_contains("content"))
        for (const auto& block : content->get_array())
            if (auto* t = block.get_object().if_contains("type"))
                if (t->is_string() && t->get_string() == "text") {
                    if (auto* txt = block.get_object().if_contains("text"))
                        response_text = strip(std::string(txt->get_string()));
                    break;
                }

    log_info("\n--- GEMINI'S RAW ANSWER ---");
    log_info(response_text);
    log_info("---------------------------\n");

    std::int64_t in_tok = 0, out_tok = 0, cache_read = 0, cache_create = 0;
    if (auto* usage = response.get_object().if_contains("usage")) {
        const auto& u = usage->get_object();
        auto geti = [&](const char* k) -> std::int64_t {
            if (auto* v = u.if_contains(k))
                if (v->is_int64()) return v->get_int64();
            return 0;
        };
        in_tok = geti("input_tokens");
        out_tok = geti("output_tokens");
        cache_read = geti("cache_read_input_tokens");
        cache_create = geti("cache_creation_input_tokens");
    }
    log_info("    Tokens: in=" + std::to_string(in_tok) + " out=" + std::to_string(out_tok) +
             " cache_read=" + std::to_string(cache_read) +
             " cache_create=" + std::to_string(cache_create));

    return parse_numbered_response(response_text);
}

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

namespace {

// Per-batch JSON log: entries accumulate per batch index and the whole batch
// is rewritten after every append, so a Ctrl+C still leaves the partial batch
// on disk.
class BatchLogger {
public:
    BatchLogger(std::string dir, int num_batches)
        : dir_(std::move(dir)), num_batches_(num_batches) {}

    void append(bj::object entry) {
        entries_.push_back(std::move(entry));
        write_file_text(path(), json_pretty(bj::value(entries_), 2));
    }

    void next_batch() {
        ++batch_num_;
        entries_.clear();
    }

    std::string path() const {
        return num_batches_ > 1
                   ? dir_ + "\\last_translation" + std::to_string(batch_num_ + 1) + ".json"
                   : dir_ + "\\last_translation.json";
    }

private:
    std::string dir_;
    int num_batches_;
    int batch_num_ = 0;
    bj::array entries_;
};

// Strips the date suffix off dated model ids for the per-model log folder.
std::string model_short(const std::string& m) {
    std::size_t p = m.find("-20");
    return p == std::string::npos ? m : m.substr(0, p);
}

}  // namespace

int translate_all(const TranslateOptions& opt) {
    log_info("Model: " + MODEL);
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) + " previous lines");

    // The gate comes first, before an API key is read and before a file is
    // removed: a run that cannot produce a correct answer must cost nothing
    // and leave nothing behind.
    log_info("\n--- Cast-roster gate (no tokens spent) ---");
    Bytes hcb;
    try {
        hcb = read_file(opt.hcb_file);
    } catch (const std::exception& e) {
        log_info(std::string("\n[ERROR] the gate cannot read ") + opt.hcb_file + ": " + e.what());
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }
    const SpeakerCheckReport gate = run_speaker_checks(hcb);
    if (!gate.passed) {
        log_info("\n[ERROR] cast check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }
    const auto line_speakers = map_line_speakers(hcb);

    // A fresh run drops what earlier runs left, so the table this run produces
    // holds exactly what this run answered.  It happens after the gate for the
    // same reason the gate comes first.
    // A run that discards the cache is certain to call the API, so the key is
    // required before anything is deleted rather than at the first batch.
    if (opt.fresh_run && gemini::load_api_key().empty()) {
        log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return 1;
    }

    if (opt.fresh_run)
        for (const std::string& p : {opt.cache_file, opt.output_file}) {
            // Reported, not thrown, at both steps: the throwing overloads name
            // no file, and this run is about to translate from scratch.  A
            // delete fails when another process holds the file open without
            // FILE_SHARE_DELETE, which scanners, indexers and sync clients
            // routinely do.
            const fs::path path = fs::u8path(p);
            std::error_code ec;
            const bool present = fs::exists(path, ec);
            if (!ec && !present) continue;  // never written: nothing to remove
            const bool removed = !ec && fs::remove(path, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + p + ": " + ec.message() +
                         " -- close whatever has it open, then retry.");
                return 1;
            }
            // A file that vanished between the two calls was not removed here.
            log_info(removed ? "Fresh run: removed " + p
                             : "Fresh run: " + p + " was already gone");
        }

    gemini::LazyClient client;

    log_info("\nLoading extracted text...");
    bj::value all_scripts_v = json_parse_file(opt.input_file);
    bj::object& all_scripts = all_scripts_v.get_object();

    log_info("Loading translation cache...");
    Cache cache;
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
    } else {
        cache = load_cache(opt.cache_file);
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    for (const auto& [jp, en] : name_translations_ordered())
        cache.set_default(jp, en);

    // Batch log dir: test runs get a per-model subfolder so a smoke run never
    // overwrites the last full run's log.
    const std::string log_dir =
        opt.test_mode ? opt.last_translate_dir + "\\" + model_short(MODEL)
                      : opt.last_translate_dir;
    fs::create_directories(fs::u8path(log_dir));
    if (opt.num_batches > 1) {
        for (int bi = 1; bi <= opt.num_batches; ++bi) {
            fs::path p = fs::u8path(log_dir + "\\last_translation" + std::to_string(bi) + ".json");
            if (fs::exists(p)) fs::remove(p);
        }
    } else {
        fs::path p = fs::u8path(log_dir + "\\last_translation.json");
        if (fs::exists(p)) fs::remove(p);
    }
    BatchLogger batch_log(log_dir, opt.num_batches);

    // Work list: every line (in stream order) that needs translation.
    bj::object& script = all_scripts.at("Sakura.hcb").get_object();
    bj::array& all_lines = script.at("lines").get_array();

    std::vector<bj::object*> work;
    for (auto& lv : all_lines) {
        auto& line = lv.get_object();
        auto* type = line.if_contains("type");
        if (!type || !type->is_string() || type->get_string() != "STRING") continue;
        if (!needs_translation(std::string(line.at("content").get_string()))) continue;
        work.push_back(&line);
    }
    log_info("\nTotal translatable lines: " + comma(static_cast<long long>(work.size())));

    auto lookup_cached = [&](const std::string& s) -> const std::string* {
        if (const std::string* en = cache.get(s)) return en;
        const std::string t = trim(s);
        if (t != s) return cache.get(t);
        return nullptr;
    };

    auto count_uncached = [&](const std::vector<bj::object*>& v) {
        std::size_t n = 0;
        for (auto* ln : v)
            if (!lookup_cached(std::string(ln->at("content").get_string()))) ++n;
        return n;
    };
    std::size_t uncached = count_uncached(work);
    log_info("  uncached: " + comma(static_cast<long long>(uncached)));

    auto format_with_speaker = [&](const bj::object& ln) -> std::string {
        std::string c(ln.at("content").get_string());
        if (const auto* off_v = ln.if_contains("offset")) {
            std::uint32_t off = 0;
            if (off_v->is_int64()) off = static_cast<std::uint32_t>(off_v->get_int64());
            else if (off_v->is_uint64()) off = static_cast<std::uint32_t>(off_v->get_uint64());
            auto it = line_speakers.find(off);
            if (it != line_speakers.end() && !it->second.empty() && it->second != "???")
                return "[" + it->second + "] " + c;
        }
        return c;
    };

    if (opt.test_mode) {
        const std::size_t target_lines =
            static_cast<std::size_t>(opt.num_batches) * static_cast<std::size_t>(opt.batch_size);
        log_info("  --test " + std::to_string(opt.num_batches) + ": targeting up to " +
                 std::to_string(std::min(uncached, target_lines)) + " uncached lines across " +
                 std::to_string(opt.num_batches) + " batch(es)");
    }

    if (uncached == 0) {
        save_cache(cache, opt.cache_file);
        log_info("\n  Everything is cached -- skipping API calls.");
    } else {
        std::vector<std::string> previous_context;
        int batches_remaining = opt.num_batches;
        auto start = std::chrono::steady_clock::now();
        long long done = 0;

        std::size_t i = 0;
        while (i < work.size()) {
            while (i < work.size()) {
                std::string c(work[i]->at("content").get_string());
                const std::string* en = lookup_cached(c);
                if (!en) break;
                previous_context.push_back(*en);
                ++i;
            }
            if (previous_context.size() > CONTEXT_WINDOW * 2)
                previous_context.erase(previous_context.begin(),
                                       previous_context.end() - CONTEXT_WINDOW * 2);
            if (i >= work.size()) break;

            const std::size_t batch_start = i;
            std::vector<std::string> api_lines;
            std::vector<std::string> prompt_lines;
            std::unordered_map<std::string, std::size_t> unique_idx;
            api_lines.reserve(static_cast<std::size_t>(opt.batch_size));
            prompt_lines.reserve(static_cast<std::size_t>(opt.batch_size));

            while (i < work.size() &&
                   api_lines.size() < static_cast<std::size_t>(opt.batch_size)) {
                std::string c(work[i]->at("content").get_string());
                if (!lookup_cached(c) && unique_idx.find(c) == unique_idx.end()) {
                    unique_idx.emplace(c, api_lines.size());
                    api_lines.push_back(c);
                    prompt_lines.push_back(format_with_speaker(*work[i]));
                }
                ++i;
            }
            const std::size_t batch_end = i;
            if (api_lines.empty()) break;

            std::vector<std::string> context_after;
            const std::size_t after_end =
                std::min(work.size(), batch_end + static_cast<std::size_t>(CONTEXT_AFTER));
            context_after.reserve(after_end - batch_end);
            for (std::size_t k = batch_end; k < after_end; ++k)
                context_after.push_back(format_with_speaker(*work[k]));

            // The first batch the cache cannot cover is where a key becomes
            // required.  Outside the try below on purpose: a missing key has
            // to stop the run, not be caught as a batch error and written to
            // the cache as the Japanese translating to itself.
            gemini::Client& api = client.get();
            print_context_window(previous_context);

            // Counted as the answers arrive, not from the batch size: a line
            // the model did not return stays queued, and a queued line is not
            // progress.
            long long translated_now = 0;
            try {
                auto translations =
                    call_gemini(api, prompt_lines, previous_context, context_after);
                if (translations.empty() &&
                    (!previous_context.empty() || !context_after.empty())) {
                    log_info("    Empty response with context window -- retrying batch without context");
                    translations = call_gemini(api, prompt_lines, {}, {});
                }
                if (translations.size() < api_lines.size())
                    log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                             std::to_string(api_lines.size()) + " translations");

                std::vector<std::string> resolved_en(api_lines.size());
                for (std::size_t j = 0; j < api_lines.size(); ++j) {
                    auto it = translations.find(static_cast<int>(j) + 1);
                    if (it != translations.end()) resolved_en[j] = strip(it->second);
                    if (resolved_en[j].empty()) {
                        log_info("    Missing translation for line " + std::to_string(j + 1) +
                                 " -- leaving it queued");
                    } else {
                        cache.set(api_lines[j], resolved_en[j]);
                    }
                }

                for (std::size_t k = batch_start; k < batch_end; ++k) {
                    bj::object* ln = work[k];
                    const std::string jp(ln->at("content").get_string());
                    auto uit = unique_idx.find(jp);
                    if (uit != unique_idx.end()) {
                        const std::string& en = resolved_en[uit->second];
                        if (en.empty()) {
                            previous_context.push_back(jp);
                            continue;
                        }
                        previous_context.push_back(en);
                        ++translated_now;
                        bj::object entry;
                        entry["line_idx"] = ln->at("idx");
                        entry["offset"] = ln->at("offset");
                        entry["japanese"] = jp;
                        entry["translated"] = en;
                        batch_log.append(std::move(entry));
                    } else if (const std::string* cached_en = lookup_cached(jp)) {
                        previous_context.push_back(*cached_en);
                    }
                }
            } catch (const gemini::RateLimitError&) {
                // Back off, then move on to the next batch -- this one is
                // picked up again by the next resume run.
                log_info("    Rate limited, waiting 60s...");
                std::this_thread::sleep_for(std::chrono::seconds(60));
                continue;
            } catch (const std::exception& e) {
                // Nothing is written: the batch stays uncached, so the next
                // run asks for exactly these lines again.
                log_info(std::string("    Batch error: ") + e.what());
            }

            if (previous_context.size() > CONTEXT_WINDOW * 2)
                previous_context.erase(previous_context.begin(),
                                       previous_context.end() - CONTEXT_WINDOW * 2);
            save_cache(cache, opt.cache_file);
            done += translated_now;
            batch_log.next_batch();

            if (opt.test_mode) {
                --batches_remaining;
                if (batches_remaining <= 0) {
                    log_info("\n  --test mode: stopping after " +
                             std::to_string(opt.num_batches) + " batch(es)");
                    break;
                }
            }

            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            double rate = elapsed > 0 ? static_cast<double>(done) / elapsed : 0.0;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  progress: %lld translated  (%.1f l/s)", done, rate);
            log_info(buf);
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done. Cache: " + std::to_string(cache.size()) + " entries");
    }

    // Build final translated_text.json
    log_info("\nBuilding translated_text.json...");
    for (auto& lv : all_lines) {
        auto& line = lv.get_object();
        std::string c(line.at("content").get_string());
        const std::string* en = lookup_cached(c);
        line["translated"] = (en && *en != c) ? strip_speaker_tags(*en) : c;
    }

    write_file(opt.output_file, json_dump(all_scripts_v));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) / 1024.0 /
                      1024.0);
    log_info("Saved " + opt.output_file + " (" + sz + ")");

    // The table the game loads is derived from the document that was just
    // written, so it is built here rather than by a second command someone has
    // to remember: a translation the game never sees is not a translation.
    print_line("");
    log_info("Building the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.cache_file, opt.tsv_file);
}

}  // namespace fav::translate
