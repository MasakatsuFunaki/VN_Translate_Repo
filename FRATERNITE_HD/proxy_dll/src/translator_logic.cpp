// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr unsigned int kCp932CodePage = 932u;
constexpr unsigned char kCp932LeadRange1Min = 0x81u;
constexpr unsigned char kCp932LeadRange1Max = 0x9Fu;
constexpr unsigned char kCp932LeadRange2Min = 0xE0u;
constexpr unsigned char kCp932LeadRange2Max = 0xFCu;
constexpr unsigned char kAsciiPrintableMin = 0x20u;
constexpr unsigned char kAsciiPrintableMax = 0x7Eu;
constexpr unsigned char kBuf3cSingleByteTextOpcode = 0x31u;
constexpr unsigned char kBuf3cDoubleByteTextOpcode = 0x32u;
constexpr std::size_t kCp932CharBytes = 2u;
constexpr std::size_t kMinSpeakerQuoteBytes = 4u;
constexpr std::size_t kSpeakerQuoteReserveDivisor = 8u;
constexpr std::size_t kResplitReserveMultiplier = 2u;
constexpr std::size_t kMinSpeakerPlusBodySegments = 2u;
constexpr int kGapSegmentIndex = -1;

bool Utf8ToWide(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty()) {
        return false;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(needed));
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), needed);
    return written == needed;
}

std::string WideToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (needed <= 0 || used_default) {
        throw std::runtime_error("WideCharToMultiByte(CP932) failed");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, &used_default);
    if (written != needed || used_default) {
        throw std::runtime_error("WideCharToMultiByte(CP932) length mismatch");
    }
    return out;
}

struct Cp932Pair {
    unsigned char lead;
    unsigned char trail;
};

Cp932Pair MakeCp932Pair(std::wstring_view glyph) {
    const std::string encoded = WideToCp932(glyph);
    if (encoded.size() != kCp932CharBytes) {
        throw std::runtime_error("Expected 2-byte CP932 glyph");
    }
    return {
        static_cast<unsigned char>(encoded[0]),
        static_cast<unsigned char>(encoded[1]),
    };
}

bool MatchesCp932Pair(unsigned char a, unsigned char b, Cp932Pair p) {
    return a == p.lead && b == p.trail;
}

bool StartsWithCp932Pair(const std::string& s, Cp932Pair p) {
    return s.size() >= kCp932CharBytes &&
           static_cast<unsigned char>(s[0]) == p.lead &&
           static_cast<unsigned char>(s[1]) == p.trail;
}

struct Cp932Punct {
    std::string open_quote_str;
    std::string close_quote_str;
    Cp932Pair open_quote;
    Cp932Pair close_quote;
    Cp932Pair period;
    Cp932Pair question;
    Cp932Pair exclamation;
    Cp932Pair ellipsis;
    Cp932Pair ideographic_space;
    Cp932Pair fullwidth_slash;
};

const Cp932Punct& Punct() {
    static const Cp932Punct kPunct = {
        WideToCp932(L"「"),
        WideToCp932(L"」"),
        MakeCp932Pair(L"「"),
        MakeCp932Pair(L"」"),
        MakeCp932Pair(L"。"),
        MakeCp932Pair(L"？"),
        MakeCp932Pair(L"！"),
        MakeCp932Pair(L"…"),
        MakeCp932Pair(L"　"),
        MakeCp932Pair(L"／"),
    };
    return kPunct;
}

// Label every CP932 slot with its owning segment index (-1 = gap).
struct Region {
    int  seg_idx;
    bool is_hidden;
};
struct Layout {
    std::vector<Region> regions;
    std::vector<size_t> seg_start;    // SIZE_MAX if seg has no slots
    std::vector<size_t> seg_own_end;  // exclusive
};

