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

#include <boost/regex.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

namespace ama::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {
constexpr const char* OPEN_BRACKET = "\xE3\x80\x8C";   // 「
constexpr const char* CLOSE_BRACKET = "\xE3\x80\x8D";  // 」
}  // namespace

bool needs_translation(const std::string& text) {
    if (text.empty()) return false;
    if (text[0] == '$') return false;
    return is_jp(text);
}

std::string strip(const std::string& s) { return trim(s); }

std::string get_speaker_tag(const std::string& name_content) {
    if (name_content.empty() || name_content[0] == '$') return "NARRATION";
    const auto& map = name_translations();
    auto it = map.find(name_content);
    return it == map.end() ? name_content : it->second;
}

std::vector<MessageRun> extract_message_runs(const bj::object& script) {
    std::vector<MessageRun> out;
    const auto* lines_v = script.if_contains("lines");
    if (!lines_v || !lines_v->is_array()) return out;

    std::string current_speaker = "NARRATION";
    bool in_dialogue = false;

    const auto& lines = lines_v->get_array();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i].get_object();
        const std::string type(line.at("type").get_string());
        std::string content;
        if (auto* c = line.if_contains("content"))
            if (c->is_string()) content = std::string(c->get_string());

        if (type == "NAME") {
            current_speaker = (!content.empty() && content[0] == '$')
                                  ? std::string("NARRATION")
                                  : get_speaker_tag(content);
            in_dialogue = false;
        } else if (type == "COMMAND" && content == "fw 0") {
            // CatSystem2: bare `fw 0` hides the face window = back to narration.
            current_speaker = "NARRATION";
            in_dialogue = false;
        } else if (type == "MESSAGE") {
            if (content.empty() || !needs_translation(content)) continue;
            // Strip leading \n (the literal two-character escape, not a real
            // newline) to check for dialogue brackets.
            std::string clean = content;
            while (clean.rfind("\\n", 0) == 0) clean = clean.substr(2);
            // Opening bracket 「 starts character dialogue.
            if (clean.rfind(OPEN_BRACKET, 0) == 0) in_dialogue = true;
            // Only attribute to a character during bracketed dialogue;
            // unbracketed lines after a NAME are narration/internal monologue.
            const std::string effective_speaker =
                (current_speaker != "NARRATION" && !in_dialogue) ? "NARRATION"
                                                                 : current_speaker;
            // Closing bracket 」 ends character dialogue.
            if (content.find(CLOSE_BRACKET) != std::string::npos) in_dialogue = false;
            out.push_back(MessageRun{static_cast<int>(i), effective_speaker, content});
        }
    }
    return out;
}

std::string strip_speaker_tags(const std::string& text) {
    static const boost::regex re(R"(\[(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!]+)\]\s*)");
    return strip(boost::regex_replace(text, re, ""));
}

std::string fix_split_dialogue(const std::string& original, const std::string& translated) {
    // In CatSystem2, 「」 dialogue can span multiple MESSAGE lines separated by
    // \@ page breaks.  Claude wraps each line in quotes independently, which
    // doubles them at the boundary.
    const bool has_open = original.find(OPEN_BRACKET) != std::string::npos;
    const bool has_close = original.find(CLOSE_BRACKET) != std::string::npos;

    std::string t = translated;
    if (has_open && !has_close) {
        // Opening dialogue line (continues on the next page).
        if (!t.empty() && t.back() == '"') {
            t.pop_back();
            t = trim_right(t);
        }
    } else if (has_close && !has_open) {
        // Continuation / closing dialogue line.
        if (!t.empty() && t.front() == '"') t = trim_left(t.substr(1));
    }
    return t;
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
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items())
        o[jp] = en;
    write_file_text(cache_file, json_pretty(bj::value(std::move(o)), 2));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
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

