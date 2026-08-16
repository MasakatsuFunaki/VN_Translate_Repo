// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace translator_logic {

// --- Low-level CP932 helpers --------------------------------------------

bool IsCp932LeadByte(unsigned char b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool HasJPLeadByte(const std::string& s)
{
    for (unsigned char c : s) {
        if (IsCp932LeadByte(c)) return true;
    }
    return false;
}

bool IsCp932Terminator(unsigned char a, unsigned char b)
{
    // 0x81 0x42 = 。  fullwidth period
    // 0x81 0x48 = ？  fullwidth question mark
    // 0x81 0x49 = ！  fullwidth exclamation
    // 0x81 0x76 = 」  close corner bracket (quote end)
    // 0x81 0x63 = …  ellipsis (horizontal)
    return a == 0x81 &&
           (b == 0x42 || b == 0x48 || b == 0x49 || b == 0x76 || b == 0x63);
}

bool IsCp932EntryStart(unsigned char a, unsigned char b)
{
    // 0x81 0x75 = 「  open corner bracket (speaker -> quote transition)
    // 0x81 0x40 = 　  fullwidth space (entry separator in compound buf38)
    if (a == 0x81 && (b == 0x75 || b == 0x40)) return true;
    if (a == '\n') { (void)b; return true; }
    return false;
}

// --- String helpers ------------------------------------------------------

void UnescapeInPlace(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            if (c == 'r')  { out += '\r'; i++; continue; }
            if (c == 'n')  { out += '\n'; i++; continue; }
            if (c == 't')  { out += '\t'; i++; continue; }
            if (c == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    s = std::move(out);
}

std::string SanitizeEn(const std::string& raw)
{
    std::string s = raw;
    for (char& c : s) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return s;
}

bool IsPrefix(const std::string& needle, const std::string& haystack)
{
    return needle.size() <= haystack.size() &&
           std::memcmp(needle.data(), haystack.data(), needle.size()) == 0;
}

// --- TSV loading ---------------------------------------------------------

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out)
{
    int count = 0;
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        size_t line_end = i;
        // Skip the line terminator(s).
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;

        // Find the tab inside the line.
        size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') tab++;
        if (tab == line_start || tab >= line_end) continue;

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) continue;
        if (en == "(garbled data)") continue;
        out[jp] = en;
        count++;
    }
    return count;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    std::vector<char> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0) return false;
    int added = ParseTsvBuffer(buf.data(), n, out);
    return added > 0;
}

// --- Speaker/quote split -------------------------------------------------

int SplitSpeakerQuote(TranslationMap& map)
{
    const std::string kaku_open  = std::string("\x81\x75", 2);   // 「
    const std::string kaku_close = std::string("\x81\x76", 2);   // 」

    std::vector<std::pair<std::string, std::string>> adds;
    adds.reserve(map.size() / 8);

    for (const auto& kv : map) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (jp.size() < 4 || en.size() < 4) continue;

        size_t jpOpen = jp.find(kaku_open);
        if (jpOpen == std::string::npos || jpOpen == 0) continue;
        if (jp.compare(jp.size() - 2, 2, kaku_close) != 0) continue;

        size_t enOpen = en.find(kaku_open);
        if (enOpen == std::string::npos || enOpen == 0) continue;
        if (en.compare(en.size() - 2, 2, kaku_close) != 0) continue;

        adds.emplace_back(jp.substr(0, jpOpen), en.substr(0, enOpen));
        adds.emplace_back(jp.substr(jpOpen),   en.substr(enOpen));
    }

    int added = 0;
    for (auto& p : adds) {
        if (p.first.empty() || p.second.empty()) continue;
        if (map.emplace(std::move(p.first), std::move(p.second)).second) {
            added++;
        }
    }
    return added;
}

// --- Segmenter -----------------------------------------------------------