Layout LayOutSegments(const std::string& jp,
                      const std::vector<Segment>& segs) {
    const Cp932Punct& punct = Punct();
    Layout L;
    size_t cursor = 0;
    size_t seg_idx = 0;
    while (cursor + 1 < jp.size()) {
        bool matched = false;
        if (seg_idx < segs.size()) {
            const auto& seg = segs[seg_idx];
            if (!seg.jp.empty() &&
                cursor + seg.jp.size() <= jp.size() &&
                std::memcmp(jp.data() + cursor, seg.jp.data(), seg.jp.size()) == 0) {
                const int n_slots = CountCp932Glyphs(seg.jp);
                if (n_slots > 0) {
                    const bool hidden = StartsWithCp932Pair(seg.jp, punct.fullwidth_slash);
                    for (int i = 0; i < n_slots; i++) {
                        L.regions.push_back({static_cast<int>(seg_idx), hidden});
                    }
                    cursor += seg.jp.size();
                    seg_idx++;
                    matched = true;
                }
            }
        }
        if (!matched) {
            const unsigned char a = static_cast<unsigned char>(jp[cursor]);
            if (IsCp932LeadByte(a)) {
                L.regions.push_back({kGapSegmentIndex, false});
                cursor += kCp932CharBytes;
            } else {
                cursor += 1;
            }
        }
    }
    L.seg_start.assign(segs.size(), SIZE_MAX);
    L.seg_own_end.assign(segs.size(), 0);
    for (size_t i = 0; i < L.regions.size(); i++) {
        const int s = L.regions[i].seg_idx;
        if (s >= 0) {
            if (L.seg_start[static_cast<size_t>(s)] == SIZE_MAX) {
                L.seg_start[static_cast<size_t>(s)] = i;
            }
            L.seg_own_end[static_cast<size_t>(s)] = i + 1;
        }
    }
    return L;
}

// Own slots plus trailing gap slots until the next segment.
size_t ExtendedSlotsForSeg(const Layout& L, size_t s) {
    if (L.seg_start[s] == SIZE_MAX) {
        return 0;
    }
    size_t ext_end = L.seg_own_end[s];
    while (ext_end < L.regions.size() && L.regions[ext_end].seg_idx == kGapSegmentIndex) {
        ext_end++;
    }
    return ext_end - L.seg_start[s];
}

std::vector<PlacedSeg> ComputePackPlanInternal(
    const std::vector<Segment>& segs,
    const Layout& L,
    const TranslationMap& map,
    int chars_per_slot) {
    const Cp932Punct& punct = Punct();
    std::vector<PlacedSeg> out;
    if (chars_per_slot <= 0) {
        return out;
    }
    size_t cursor = 0;
    size_t placed_count = 0;
    size_t prev_placed_idx = SIZE_MAX;
    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) {
            continue;
        }
        if (L.regions[L.seg_start[s]].is_hidden) {
            continue;
        }
        std::string en = FlattenEnToAscii(segs[s].en);
        while (!en.empty() && en.back() == ' ') {
            en.pop_back();
        }
        if (en.empty()) {
            continue;
        }
        if (placed_count == 0) {
            cursor = L.seg_start[s];
        }
        const bool starts_with_kakko = StartsWithCp932Pair(segs[s].jp, punct.open_quote);
        bool break_pack = false;
        if (placed_count == 1 && starts_with_kakko) {
            const Segment& prev = segs[prev_placed_idx];
            if (map.find(prev.jp) != map.end()) {
                break_pack = true;
            }
        }
        size_t place_at = 0;
        if (placed_count == 0) {
            place_at = cursor;
        } else if (break_pack) {
            place_at = std::max(cursor, L.seg_start[s]);
        } else {
            while (!en.empty() && en.front() == ' ') {
                en.erase(0, 1);
            }
            if (en.empty()) {
                continue;
            }
            en.insert(en.begin(), ' ');
            place_at = cursor;
        }
        const size_t en_slots =
            (en.size() + static_cast<size_t>(chars_per_slot) - 1) /
            static_cast<size_t>(chars_per_slot);
        out.push_back({s, std::move(en), place_at, en_slots});
        cursor = place_at + en_slots;
        prev_placed_idx = s;
        placed_count++;
    }
    return out;
}