void apply_line_result(Cache& cache, std::map<int, std::string>& results,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       int idx, const std::string& speaker, const std::string& jp,
                       const std::string& suffix, const std::string& en) {
    // An answer that never arrived is not a translation.  Recording the
    // Japanese as its own English would cache it and put it in the run log as
    // though it had been translated, and no later run would ask for the line
    // again.  The context window still gets the Japanese, so the next request
    // reads in sequence.
    if (en.empty()) {
        previous_context.emplace_back(speaker, jp);
        return;
    }
    const std::string tr = fix_split_dialogue(jp, en) + suffix;
    results[idx] = tr;
    cache.set(jp, tr);
    previous_context.emplace_back(speaker, en);
}

// --------------------------------------------------------------------------- //
// API call
// --------------------------------------------------------------------------- //

namespace {

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    const std::size_t n = std::min<std::size_t>(previous_context.size(), CONTEXT_WINDOW);
    log_info("\n=== ROLLING CONTEXT WINDOW (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of file)");
    } else {
        for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i) {
            const auto& [spk, txt] = previous_context[i];
            std::string flat;
            for (std::size_t k = 0; k < txt.size(); ++k) {
                if (txt.compare(k, 2, "\r\n") == 0) { flat += '|'; ++k; }
                else if (txt[k] == '\n') flat += '|';
                else flat += txt[k];
            }
            if (flat.size() > 120) flat.resize(120);
            log_info("  [" + spk + "] " + flat);
        }
    }
    log_info("=== END CONTEXT ===\n");
}

}  // namespace

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                      ? previous_context.size() - CONTEXT_WINDOW
                                      : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i)
            user_prompt += "[" + previous_context[i].first + "] " +
                           previous_context[i].second + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i)
        user_prompt += std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " +
                       batch_lines[i].second + "\n";
    user_prompt += "</lines_to_translate>\n\n";
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use the <previous_context> (if present) for scene continuity but do NOT translate it. "
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

std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = std::max<std::int64_t>(
        static_cast<std::int64_t>(batch_lines.size()) * 300, 16384);
    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] = SYSTEM_PROMPT;
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    body["system"] = bj::array{std::move(sys_block)};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context);
    body["messages"] = bj::array{std::move(msg)};

    if (MODEL == MODEL_SONNET)
        body["temperature"] = 0.3;
    // Reasoning-budget knob (per Anthropic docs): Opus 4.7 takes adaptive
    // thinking; Sonnet 4.6 / Opus 4.6 take output_config.effort.  The field
    // goes straight into the request body with nothing filtering it, so a model
    // that does not support the knob must never reach its branch.
    if (MODEL == MODEL_OPUS_47)
        body["thinking"] = bj::object{{"type", "adaptive"}};
    else if (MODEL == MODEL_SONNET || MODEL == MODEL_OPUS_46)
        body["output_config"] = bj::object{{"effort", EFFORT}};

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

    log_info("\n--- CLAUDE'S RAW ANSWER ---");
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
// Per-script translation
// --------------------------------------------------------------------------- //

namespace {

// Per-batch JSON log: entries accumulate per batch index and the whole batch is
// rewritten after every append, so a Ctrl+C still leaves the partial batch on
// disk.
class BatchLogger {
public:
    BatchLogger(std::string dir, int num_batches)
        : dir_(std::move(dir)), num_batches_(num_batches) {}