std::vector<Segment> SegmentMessage(const std::string& msg,
                                    const TranslationMap& map)
{
    std::vector<Segment> out;
    size_t pos = 0;
    while (pos + 2 <= msg.size()) {
        size_t hitLen = 0;
        const std::string* hitEn = nullptr;

        // Scan from longest remaining length down to 2 bytes in 2-byte
        // steps (CP932 glyphs are 2 bytes).
        for (size_t len = msg.size() - pos; len >= 2; len -= 2) {
            auto it = map.find(msg.substr(pos, len));
            if (it == map.end()) continue;

            // BOUNDARY GATE: the match must represent a complete YSTB
            // entry. See translator_logic.h for the full rationale.
            bool consumesRest = (pos + len == msg.size());
            bool endsAtTerminator =
                IsCp932Terminator(static_cast<unsigned char>(msg[pos + len - 2]),
                                  static_cast<unsigned char>(msg[pos + len - 1]));
            bool followedByEntryStart =
                (pos + len + 2 <= msg.size()) &&
                IsCp932EntryStart(static_cast<unsigned char>(msg[pos + len]),
                                  static_cast<unsigned char>(msg[pos + len + 1]));
            // ALSO accept matches that are followed by a terminator
            // (sentence end / close-bracket). Without this, an entry
            // like "芽生、ご苦労様" (no trailing 」 in the TSV key) would
            // be rejected when buf38 has it followed by 」 — even though
            // 」 IS a clean entry boundary.
            bool followedByTerminator =
                (pos + len + 2 <= msg.size()) &&
                IsCp932Terminator(static_cast<unsigned char>(msg[pos + len]),
                                  static_cast<unsigned char>(msg[pos + len + 1]));
            if (!consumesRest && !endsAtTerminator
                && !followedByEntryStart && !followedByTerminator)
                continue;

            hitLen = len;
            hitEn = &it->second;
            break;
        }

        if (hitLen == 0) {
            pos += 2;  // no valid match at this position, skip one glyph
            continue;
        }
        out.push_back({msg.substr(pos, hitLen), *hitEn});
        pos += hitLen;
    }
    return out;
}

// --- buf38 reconstruction ------------------------------------------------

std::string ReconstructJpFromBuffers(const char* buf38, size_t buf38_cap,
                                     const char* buf3c, size_t buf3c_cap,
                                     int opcount)
{
    std::string jp;
    if (!buf38 || !buf3c || opcount <= 0) return jp;
    if ((size_t)opcount > buf3c_cap)      return jp;
    jp.reserve((size_t)opcount * 2);
    size_t byte_pos = 0;
    for (int i = 0; i < opcount; i++) {
        unsigned char op = (unsigned char)buf3c[i];
        if (op == 0x31) {
            if (byte_pos >= buf38_cap) break;
            jp.push_back(buf38[byte_pos++]);
        } else if (op == 0x32) {
            if (byte_pos + 1 >= buf38_cap) break;
            jp.push_back(buf38[byte_pos]);
            jp.push_back(buf38[byte_pos + 1]);
            byte_pos += 2;
        }
        // 0x43 / 0x45 / 0x50 / 0x52 / 0x70 / 0x72 — non-text opcodes.
        // The engine's renderer does NOT advance the text cursor for
        // these, so we don't either; otherwise our reconstructed
        // string would have stray bytes that don't match the TSV key
        // the JP-extractor recorded.
    }
    return jp;
}

// --- Rendering pipeline (per-glyph-slot chunk plan) ---------------------

int CountCp932Glyphs(const std::string& s)
{
    // Counts only CP932 (2-byte) glyphs. ASCII bytes don't drive the
    // hooked TextOutA path (the hook returns early for `c != 2`), so
    // they don't consume plan chunks and shouldn't contribute to the
    // glyph count the plan length is checked against.
    int n = 0;
    for (size_t i = 0; i + 1 < s.size(); ) {
        unsigned char a = (unsigned char)s[i];
        if ((a >= 0x81 && a <= 0x9F) || (a >= 0xE0 && a <= 0xFC)) {
            n++; i += 2;
        } else {
            i += 1;
        }
    }
    return n;
}