bool PlanFits(const std::vector<PlacedSeg>& plan, size_t total_slots) {
    for (const auto& p : plan) {
        if (p.place_at + p.en_slots > total_slots) {
            return false;
        }
    }
    return true;
}

std::string CollapseSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;
    for (char c : s) {
        const bool is_space = (c == ' ');
        if (is_space && prev_space) {
            continue;
        }
        out += c;
        prev_space = is_space;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

bool IsCp932LeadByte(unsigned char b) {
    return (b >= kCp932LeadRange1Min && b <= kCp932LeadRange1Max) ||
           (b >= kCp932LeadRange2Min && b <= kCp932LeadRange2Max);
}

bool HasJPLeadByte(const std::string& s) {
    for (unsigned char c : s) {
        if (IsCp932LeadByte(c)) {
            return true;
        }
    }
    return false;
}

bool IsCp932Terminator(unsigned char a, unsigned char b) {
    const Cp932Punct& p = Punct();
    return MatchesCp932Pair(a, b, p.period) ||
           MatchesCp932Pair(a, b, p.question) ||
           MatchesCp932Pair(a, b, p.exclamation) ||
           MatchesCp932Pair(a, b, p.close_quote) ||
           MatchesCp932Pair(a, b, p.ellipsis);
}

bool IsCp932EntryStart(unsigned char a, unsigned char b) {
    const Cp932Punct& p = Punct();
    if (MatchesCp932Pair(a, b, p.open_quote) ||
        MatchesCp932Pair(a, b, p.ideographic_space)) {
        return true;
    }
    return a == '\n';
}

void UnescapeInPlace(std::string& s) {
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

std::string SanitizeEn(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
    }
    return s;
}

bool IsPrefix(const std::string& needle, const std::string& haystack) {
    return needle.size() <= haystack.size() &&
           std::memcmp(needle.data(), haystack.data(), needle.size()) == 0;
}

int ParseTsvBuffer(const char* data, size_t len, TranslationMap& out) {
    int count = 0;
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            i++;
        }
        size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) {
            i++;
        }

        size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') {
            tab++;
        }
        if (tab == line_start || tab >= line_end) {
            continue;
        }

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + 1, line_end - (tab + 1));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) {
            continue;
        }
        if (en == "(garbled data)") {
            continue;
        }
        out[jp] = en;
        count++;
    }
    return count;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    std::wstring wpath;
    if (!Utf8ToWide(path, wpath)) {
        return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(
        _wfopen(wpath.c_str(), L"rb"), &std::fclose);
    if (!file) {
        return false;
    }
    if (std::fseek(file.get(), 0, SEEK_END) != 0) {
        return false;
    }
    const long sz = std::ftell(file.get());
    if (sz <= 0) {
        return false;
    }
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(sz));
    const size_t n = std::fread(buf.data(), 1, buf.size(), file.get());
    if (n == 0) {
        return false;
    }
    const int added = ParseTsvBuffer(buf.data(), n, out);
    return added > 0;
}

int SplitSpeakerQuote(TranslationMap& map) {
    const Cp932Punct& punct = Punct();
    const std::string& kaku_open = punct.open_quote_str;
    const std::string& kaku_close = punct.close_quote_str;

    std::vector<std::pair<std::string, std::string>> adds;
    adds.reserve(map.size() / kSpeakerQuoteReserveDivisor);

    for (const auto& kv : map) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (jp.size() < kMinSpeakerQuoteBytes || en.size() < kMinSpeakerQuoteBytes) {
            continue;
        }

        const size_t jpOpen = jp.find(kaku_open);
        if (jpOpen == std::string::npos || jpOpen == 0) {
            continue;
        }
        if (jp.compare(jp.size() - kCp932CharBytes, kCp932CharBytes, kaku_close) != 0) {
            continue;
        }

        const size_t enOpen = en.find(kaku_open);
        if (enOpen == std::string::npos || enOpen == 0) {
            continue;
        }
        if (en.compare(en.size() - kCp932CharBytes, kCp932CharBytes, kaku_close) != 0) {
            continue;
        }

        adds.emplace_back(jp.substr(0, jpOpen), en.substr(0, enOpen));
        adds.emplace_back(jp.substr(jpOpen), en.substr(enOpen));
    }

    int added = 0;
    for (auto& p : adds) {
        if (p.first.empty() || p.second.empty()) {
            continue;
        }
        if (map.emplace(std::move(p.first), std::move(p.second)).second) {
            added++;
        }
    }
    return added;
}