    void append(bj::object entry) {
        entries_.push_back(std::move(entry));
        ++total_;
        write_file_text(path(), json_pretty(bj::value(entries_), 2));
    }
    void next_batch() {
        ++batch_num_;
        if (!entries_.empty()) counts_.emplace_back(batch_num_ - 1, entries_.size());
        entries_.clear();
    }
    std::string path() const {
        return num_batches_ > 1
                   ? dir_ + "\\last_translation" + std::to_string(batch_num_ + 1) + ".json"
                   : dir_ + "\\last_translation.json";
    }
    std::size_t total() const { return total_; }
    // (batch index, entry count) for every batch that logged anything.
    const std::vector<std::pair<int, std::size_t>>& counts() const { return counts_; }
    void flush_current() {
        if (!entries_.empty()) counts_.emplace_back(batch_num_, entries_.size());
    }
    const std::string& dir() const { return dir_; }
    int num_batches() const { return num_batches_; }

private:
    std::string dir_;
    int num_batches_;
    int batch_num_ = 0;
    std::size_t total_ = 0;
    bj::array entries_;
    std::vector<std::pair<int, std::size_t>> counts_;
};

// Strip the date suffix off a dated model id ("claude-haiku-4-5-20251001" ->
// "claude-haiku-4-5"); used as a per-model log folder name.
std::string model_short(const std::string& m) {
    std::size_t p = m.find("-20");
    return p == std::string::npos ? m : m.substr(0, p);
}

struct PendingLine {
    int idx;
    std::string speaker;
    std::string content;
    std::string suffix;  // trailing \@ page break, split off before the API call
    std::string clean;   // content minus that suffix (leading \n is KEPT)
};

// Returns true when the run stopped early (test-mode batch budget hit).
bool translate_script(anthropic::LazyClient& client, const bj::object& script, Cache& cache,
                      int batch_size, const std::string& cache_file, bool test_mode,
                      const std::string& script_name, BatchLogger& batch_log,
                      int* batches_remaining) {
    std::vector<MessageRun> messages = extract_message_runs(script);
    if (messages.empty()) return false;

    std::map<int, std::string> results;
    std::vector<std::pair<std::string, std::string>> previous_context;  // (speaker, en)
    bool stopped_early = false;

    for (std::size_t batch_start = 0; batch_start < messages.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end =
            std::min(messages.size(), batch_start + static_cast<std::size_t>(batch_size));

        // Separate cached vs uncached, but keep all for context.  Leading \n is
        // deliberately KEPT in the text sent to Claude so it can recognise
        // continuation lines (soft wraps inside one text box) and translate them
        // coherently with the previous line.  Only the trailing \@ is split off.
        std::vector<PendingLine> needs_api;
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto& run = messages[i];
            if (const std::string* en = cache.get(run.content)) {
                results[run.line_idx] = *en;
                previous_context.emplace_back(run.speaker, *en);
                continue;
            }
            PendingLine p;
            p.idx = run.line_idx;
            p.speaker = run.speaker;
            p.content = run.content;
            p.clean = run.content;
            if (p.clean.size() >= 2 && p.clean.compare(p.clean.size() - 2, 2, "\\@") == 0) {
                p.suffix = "\\@";
                p.clean.resize(p.clean.size() - 2);
            }
            needs_api.push_back(std::move(p));
        }

        if (needs_api.empty()) continue;

        // The first batch the cache cannot cover is where a key becomes
        // required.  Outside the try below on purpose: that handler catches
        // every exception, so a missing key would be logged once per batch and
        // the run would still finish, leaving a table full of Japanese.
        anthropic::Client& api = client.get();

        std::vector<std::pair<std::string, std::string>> api_lines;
        api_lines.reserve(needs_api.size());
        for (const auto& p : needs_api) api_lines.emplace_back(p.speaker, p.clean);

        print_context_window(previous_context);

        auto apply = [&](const std::map<int, std::string>& translations) {
            for (std::size_t i = 0; i < needs_api.size(); ++i) {
                const auto& p = needs_api[i];
                auto it = translations.find(static_cast<int>(i) + 1);
                const std::string en = it == translations.end() ? std::string() : it->second;
                if (en.empty())
                    log_info("    Missing translation for line " + std::to_string(i + 1) +
                             " -- leaving it queued");
                apply_line_result(cache, results, previous_context, p.idx, p.speaker,
                                  p.content, p.suffix, en);
            }
        };

        // A batch that never produced an answer leaves its lines queued: no
        // cache entry, no row in the run log, and the next run asks for exactly
        // these lines again.
        auto leave_queued = [&] {
            for (const auto& p : needs_api)
                apply_line_result(cache, results, previous_context, p.idx, p.speaker,
                                  p.content, p.suffix, "");
        };

        try {
            auto translations = call_anthropic(api, api_lines, previous_context);
            if (translations.size() < needs_api.size())
                log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");
            apply(translations);
        } catch (const anthropic::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            try {
                apply(call_anthropic(api, api_lines, previous_context));
            } catch (const std::exception& e2) {
                log_info(std::string("    Retry failed: ") + e2.what() +
                         ", leaving this batch queued");
                leave_queued();
            }
        } catch (const std::exception& e) {
            log_info(std::string("    Batch error: ") + e.what());
            leave_queued();
        }

        if (previous_context.size() > CONTEXT_WINDOW * 2)
            previous_context.erase(previous_context.begin(),
                                   previous_context.end() - CONTEXT_WINDOW * 2);

        save_cache(cache, cache_file);

        // Log the whole batch, cached lines included: the log is a record of
        // what the run produced, not just of what went to the API.
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto& run = messages[i];
            auto it = results.find(run.line_idx);
            if (it == results.end()) continue;
            bj::object entry;
            entry["script"] = script_name;
            entry["line_idx"] = static_cast<std::int64_t>(run.line_idx);
            entry["type"] = "MESSAGE";
            entry["speaker"] = run.speaker;
            entry["japanese"] = run.content;
            entry["translated"] = it->second;
            batch_log.append(std::move(entry));
        }