std::string FlattenEnToAscii(const std::string& en)
{
    std::string out;
    out.reserve(en.size());
    for (size_t i = 0; i < en.size(); i++) {
        unsigned char c = (unsigned char)en[i];
        if (c >= 0x20 && c <= 0x7E) {
            out += (char)c;
        } else if (c == 0x81 && i + 1 < en.size()) {
            unsigned char c2 = (unsigned char)en[i + 1];
            if      (c2 == 0x75) out += '"';   // open kakko
            else if (c2 == 0x76) out += '"';   // close kakko
            else if (c2 == 0x42) out += '.';   // fullwidth period
            else if (c2 == 0x40) out += ' ';   // fullwidth space
            else                 out += ' ';
            i++;
        }
        // Drop anything else.
    }
    return out;
}

void SpreadEnAcrossSlots(const std::string& en, int slots, int chars_per_slot,
                         std::vector<std::string>& out)
{
    if (slots <= 0 || chars_per_slot <= 0) return;
    int budget = slots * chars_per_slot;
    int take = (int)en.size() < budget ? (int)en.size() : budget;
    int produced = 0;
    for (int i = 0; i + chars_per_slot <= take; i += chars_per_slot) {
        out.emplace_back(en.substr((size_t)i, (size_t)chars_per_slot));
        produced++;
    }
    int leftover = take - produced * chars_per_slot;
    if (leftover > 0) {
        std::string last = en.substr((size_t)(produced * chars_per_slot),
                                     (size_t)leftover);
        last.append((size_t)(chars_per_slot - leftover), ' ');
        out.push_back(std::move(last));
        produced++;
    }
    std::string pad((size_t)chars_per_slot, ' ');
    for (int i = produced; i < slots; i++) {
        out.push_back(pad);
    }
}

void ResplitSpeakerQuote(std::vector<Segment>& segs,
                         const TranslationMap& map)
{
    const std::string kaku_open  = std::string("\x81\x75", 2);  // open kakko
    std::vector<Segment> out;
    out.reserve(segs.size() * 2);
    for (auto& seg : segs) {
        size_t open = seg.jp.find(kaku_open);
        if (open == std::string::npos || open == 0) {
            out.push_back(std::move(seg));
            continue;
        }
        std::string speaker_jp = seg.jp.substr(0, open);
        std::string quote_jp   = seg.jp.substr(open);
        auto sit = map.find(speaker_jp);
        auto qit = map.find(quote_jp);

        // Best case: both pieces are standalone keys.
        if (sit != map.end() && qit != map.end()) {
            out.push_back({speaker_jp, sit->second});
            out.push_back({quote_jp,   qit->second});
            continue;
        }

        // Fallback A: speaker is a key but quote isn't (extraction may
        // have dropped the closing close-kakko, so quote_jp isn't a TSV
        // key). Use speaker's known EN, treat the rest of the combined
        // EN as the quote EN.
        if (sit != map.end()) {
            const std::string& speaker_en = sit->second;
            size_t en_pos = seg.en.find(speaker_en);
            if (en_pos != std::string::npos) {
                std::string quote_en = seg.en.substr(en_pos + speaker_en.size());
                if (!quote_en.empty()) {
                    out.push_back({speaker_jp, speaker_en});
                    out.push_back({quote_jp,   std::move(quote_en)});
                    continue;
                }
            }
        }

        // Fallback B: split combined EN at first open-kakko or ASCII ".
        size_t en_split = seg.en.find(kaku_open);
        if (en_split == std::string::npos) en_split = seg.en.find('"');
        if (en_split != std::string::npos && en_split > 0) {
            out.push_back({speaker_jp, seg.en.substr(0, en_split)});
            out.push_back({quote_jp,   seg.en.substr(en_split)});
            continue;
        }

        // No split possible — keep combined.
        out.push_back(std::move(seg));
    }
    segs = std::move(out);
}