std::vector<Segment> SegmentMessage(const std::string& msg,
                                    const TranslationMap& map) {
    std::vector<Segment> out;
    size_t pos = 0;
    while (pos + kCp932CharBytes <= msg.size()) {
        size_t hitLen = 0;
        const std::string* hitEn = nullptr;

        // Longest match first, in 2-byte steps.
        for (size_t len = msg.size() - pos; len >= kCp932CharBytes; len -= kCp932CharBytes) {
            auto it = map.find(msg.substr(pos, len));
            if (it == map.end()) {
                continue;
            }

            // Boundary gate: match must align with a YSTB entry boundary.
            const bool consumesRest = (pos + len == msg.size());
            const bool endsAtTerminator =
                IsCp932Terminator(static_cast<unsigned char>(msg[pos + len - kCp932CharBytes]),
                                  static_cast<unsigned char>(msg[pos + len - 1]));
            const bool followedByEntryStart =
                (pos + len + kCp932CharBytes <= msg.size()) &&
                IsCp932EntryStart(static_cast<unsigned char>(msg[pos + len]),
                                  static_cast<unsigned char>(msg[pos + len + 1]));
            const bool followedByTerminator =
                (pos + len + kCp932CharBytes <= msg.size()) &&
                IsCp932Terminator(static_cast<unsigned char>(msg[pos + len]),
                                  static_cast<unsigned char>(msg[pos + len + 1]));
            if (!consumesRest && !endsAtTerminator &&
                !followedByEntryStart && !followedByTerminator) {
                continue;
            }

            hitLen = len;
            hitEn = &it->second;
            break;
        }

        if (hitLen == 0) {
            pos += kCp932CharBytes;
            continue;
        }
        out.push_back({msg.substr(pos, hitLen), *hitEn});
        pos += hitLen;
    }
    return out;
}

std::string ReconstructJpFromBuffers(const char* buf38, size_t buf38_cap,
                                     const char* buf3c, size_t buf3c_cap,
                                     int opcount) {
    std::string jp;
    if (!buf38 || !buf3c || opcount <= 0) {
        return jp;
    }
    if (static_cast<size_t>(opcount) > buf3c_cap) {
        return jp;
    }
    jp.reserve(static_cast<size_t>(opcount) * kCp932CharBytes);
    size_t byte_pos = 0;
    for (int i = 0; i < opcount; i++) {
        const unsigned char op = static_cast<unsigned char>(buf3c[i]);
        if (op == kBuf3cSingleByteTextOpcode) {
            if (byte_pos >= buf38_cap) {
                break;
            }
            jp.push_back(buf38[byte_pos++]);
            continue;
        }
        if (op == kBuf3cDoubleByteTextOpcode) {
            if (byte_pos + 1 >= buf38_cap) {
                break;
            }
            jp.push_back(buf38[byte_pos]);
            jp.push_back(buf38[byte_pos + 1]);
            byte_pos += kCp932CharBytes;
        }
    }
    return jp;
}

int CountCp932Glyphs(const std::string& s) {
    // Only 2-byte glyphs because ASCII bytes do not drive the 2-byte hook path.
    int n = 0;
    for (size_t i = 0; i + 1 < s.size();) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        if (IsCp932LeadByte(a)) {
            n++;
            i += kCp932CharBytes;
        } else {
            i += 1;
        }
    }
    return n;
}