        batch_log.next_batch();
        if (batches_remaining) --*batches_remaining;

        if (test_mode && batches_remaining && *batches_remaining <= 0) {
            stopped_early = true;
            break;
        }
    }

    return stopped_early;
}

}  // namespace

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

std::optional<std::string> validate_options(int batch_size, int test_n) {
    if (batch_size <= 0)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < 0)
        return "--test must not be negative (got " + std::to_string(test_n) + ")";
    return std::nullopt;
}

int translate_all(const TranslateOptions& opt) {
    log_info("Model: " + MODEL);
    if (is_effort_model(MODEL)) log_info("Effort: " + EFFORT);
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) + " previous lines");

    log_info("\nLoading extracted text...");
    bj::value all_scripts_v = json_parse_file(opt.input_file);
    bj::object& all_scripts = all_scripts_v.get_object();

    // The gate comes first, before an API key is read and before a file is
    // removed: a run that cannot produce a correct answer must cost nothing and
    // leave nothing behind.
    log_info("\n--- Speaker gate (no tokens spent) ---");
    const SpeakerCheckReport gate = run_speaker_checks(all_scripts);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }

    // A fresh run drops what earlier runs left, so the table this run produces
    // holds exactly what this run answered.  It happens after the gate for the
    // same reason the gate comes first.
    // A run that discards the cache is certain to call the API, so the key is
    // required before anything is deleted rather than at the first batch.
    if (opt.fresh_run && anthropic::load_api_key().empty()) {
        log_info("[ERROR] ANTHROPIC_API_KEY not set -- refusing to discard the "
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

    anthropic::LazyClient client;

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

    // Note: unconditional set (not setdefault) -- the glossary wins over any
    // cached translation of a bare name.
    for (const auto& [jp, en] : name_translations_ordered()) cache.set(jp, en);

    // (script_name, message_count, uncached), in stored order.
    std::vector<std::tuple<std::string, std::size_t, std::size_t>> scripts_with_messages;
    std::size_t total_messages = 0;
    for (const auto& kv : all_scripts) {
        const std::string script_name(kv.key());
        if (opt.only_script && script_name != *opt.only_script) continue;
        auto msgs = extract_message_runs(kv.value().get_object());
        if (msgs.empty()) continue;
        std::size_t uncached = 0;
        for (const auto& m : msgs)
            if (!cache.contains(m.content)) ++uncached;
        scripts_with_messages.emplace_back(script_name, msgs.size(), uncached);
        total_messages += msgs.size();
    }
    std::size_t total_uncached = 0;
    for (const auto& [n, c, u] : scripts_with_messages) total_uncached += u;

    log_info("\nTranslation summary:");
    log_info("  Scripts with dialogue: " + std::to_string(scripts_with_messages.size()));
    log_info("  Total MESSAGE lines: " + std::to_string(total_messages));
    log_info("  Need translating: " + std::to_string(total_uncached) +
             " (cached: " + std::to_string(total_messages - total_uncached) + ")");

    // Batch log dir: test runs get a per-model subfolder so different models
    // don't clobber each other.
    const std::string log_dir = opt.test_mode
                                    ? opt.last_translate_dir + "\\" + model_short(MODEL)
                                    : opt.last_translate_dir;
    fs::create_directories(fs::u8path(log_dir));
    if (opt.num_batches > 1) {
        for (int bi = 1; bi <= opt.num_batches; ++bi) {
            fs::path p =
                fs::u8path(log_dir + "\\last_translation" + std::to_string(bi) + ".json");
            if (fs::exists(p)) fs::remove(p);
        }
    } else {
        fs::path p = fs::u8path(log_dir + "\\last_translation.json");
        if (fs::exists(p)) fs::remove(p);
    }
    BatchLogger batch_log(log_dir, opt.num_batches);

    int batches_remaining = opt.num_batches;

    if (total_uncached == 0) {
        log_info("  Everything is already cached!");
    } else {
        log_info("\n--- Translating in story order ---");
        std::size_t done_lines = 0, done_scripts = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& [script_name, msg_count, uncached_count] : scripts_with_messages) {
            if (uncached_count == 0) {
                done_lines += msg_count;
                ++done_scripts;
                continue;
            }

            log_info("  Translating: " + script_name + " (" + std::to_string(uncached_count) +
                     " uncached / " + std::to_string(msg_count) + " total)");
            const bool stopped = translate_script(
                client, all_scripts.at(script_name).get_object(), cache, opt.batch_size,
                opt.cache_file, opt.test_mode, script_name, batch_log,
                opt.test_mode ? &batches_remaining : nullptr);

            done_lines += msg_count;
            ++done_scripts;

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            const double eta = rate > 0 ? (total_messages - done_lines) / rate : 0.0;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "  [%zu/%zu] %-30.30s %3zu lines | total: %zu/%zu (%zu%%) | %.1f l/s "
                          "| ETA: %.0fmin",
                          done_scripts, scripts_with_messages.size(), script_name.c_str(),
                          msg_count, done_lines, total_messages,
                          total_messages ? done_lines * 100 / total_messages : 0, rate,
                          eta / 60.0);
            log_info(buf);

            if (stopped) {
                log_info("\n  --test mode: stopping after " + std::to_string(opt.num_batches) +
                         " batch(es)");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) + " entries");
        batch_log.flush_current();
        if (batch_log.total()) {
            if (batch_log.num_batches() > 1) {
                for (const auto& [bi, n] : batch_log.counts())
                    log_info("Translation log: " + batch_log.dir() + "\\last_translation" +
                             std::to_string(bi + 1) + ".json (" + std::to_string(n) + " entries)");
            } else {
                log_info("Translation log: " + batch_log.dir() + "\\last_translation.json (" +
                         std::to_string(batch_log.total()) + " entries)");
            }
        }
    }

    // Build final output
    log_info("\nBuilding translated scripts...");
    const auto& names = name_translations();
    for (auto& kv : all_scripts) {
        auto* lines_v = kv.value().get_object().if_contains("lines");
        if (!lines_v || !lines_v->is_array()) continue;
        for (auto& lv : lines_v->get_array()) {
            auto& line = lv.get_object();
            const std::string type(line.at("type").get_string());
            std::string content;
            if (auto* c = line.if_contains("content"))
                if (c->is_string()) content = std::string(c->get_string());

            if (type == "NAME") {
                auto it = names.find(content);
                line["translated"] = it == names.end() ? content : it->second;
            } else if (type == "MESSAGE") {
                const std::string* en = cache.get(content);
                line["translated"] =
                    (en && *en != content) ? strip_speaker_tags(*en) : content;
            } else {
                line["translated"] = content;
            }
        }
    }

    write_file(opt.output_file, json_dump(all_scripts_v));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) / 1024.0 /
                      1024.0);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");

    // The runtime table, built from the document this run just wrote.  It is
    // the last thing the step does, so one invocation leaves both the
    // translated document and the table the game reads.
    log_info("\n--- Runtime translation table ---");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace ama::translate