// Internal: walk JP glyph-by-glyph and label every CP932 slot with
// which segment owns it (or -1 for gap). Returns regions plus the
// per-segment [start, own_end) ranges. ASCII bytes don't fire the
// c=2 TextOutA hook and so don't produce a slot.
namespace {
struct Region {
    int  seg_idx;     // -1 for gap
    bool is_hidden;   // CP932 / (0x81 0x5E) speaker-hidden marker
};
struct Layout {
    std::vector<Region> regions;
    std::vector<size_t> seg_start;    // SIZE_MAX if seg has no slots
    std::vector<size_t> seg_own_end;  // exclusive
};

Layout LayOutSegments(const std::string& jp,
                      const std::vector<Segment>& segs)
{
    Layout L;
    size_t cursor = 0;
    size_t seg_idx = 0;
    while (cursor + 1 < jp.size()) {
        bool matched = false;
        if (seg_idx < segs.size()) {
            const auto& seg = segs[seg_idx];
            if (!seg.jp.empty()
                && cursor + seg.jp.size() <= jp.size()
                && std::memcmp(jp.data() + cursor,
                               seg.jp.data(), seg.jp.size()) == 0) {
                int n_slots = CountCp932Glyphs(seg.jp);
                if (n_slots > 0) {
                    bool hidden = (seg.jp.size() >= 2
                                && (unsigned char)seg.jp[0] == 0x81
                                && (unsigned char)seg.jp[1] == 0x5E);
                    for (int i = 0; i < n_slots; i++) {
                        L.regions.push_back({(int)seg_idx, hidden});
                    }
                    cursor += seg.jp.size();
                    seg_idx++;
                    matched = true;
                }
            }
        }
        if (!matched) {
            unsigned char a = (unsigned char)jp[cursor];
            bool is_cp932 = (a >= 0x81 && a <= 0x9F)
                         || (a >= 0xE0 && a <= 0xFC);
            if (is_cp932) {
                L.regions.push_back({-1, false});
                cursor += 2;
            } else {
                cursor += 1;
            }
        }
    }
    L.seg_start.assign(segs.size(), SIZE_MAX);
    L.seg_own_end.assign(segs.size(), 0);
    for (size_t i = 0; i < L.regions.size(); i++) {
        int s = L.regions[i].seg_idx;
        if (s >= 0) {
            if (L.seg_start[(size_t)s] == SIZE_MAX) L.seg_start[(size_t)s] = i;
            L.seg_own_end[(size_t)s] = i + 1;
        }
    }
    return L;
}

// Extended slot count for segment `s`: own slots plus trailing gap
// slots, until the next segment. Returns 0 if the seg has no slots.
size_t ExtendedSlotsForSeg(const Layout& L, size_t s)
{
    if (L.seg_start[s] == SIZE_MAX) return 0;
    size_t ext_end = L.seg_own_end[s];
    while (ext_end < L.regions.size() && L.regions[ext_end].seg_idx == -1) {
        ext_end++;
    }
    return ext_end - L.seg_start[s];
}
}  // namespace