std::string FlattenEnToAscii(const std::string& en) {
    const Cp932Punct& p = Punct();
    std::string out;
    out.reserve(en.size());
    for (size_t i = 0; i < en.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(en[i]);
        if (c >= kAsciiPrintableMin && c <= kAsciiPrintableMax) {
            out += static_cast<char>(c);
            continue;
        }
        if (c == p.open_quote.lead && i + 1 < en.size()) {
            const unsigned char c2 = static_cast<unsigned char>(en[i + 1]);
            if (MatchesCp932Pair(c, c2, p.open_quote) ||
                MatchesCp932Pair(c, c2, p.close_quote)) {
                out += '"';
            } else if (MatchesCp932Pair(c, c2, p.period)) {
                out += '.';
            } else if (MatchesCp932Pair(c, c2, p.ideographic_space)) {
                out += ' ';
            } else {
                out += ' ';
            }
            i++;
        }
    }
    return out;
}

void SpreadEnAcrossSlots(const std::string& en, int slots, int chars_per_slot,
                         std::vector<std::string>& out) {
    if (slots <= 0 || chars_per_slot <= 0) {
        return;
    }
    const int budget = slots * chars_per_slot;
    const int take = static_cast<int>(en.size()) < budget ? static_cast<int>(en.size()) : budget;
    int produced = 0;
    for (int i = 0; i + chars_per_slot <= take; i += chars_per_slot) {
        out.emplace_back(en.substr(static_cast<size_t>(i), static_cast<size_t>(chars_per_slot)));
        produced++;
    }
    const int leftover = take - produced * chars_per_slot;
    if (leftover > 0) {
        std::string last = en.substr(static_cast<size_t>(produced * chars_per_slot),
                                     static_cast<size_t>(leftover));
        last.append(static_cast<size_t>(chars_per_slot - leftover), ' ');
        out.push_back(std::move(last));
        produced++;
    }
    const std::string pad(static_cast<size_t>(chars_per_slot), ' ');
    for (int i = produced; i < slots; i++) {
        out.push_back(pad);
    }
}

void ResplitSpeakerQuote(std::vector<Segment>& segs,
                         const TranslationMap& map) {
    const std::string& kaku_open = Punct().open_quote_str;
    std::vector<Segment> out;
    out.reserve(segs.size() * kResplitReserveMultiplier);
    for (auto& seg : segs) {
        const size_t open = seg.jp.find(kaku_open);
        if (open == std::string::npos || open == 0) {
            out.push_back(std::move(seg));
            continue;
        }
        std::string speaker_jp = seg.jp.substr(0, open);
        std::string quote_jp = seg.jp.substr(open);
        auto sit = map.find(speaker_jp);
        auto qit = map.find(quote_jp);

        if (sit != map.end() && qit != map.end()) {
            out.push_back({speaker_jp, sit->second});
            out.push_back({quote_jp, qit->second});
            continue;
        }

        if (sit != map.end()) {
            const std::string& speaker_en = sit->second;
            const size_t en_pos = seg.en.find(speaker_en);
            if (en_pos != std::string::npos) {
                std::string quote_en = seg.en.substr(en_pos + speaker_en.size());
                if (!quote_en.empty()) {
                    out.push_back({speaker_jp, speaker_en});
                    out.push_back({quote_jp, std::move(quote_en)});
                    continue;
                }
            }
        }

        size_t en_split = seg.en.find(kaku_open);
        if (en_split == std::string::npos) {
            en_split = seg.en.find('"');
        }
        if (en_split != std::string::npos && en_split > 0) {
            out.push_back({speaker_jp, seg.en.substr(0, en_split)});
            out.push_back({quote_jp, seg.en.substr(en_split)});
            continue;
        }

        out.push_back(std::move(seg));
    }
    segs = std::move(out);
}

std::vector<PlacedSeg>
ComputePackPlan(const std::string& jp, const TranslationMap& map,
                int chars_per_slot) {
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);
    Layout L = LayOutSegments(jp, segs);
    return ComputePackPlanInternal(segs, L, map, chars_per_slot);
}