// Pack-tight planner. Walks visible segments left-to-right, deciding
// where each segment's EN should land in the chunk array.
//
// Default rule: pack tight. The next segment starts at the slot
// immediately after the previous one ended (with a leading separator
// space prepended so consecutive sentences don't visually butt
// together; trailing whitespace gets trimmed first so we don't
// produce fully-blank end chunks).
//
// Speaker→quote exception: when the FIRST visible segment is itself
// a TSV key (a known character name like 美桜→Mio) AND the SECOND
// visible segment's JP starts with open-kakko `「`, the engine renders
// the speaker as a separate label above the dialog body. Packing the
// body into the speaker slots would land dialog bytes on top of the
// speaker label. In that case the body segment starts at its natural
// JP slot (the `「` position), without the separator-space prefix.
namespace {
std::vector<PlacedSeg> ComputePackPlanInternal(
    const std::vector<Segment>& segs,
    const Layout& L,
    const TranslationMap& map,
    int chars_per_slot)
{
    std::vector<PlacedSeg> out;
    if (chars_per_slot <= 0) return out;
    size_t cursor = 0;
    size_t placed_count = 0;
    size_t prev_placed_idx = SIZE_MAX;
    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) continue;
        if (L.regions[L.seg_start[s]].is_hidden) continue;
        std::string en = FlattenEnToAscii(segs[s].en);
        while (!en.empty() && en.back() == ' ') en.pop_back();
        if (en.empty()) continue;
        if (placed_count == 0) {
            cursor = L.seg_start[s];
        }
        bool starts_with_kakko = (segs[s].jp.size() >= 2
            && (unsigned char)segs[s].jp[0] == 0x81
            && (unsigned char)segs[s].jp[1] == 0x75);
        bool break_pack = false;
        if (placed_count == 1 && starts_with_kakko) {
            const Segment& prev = segs[prev_placed_idx];
            if (map.find(prev.jp) != map.end()) break_pack = true;
        }
        size_t place_at;
        if (placed_count == 0) {
            place_at = cursor;
        } else if (break_pack) {
            place_at = std::max(cursor, L.seg_start[s]);
        } else {
            while (!en.empty() && en.front() == ' ') en.erase(0, 1);
            if (en.empty()) continue;
            en.insert(en.begin(), ' ');
            place_at = cursor;
        }
        size_t en_slots = (en.size() + chars_per_slot - 1) / chars_per_slot;
        out.push_back({s, std::move(en), place_at, en_slots});
        cursor = place_at + en_slots;
        prev_placed_idx = s;
        placed_count++;
    }
    return out;
}

bool PlanFits(const std::vector<PlacedSeg>& plan, size_t total_slots)
{
    for (const auto& p : plan) {
        if (p.place_at + p.en_slots > total_slots) return false;
    }
    return true;
}
}  // namespace

std::vector<PlacedSeg>
ComputePackPlan(const std::string& jp, const TranslationMap& map,
                int chars_per_slot)
{
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);
    Layout L = LayOutSegments(jp, segs);
    return ComputePackPlanInternal(segs, L, map, chars_per_slot);
}

void BuildRenderPlan(const std::string& jp, const TranslationMap& map,
                     int chars_per_slot,
                     std::vector<std::string>& chunks)
{
    chunks.clear();
    if (chars_per_slot <= 0) return;
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);

    Layout L = LayOutSegments(jp, segs);

    const std::string blank((size_t)chars_per_slot, ' ');
    chunks.assign(L.regions.size(), blank);

    auto plan = ComputePackPlanInternal(segs, L, map, chars_per_slot);
    if (PlanFits(plan, L.regions.size()) && !plan.empty()) {
        for (const auto& p : plan) {
            std::vector<std::string> spread;
            SpreadEnAcrossSlots(p.en, (int)p.en_slots, chars_per_slot, spread);
            for (size_t i = 0; i < spread.size() && p.place_at + i < chunks.size(); i++) {
                chunks[p.place_at + i] = std::move(spread[i]);
            }
        }
        return;
    }

    // Doesn't fit pack-tight at this cps: fall back to the original
    // per-segment layout — each segment renders at its own JP slot
    // start with overflow allowed into the trailing gap (but stopped
    // at the next segment's slot). Some content will truncate, but
    // every segment retains its visual position so the user can at
    // least tell which segment got cut. The auto-fit logic should
    // narrow chars_per_slot until pack-tight fits, so this fallback
    // only fires for genuinely-too-long messages at the max cps.
    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) continue;
        size_t start = L.seg_start[s];
        size_t ext_slots = ExtendedSlotsForSeg(L, s);
        if (L.regions[start].is_hidden) continue;
        std::string en = FlattenEnToAscii(segs[s].en);
        std::vector<std::string> spread;
        SpreadEnAcrossSlots(en, (int)ext_slots, chars_per_slot, spread);
        for (size_t i = 0; i < spread.size() && i < ext_slots; i++) {
            chunks[start + i] = std::move(spread[i]);
        }
    }
}

std::vector<OverflowedSegment>
DetectOverflows(const std::string& jp, const TranslationMap& map,
                int chars_per_slot)
{
    std::vector<OverflowedSegment> out;
    if (chars_per_slot <= 0) return out;
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);

    Layout L = LayOutSegments(jp, segs);

    // Mirror BuildRenderPlan's pack-tight test: if the planner can
    // place every visible segment within total slots, nothing overflows.
    auto plan = ComputePackPlanInternal(segs, L, map, chars_per_slot);
    if (PlanFits(plan, L.regions.size())) {
        return out;
    }

    // Fallback layout: per-segment with spillover into trailing gap.
    // Report any segment whose EN exceeds its extended budget.
    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) continue;
        if (L.regions[L.seg_start[s]].is_hidden) continue;
        size_t ext_slots = ExtendedSlotsForSeg(L, s);
        std::string en = FlattenEnToAscii(segs[s].en);
        int budget = (int)ext_slots * chars_per_slot;
        if ((int)en.size() > budget) {
            out.push_back({segs[s].jp, std::move(en),
                           (int)ext_slots, (int)segs[s].en.size()});
            out.back().chars_needed = (int)out.back().seg_en.size();
        }
    }
    return out;
}

int ComputeAutoFitCharsPerSlot(const std::string& jp,
                               const TranslationMap& map,
                               int min_cps, int max_cps)
{
    if (max_cps < min_cps) return min_cps;
    // Smallest cps in [min, max] for which DetectOverflows is empty.
    // DetectOverflows uses the same pack-tight model BuildRenderPlan
    // does, so picking on it guarantees the chosen cps renders without
    // truncation AND without inter-segment gaps.
    for (int cps = min_cps; cps <= max_cps; cps++) {
        if (DetectOverflows(jp, map, cps).empty()) return cps;
    }
    return max_cps;
}

// --- Full-line English reconstruction (overlay renderer) -----------------

namespace {
// Trim leading/trailing spaces and collapse internal runs to one space.
std::string CollapseSpaces(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;  // strips leading spaces too
    for (char c : s) {
        bool is_space = (c == ' ');
        if (is_space && prev_space) continue;
        out += c;
        prev_space = is_space;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}  // namespace

EnglishLine ReconstructEnglish(const std::string& jp, const TranslationMap& map)
{
    EnglishLine out;
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);
    if (segs.empty()) return out;  // valid stays false -> caller falls through

    // Leading speaker label: first segment is itself a known TSV key
    // (a character name) AND the next segment's JP starts with open-kakko.
    // Same shape ComputePackPlanInternal uses for its speaker exception.
    size_t body_start = 0;
    if (segs.size() >= 2
        && map.find(segs[0].jp) != map.end()
        && segs[1].jp.size() >= 2
        && (unsigned char)segs[1].jp[0] == 0x81
        && (unsigned char)segs[1].jp[1] == 0x75) {
        out.speaker       = CollapseSpaces(FlattenEnToAscii(segs[0].en));
        out.speaker_slots = CountCp932Glyphs(segs[0].jp);
        body_start        = 1;
    }

    std::string body;
    for (size_t i = body_start; i < segs.size(); i++) {
        std::string e = FlattenEnToAscii(segs[i].en);
        if (e.empty()) continue;
        if (!body.empty()) body += ' ';
        body += e;
    }
    out.body  = CollapseSpaces(body);
    out.valid = !out.body.empty();
    return out;
}

}  // namespace translator_logic