void BuildRenderPlan(const std::string& jp, const TranslationMap& map,
                     int chars_per_slot,
                     std::vector<std::string>& chunks) {
    chunks.clear();
    if (chars_per_slot <= 0) {
        return;
    }
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);

    Layout L = LayOutSegments(jp, segs);

    const std::string blank(static_cast<size_t>(chars_per_slot), ' ');
    chunks.assign(L.regions.size(), blank);

    auto plan = ComputePackPlanInternal(segs, L, map, chars_per_slot);
    if (PlanFits(plan, L.regions.size()) && !plan.empty()) {
        for (const auto& p : plan) {
            std::vector<std::string> spread;
            SpreadEnAcrossSlots(p.en, static_cast<int>(p.en_slots), chars_per_slot, spread);
            for (size_t i = 0; i < spread.size() && p.place_at + i < chunks.size(); i++) {
                chunks[p.place_at + i] = std::move(spread[i]);
            }
        }
        return;
    }

    // Fallback: per-segment layout at natural JP positions.
    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) {
            continue;
        }
        const size_t start = L.seg_start[s];
        const size_t ext_slots = ExtendedSlotsForSeg(L, s);
        if (L.regions[start].is_hidden) {
            continue;
        }
        std::string en = FlattenEnToAscii(segs[s].en);
        std::vector<std::string> spread;
        SpreadEnAcrossSlots(en, static_cast<int>(ext_slots), chars_per_slot, spread);
        for (size_t i = 0; i < spread.size() && i < ext_slots; i++) {
            chunks[start + i] = std::move(spread[i]);
        }
    }
}

std::vector<OverflowedSegment>
DetectOverflows(const std::string& jp, const TranslationMap& map,
                int chars_per_slot) {
    std::vector<OverflowedSegment> out;
    if (chars_per_slot <= 0) {
        return out;
    }
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);

    Layout L = LayOutSegments(jp, segs);

    auto plan = ComputePackPlanInternal(segs, L, map, chars_per_slot);
    if (PlanFits(plan, L.regions.size())) {
        return out;
    }

    for (size_t s = 0; s < segs.size(); s++) {
        if (L.seg_start[s] == SIZE_MAX) {
            continue;
        }
        if (L.regions[L.seg_start[s]].is_hidden) {
            continue;
        }
        const size_t ext_slots = ExtendedSlotsForSeg(L, s);
        std::string en = FlattenEnToAscii(segs[s].en);
        const int budget = static_cast<int>(ext_slots) * chars_per_slot;
        if (static_cast<int>(en.size()) > budget) {
            out.push_back({segs[s].jp, std::move(en),
                           static_cast<int>(ext_slots),
                           static_cast<int>(segs[s].en.size())});
            out.back().chars_needed = static_cast<int>(out.back().seg_en.size());
        }
    }
    return out;
}

int ComputeAutoFitCharsPerSlot(const std::string& jp,
                               const TranslationMap& map,
                               int min_cps, int max_cps) {
    if (max_cps < min_cps) {
        return min_cps;
    }
    for (int cps = min_cps; cps <= max_cps; cps++) {
        if (DetectOverflows(jp, map, cps).empty()) {
            return cps;
        }
    }
    return max_cps;
}

EnglishLine ReconstructEnglish(const std::string& jp, const TranslationMap& map) {
    const Cp932Punct& punct = Punct();
    EnglishLine out;
    auto segs = SegmentMessage(jp, map);
    ResplitSpeakerQuote(segs, map);
    if (segs.empty()) {
        return out;
    }

    size_t body_start = 0;
    if (segs.size() >= kMinSpeakerPlusBodySegments &&
        map.find(segs[0].jp) != map.end() &&
        StartsWithCp932Pair(segs[1].jp, punct.open_quote)) {
        out.speaker       = CollapseSpaces(FlattenEnToAscii(segs[0].en));
        out.speaker_slots = CountCp932Glyphs(segs[0].jp);
        body_start        = 1;
    }

    std::string body;
    for (size_t i = body_start; i < segs.size(); i++) {
        std::string e = FlattenEnToAscii(segs[i].en);
        if (e.empty()) {
            continue;
        }
        if (!body.empty()) {
            body += ' ';
        }
        body += e;
    }
    out.body  = CollapseSpaces(body);
    out.valid = !out.body.empty();
    return out;
}

}  // namespace translator_logic
