// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace translator_logic {

namespace {

constexpr unsigned char kMinCp932Lead1 = 0x81;
constexpr unsigned char kMaxCp932Lead1 = 0x9F;
constexpr unsigned char kMinCp932Lead2 = 0xE0;
constexpr unsigned char kMaxCp932Lead2 = 0xFC;
constexpr unsigned char kFwSpaceLeadByte = 0x81;
constexpr unsigned char kFwSpaceTrailByte = 0x40;
constexpr std::size_t kCp932PairBytes = 2;

constexpr unsigned char kMinControlPrefix = 0x02;
constexpr unsigned char kMaxControlPrefix = 0x08;
constexpr unsigned char kAsciiLf = 0x0A;
constexpr unsigned char kAsciiCr = 0x0D;

#ifdef _WIN32
constexpr unsigned kCodePageCp932 = 932;
constexpr std::size_t kCp932ScratchBufSize = 8;
#endif

constexpr unsigned char kUtf8AsciiLimit = 0x80;
constexpr unsigned char kUtf8TwoByteMask = 0xE0;
constexpr unsigned char kUtf8TwoByteLead = 0xC0;
constexpr unsigned char kUtf8TwoBytePayload = 0x1F;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0;
constexpr unsigned char kUtf8ThreeByteLead = 0xE0;
constexpr unsigned char kUtf8ThreeBytePayload = 0x0F;
constexpr unsigned char kUtf8FourByteMask = 0xF8;
constexpr unsigned char kUtf8FourByteLead = 0xF0;
constexpr unsigned char kUtf8FourBytePayload = 0x07;
constexpr unsigned char kUtf8ContMask = 0xC0;
constexpr unsigned char kUtf8ContTag = 0x80;
constexpr unsigned char kUtf8ContPayload = 0x3F;
constexpr unsigned kUtf8ContShiftBits = 6;
constexpr std::size_t kOneExtraByte = 1;
constexpr std::size_t kTwoExtraBytes = 2;
constexpr std::size_t kThreeExtraBytes = 3;

constexpr unsigned kBmpMax = 0xFFFFu;
constexpr unsigned kUnicodeMax = 0x10FFFFu;
constexpr unsigned kSupplementaryBase = 0x10000u;
constexpr unsigned kHighSurrogateBase = 0xD800u;
constexpr unsigned kLowSurrogateBase = 0xDC00u;
constexpr unsigned kSurrogatePayloadMask = 0x3FFu;
constexpr unsigned kSurrogateShiftBits = 10;

constexpr unsigned kLatin1FoldFirst = 0x00C0u;
constexpr unsigned kLatin1FoldLast = 0x00FFu;
constexpr unsigned kFullwidthAsciiFirst = 0xFF01u;
constexpr unsigned kFullwidthAsciiLast = 0xFF5Eu;
constexpr unsigned kFullwidthAsciiOffset = 0xFEE0u;

constexpr std::size_t kMinRubyCloserLen = 2;
constexpr std::size_t kMinLinesForUnmeasuredSurface = 2;
constexpr unsigned kByteShiftBits = 8;
constexpr std::size_t kMinSegmentsForConcatMatch = 2;
constexpr std::size_t kMinJoinRoomColumns = 2;

}  // namespace

// TSV parsing.

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case 't':  out += '\t'; ++i; continue;
                case '\\': out += '\\'; ++i; continue;
            }
        }
        out += c;
    }
    s.swap(out);
}

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out,
                   std::vector<UnmappableChar>* unmappable,
                   std::vector<std::string>* markup_rows) {
    int added = 0;
    std::size_t i = 0;
    while (i < len) {
        // Find end-of-line
        std::size_t eol = i;
        while (eol < len && data[eol] != '\n') ++eol;
        std::size_t line_end = eol;
        // Trim trailing \r
        if (line_end > i && data[line_end - 1] == '\r') --line_end;

        // Find tab separator
        std::size_t tab = i;
        while (tab < line_end && data[tab] != '\t') ++tab;

        if (tab < line_end) {
            std::string jp(data + i, tab - i);
            std::string en(data + tab + 1, line_end - tab - 1);
            UnescapeInPlace(jp);
            UnescapeInPlace(en);
            if (!jp.empty() && !en.empty()) {
                std::vector<unsigned> lost;
                std::string key = Utf8ToCp932(jp);
                std::string value;
                if (en == jp) {
                    // Identity row: pass the script's own text through unchanged.
                    value = Utf8ToCp932(en);
                } else {
                    // Strip engine markup copied from the Japanese.
                    if (CarriesEngineMarkup(en)) {
                        if (markup_rows) markup_rows->push_back(jp);
                        en = StripEngineMarkup(en);
                    }
                    value = EnglishToCp932(en, &lost);
                }
                if (unmappable) {
                    for (unsigned cp : lost) {
                        UnmappableChar u;
                        u.jp_key = jp;
                        u.codepoint = cp;
                        unmappable->push_back(u);
                    }
                }
                // Strip trailing breaks -- the wire's own 0x0A chains the box.
                while (!value.empty() &&
                       (value.back() == '\n' || value.back() == '\r'))
                    value.pop_back();
                if (!value.empty()) {
                    out[key] = value;
                    ++added;
                }
            }
        }

        i = eol + 1;
    }
    return added;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out,
                 std::vector<UnmappableChar>* unmappable,
                 std::vector<std::string>* markup_rows) {
    if (path.empty()) {
        return false;
    }
    try {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        if (!f) {
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string buf = ss.str();
        const int n = ParseTsvBuffer(
            buf.data(), buf.size(), out, unmappable, markup_rows);
        return n > 0;
    } catch (...) {
        return false;
    }
}

// CP932 helpers.

bool IsCp932LeadByte(unsigned char b) {
    return (b >= kMinCp932Lead1 && b <= kMaxCp932Lead1) ||
           (b >= kMinCp932Lead2 && b <= kMaxCp932Lead2);
}

bool HasJPLeadByte(const char* data, std::size_t len) {
    for (std::size_t i = 0; i + 1 < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(data[i]);
        if (IsCp932LeadByte(b)) return true;
    }
    return false;
}

std::string Utf8ToCp932(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int wn = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], wn);
    const int bn = WideCharToMultiByte(
        kCodePageCp932, 0, w.data(), wn, nullptr, 0, "?", nullptr);
    if (bn <= 0) return utf8;
    std::string out(static_cast<std::size_t>(bn), '\0');
    WideCharToMultiByte(
        kCodePageCp932, 0, w.data(), wn, &out[0], bn, "?", nullptr);
    return out;
#else
    return utf8;
#endif
}

std::string Cp932ToUtf8(const std::string& cp932) {
#ifdef _WIN32
    if (cp932.empty()) return {};
    const int wn = MultiByteToWideChar(
        kCodePageCp932, 0, cp932.data(), static_cast<int>(cp932.size()),
        nullptr, 0);
    if (wn <= 0) return cp932;
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    MultiByteToWideChar(
        kCodePageCp932, 0, cp932.data(), static_cast<int>(cp932.size()),
        &w[0], wn);
    const int bn = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (bn <= 0) return cp932;
    std::string out(static_cast<std::size_t>(bn), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.data(), wn, &out[0], bn, nullptr, nullptr);
    return out;
#else
    return cp932;
#endif
}

// Ruby markup.

namespace {

// End of the tag at `open`, or npos.  '<' and '>' are never CP932 trail bytes.
std::size_t EngineTagEnd(const std::string& text, std::size_t open) {
    const std::size_t close = text.find('>', open + 1);
    // "<>" carries no name; the engine draws both characters.
    if (close == open + 1) return std::string::npos;
    return close;
}

// Tag name between the brackets, lowercased.
std::string TagName(const std::string& text, std::size_t open, std::size_t close) {
    std::string name = text.substr(open + 1, close - open - 1);
    for (char& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return name;
}

// True for furigana tags ("r..." or "/r"); the engine prefix-matches.
bool IsRubyTag(const std::string& name) {
    if (name.empty()) return false;
    if (name[0] == 'r') return true;
    return name.size() >= kMinRubyCloserLen && name[0] == '/' && name[1] == 'r';
}

}  // namespace

bool CarriesEngineMarkup(const std::string& text) {
    for (std::size_t i = text.find('<'); i != std::string::npos;
         i = text.find('<', i + 1))
        if (EngineTagEnd(text, i) != std::string::npos) return true;
    return false;
}

std::string StripEngineMarkup(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ) {
        if (text[i] != '<') { out += text[i]; ++i; continue; }
        // The engine consumes the tag regardless of name; keep the text around it.
        const std::size_t close = EngineTagEnd(text, i);
        if (close != std::string::npos) { i = close + 1; continue; }
        // No '>' -- the engine draws the bracket as-is.
        out += text[i];
        ++i;
    }
    return out;
}

bool CarriesRuby(const std::string& cp932) {
    for (std::size_t i = cp932.find('<'); i != std::string::npos;
         i = cp932.find('<', i + 1)) {
        const std::size_t close = EngineTagEnd(cp932, i);
        if (close != std::string::npos && IsRubyTag(TagName(cp932, i, close)))
            return true;
    }
    return false;
}

std::string StripRubyForDisplay(const std::string& cp932) {
    std::string out;
    out.reserve(cp932.size());
    for (std::size_t i = 0; i < cp932.size(); ) {
        if (cp932[i] != '<') { out += cp932[i]; ++i; continue; }
        const std::size_t close = EngineTagEnd(cp932, i);
        if (close != std::string::npos && IsRubyTag(TagName(cp932, i, close))) {
            i = close + 1;   // the reading goes, the base text after it stays
            continue;
        }
        // Non-ruby tags and unmatched '<' are kept.
        out += cp932[i];
        ++i;
    }
    return out;
}

// English value preparation.

namespace {

// Typographic characters folded to ASCII (CP932 fullwidth forms would draw
// two columns wide in halfwidth prose).
struct Fold {
    unsigned    cp;
    const char* ascii;
};

const Fold kTypographyFolds[] = {
    { 0x00A0, " "     },  // no-break space
    { 0x00A1, "!"     },  // inverted exclamation mark
    { 0x00A5, "Y"     },  // yen sign: CP932 draws its own 0x5C as a backslash
    { 0x00AB, "\""    },  // left  double guillemet
    { 0x00AD, ""      },  // soft hyphen: draws nothing
    { 0x00B0, "deg"   },  // degree sign
    { 0x00B1, "+/-"   },  // plus-minus sign
    { 0x00B5, "u"     },  // micro sign
    { 0x00B7, " "     },  // middle dot, used to separate name parts
    { 0x00BB, "\""    },  // right double guillemet
    { 0x00BC, "1/4"   },
    { 0x00BD, "1/2"   },
    { 0x00BE, "3/4"   },
    { 0x00BF, "?"     },  // inverted question mark
    { 0x00A9, "(c)"   },  // copyright sign
    { 0x00AE, "(R)"   },  // registered sign
    { 0x2010, "-"     },  // hyphen
    { 0x2011, "-"     },  // non-breaking hyphen
    { 0x2012, "-"     },  // figure dash
    { 0x2013, "-"     },  // en dash
    { 0x2014, "--"    },  // em dash
    { 0x2015, "--"    },  // horizontal bar
    { 0x2018, "'"     },  // left  single quote
    { 0x2019, "'"     },  // right single quote, and the curly apostrophe
    { 0x201A, "'"     },  // single low quote
    { 0x201B, "'"     },  // single high reversed quote
    { 0x201C, "\""    },  // left  double quote
    { 0x201D, "\""    },  // right double quote
    { 0x201E, "\""    },  // double low quote
    { 0x2020, "+"     },  // dagger
    { 0x2021, "++"    },  // double dagger
    { 0x2025, ".."    },  // two dot leader
    { 0x2026, "..."   },  // horizontal ellipsis
    { 0x2030, "%%"    },  // per mille sign
    { 0x2032, "'"     },  // prime
    { 0x2033, "\""    },  // double prime
    { 0x2039, "'"     },  // left  single guillemet
    { 0x203A, "'"     },  // right single guillemet
    { 0x203B, "*"     },  // reference mark
    { 0x20AC, "EUR"   },  // euro sign
    { 0x2116, "No."   },  // numero sign
    { 0x2122, "(TM)"  },  // trade mark sign
    { 0x2190, "--"    },  // leftwards arrow: a head plus a later '>' is a tag
    { 0x2191, "^"     },  // upwards arrow
    { 0x2192, "->"    },  // rightwards arrow
    { 0x2193, "v"     },  // downwards arrow
    { 0x2212, "-"     },  // minus sign
    { 0x2500, "-"     },  // box drawings light horizontal
    { 0x2501, "-"     },  // box drawings heavy horizontal
    { 0x2605, "*"     },  // black star
    { 0x2606, "*"     },  // white star
    { 0x22EF, "..."   },  // midline horizontal ellipsis
    { 0x2E3A, "--"    },  // two-em dash
    { 0x2E3B, "---"   },  // three-em dash
    { 0x3000, ""      },  // ideographic space: the source's paragraph indent
    { 0x3001, ","     },  // ideographic comma
    { 0x3002, "."     },  // ideographic full stop
    { 0x3008, "\""    },  // left  angle bracket: never '<'
    { 0x3009, "\""    },  // right angle bracket
    { 0x300A, "\""    },  // left  double angle bracket
    { 0x300B, "\""    },  // right double angle bracket
    { 0x300C, "\""    },  // left  corner bracket
    { 0x300D, "\""    },  // right corner bracket
    { 0x300E, "\""    },  // left  white corner bracket
    { 0x300F, "\""    },  // right white corner bracket
    { 0x3010, "["     },  // left  black lenticular bracket
    { 0x3011, "]"     },  // right black lenticular bracket
    { 0x301C, "~"     },  // wave dash
    { 0x30FB, " "     },  // katakana middle dot, used between name parts
    { 0xFFE5, "Y"     },  // fullwidth yen sign
};

// Zero-width characters dropped so they do not consume byte budget.
const Fold kZeroWidthFolds[] = {
    { 0x200B, "" },  // zero width space
    { 0x200C, "" },  // zero width non-joiner
    { 0x200D, "" },  // zero width joiner
    { 0x2060, "" },  // word joiner
    { 0xFE0F, "" },  // variation selector-16
    { 0xFEFF, "" },  // byte order mark / zero width no-break space
};

// U+00C0..U+00FF folded to the unaccented ASCII base, in code point order.
const char* const kLatin1Folds[] = {
    "A", "A", "A", "A", "A", "A", "AE", "C",  "E", "E", "E", "E",  "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O",  "x",  "O", "U", "U", "U",  "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c",  "e", "e", "e", "e",  "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o",  "/",  "o", "u", "u", "u",  "u", "y", "th", "y",
};

// Latin Extended-A folds (macron vowels from romanized Japanese, ligatures).
const Fold kLatinExtendedFolds[] = {
    { 0x0100, "A"  },  // A with macron
    { 0x0101, "a"  },
    { 0x0112, "E"  },  // E with macron
    { 0x0113, "e"  },
    { 0x012A, "I"  },  // I with macron
    { 0x012B, "i"  },
    { 0x014C, "O"  },  // O with macron
    { 0x014D, "o"  },
    { 0x016A, "U"  },  // U with macron
    { 0x016B, "u"  },
    { 0x0152, "OE" },  // upper ligature oe
    { 0x0153, "oe" },  // lower ligature oe
    { 0x0160, "S"  },
    { 0x0161, "s"  },
    { 0x0178, "Y"  },
    { 0x017D, "Z"  },
    { 0x017E, "z"  },
};

// Greek letters spelled out (CP932 fullwidth forms would draw two columns).
const Fold kGreekFolds[] = {
    { 0x0391, "Alpha"   }, { 0x03B1, "alpha"   },
    { 0x0392, "Beta"    }, { 0x03B2, "beta"    },
    { 0x0393, "Gamma"   }, { 0x03B3, "gamma"   },
    { 0x0394, "Delta"   }, { 0x03B4, "delta"   },
    { 0x0395, "Epsilon" }, { 0x03B5, "epsilon" },
    { 0x0396, "Zeta"    }, { 0x03B6, "zeta"    },
    { 0x0397, "Eta"     }, { 0x03B7, "eta"     },
    { 0x0398, "Theta"   }, { 0x03B8, "theta"   },
    { 0x0399, "Iota"    }, { 0x03B9, "iota"    },
    { 0x039A, "Kappa"   }, { 0x03BA, "kappa"   },
    { 0x039B, "Lambda"  }, { 0x03BB, "lambda"  },
    { 0x039C, "Mu"      }, { 0x03BC, "mu"      },
    { 0x039D, "Nu"      }, { 0x03BD, "nu"      },
    { 0x039E, "Xi"      }, { 0x03BE, "xi"      },
    { 0x039F, "Omicron" }, { 0x03BF, "omicron" },
    { 0x03A0, "Pi"      }, { 0x03C0, "pi"      },
    { 0x03A1, "Rho"     }, { 0x03C1, "rho"     },
    { 0x03A3, "Sigma"   }, { 0x03C3, "sigma"   },
    { 0x03C2, "sigma"   },  // final sigma
    { 0x03A4, "Tau"     }, { 0x03C4, "tau"     },
    { 0x03A5, "Upsilon" }, { 0x03C5, "upsilon" },
    { 0x03A6, "Phi"     }, { 0x03C6, "phi"     },
    { 0x03A7, "Chi"     }, { 0x03C7, "chi"     },
    { 0x03A8, "Psi"     }, { 0x03C8, "psi"     },
    { 0x03A9, "Omega"   }, { 0x03C9, "omega"   },
};

// Decode one UTF-8 code point at `i`, advancing past it.  Malformed bytes
// set `malformed` so the caller reports rather than folds them.
unsigned NextCodepoint(const std::string& s, std::size_t& i, bool& malformed) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    unsigned cp = 0;
    malformed = false;
    if (b < kUtf8AsciiLimit) {
        ++i;
        return b;
    }
    if ((b & kUtf8TwoByteMask) == kUtf8TwoByteLead) {
        extra = kOneExtraByte;
        cp = b & kUtf8TwoBytePayload;
    } else if ((b & kUtf8ThreeByteMask) == kUtf8ThreeByteLead) {
        extra = kTwoExtraBytes;
        cp = b & kUtf8ThreeBytePayload;
    } else if ((b & kUtf8FourByteMask) == kUtf8FourByteLead) {
        extra = kThreeExtraBytes;
        cp = b & kUtf8FourBytePayload;
    } else {
        ++i;
        malformed = true;
        return b;
    }

    if (i + extra >= s.size()) {
        ++i;
        malformed = true;
        return b;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & kUtf8ContMask) != kUtf8ContTag) {
            ++i;
            malformed = true;
            return b;
        }
        cp = (cp << kUtf8ContShiftBits) | (c & kUtf8ContPayload);
    }
    i += extra + 1;
    return cp;
}

// The CP932 bytes for one code point, or "" when it has none.
std::string Cp932FromCodepoint(unsigned cp) {
#ifdef _WIN32
    wchar_t w[kCp932PairBytes];
    int wn = 0;
    if (cp <= kBmpMax) {
        w[wn++] = static_cast<wchar_t>(cp);
    } else if (cp <= kUnicodeMax) {
        const unsigned v = cp - kSupplementaryBase;
        w[wn++] = static_cast<wchar_t>(
            kHighSurrogateBase + (v >> kSurrogateShiftBits));
        w[wn++] = static_cast<wchar_t>(
            kLowSurrogateBase + (v & kSurrogatePayloadMask));
    } else {
        return {};
    }
    char buf[kCp932ScratchBufSize];
    BOOL used_default = FALSE;
    const int bn = WideCharToMultiByte(
        kCodePageCp932, 0, w, wn, buf, sizeof(buf), "?", &used_default);
    if (bn <= 0 || used_default) return {};
    return std::string(buf, static_cast<std::size_t>(bn));
#else
    std::string out;
    if (cp < kUtf8AsciiLimit) {
        out += static_cast<char>(cp);
        return out;
    }
    return out;
#endif
}

// The fold for `cp`, or nullptr when there is none.
const char* FoldFor(unsigned cp) {
    if (cp >= kLatin1FoldFirst && cp <= kLatin1FoldLast) {
        return kLatin1Folds[cp - kLatin1FoldFirst];
    }
    for (const Fold& f : kTypographyFolds) {
        if (f.cp == cp) return f.ascii;
    }
    for (const Fold& f : kZeroWidthFolds) {
        if (f.cp == cp) return f.ascii;
    }
    for (const Fold& f : kLatinExtendedFolds) {
        if (f.cp == cp) return f.ascii;
    }
    for (const Fold& f : kGreekFolds) {
        if (f.cp == cp) return f.ascii;
    }
    return nullptr;
}

}  // namespace

std::string EnglishToCp932(const std::string& utf8,
                           std::vector<unsigned>* unmappable) {
    std::string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        bool malformed = false;
        const unsigned cp = NextCodepoint(utf8, i, malformed);
        if (malformed) {
            if (unmappable) unmappable->push_back(cp);
            out += '?';
            continue;
        }
        if (cp < kUtf8AsciiLimit) {
            out += static_cast<char>(cp);
            continue;
        }

        if (const char* fold = FoldFor(cp)) {
            out += fold;
            continue;
        }
        // Fullwidth ASCII sits exactly one block above its halfwidth twin.
        if (cp >= kFullwidthAsciiFirst && cp <= kFullwidthAsciiLast) {
            out += static_cast<char>(cp - kFullwidthAsciiOffset);
            continue;
        }

        const std::string bytes = Cp932FromCodepoint(cp);
        if (bytes.empty()) {
            if (unmappable) unmappable->push_back(cp);
            out += '?';
        } else {
            out += bytes;
        }
    }
    // Turn surviving '<...>' pairs into '(' so the engine does not consume them.
    for (std::size_t at = out.find('<'); at != std::string::npos;
         at = out.find('<', at + 1)) {
        if (EngineTagEnd(out, at) != std::string::npos) out[at] = '(';
    }
    return out;
}

bool HasWideCharacter(const std::string& cp932) {
    for (std::size_t i = 0; i < cp932.size(); ++i) {
        if (IsCp932LeadByte(static_cast<unsigned char>(cp932[i])) &&
            i + 1 < cp932.size()) {
            return true;
        }
    }
    return false;
}

// Word wrap.

namespace {

const char kTruncationMarker[] = "...";
const std::size_t kMarkerLen = sizeof(kTruncationMarker) - 1;

// First byte of the CP932 character covering `at`, scanning forward from `from`.
std::size_t Cp932CharStart(const std::string& s, std::size_t from, std::size_t at) {
    std::size_t i = from;
    while (i < at) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        std::size_t width = (IsCp932LeadByte(b) && i + 1 < s.size()) ? 2 : 1;
        if (i + width > at) return i;  // `at` falls inside this character
        i += width;
    }
    return at;
}

// Cut the last line back to fit the "..." marker inside `width`.
void MarkTruncated(std::string& text, std::size_t line_start, std::size_t width) {
    // A continued line can leave less room than the marker needs; the marker
    // then overflows by a character or two rather than the cut underflowing.
    if (width < kMarkerLen) width = kMarkerLen;
    if (text.size() - line_start + kMarkerLen > width) {
        text.resize(Cp932CharStart(text, line_start,
                                   line_start + width - kMarkerLen));
        std::size_t space = text.find_last_of(' ');
        if (space != std::string::npos && space >= line_start)
            text.resize(space);
    }
    while (text.size() > line_start && text.back() == ' ') text.pop_back();
    text += kTruncationMarker;
}

// Trim `text` to `max_bytes`, dropping whole lines first, then marking.
// Returns true when anything was dropped.
bool TrimToBytes(std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return false;
    for (;;) {
        const std::size_t nl = text.find_last_of('\n');
        const std::size_t line_start = (nl == std::string::npos) ? 0 : nl + 1;
        // The continuation indent is not content and is not cut into.
        std::size_t content = line_start;
        while (content < text.size() && content < line_start + WRAP_INDENT &&
               text[content] == ' ')
            ++content;

        if (max_bytes >= content + kMarkerLen) {
            text.resize(Cp932CharStart(text, content, max_bytes - kMarkerLen));
            const std::size_t space = text.find_last_of(' ');
            if (space != std::string::npos && space >= content)
                text.resize(space);
            while (text.size() > content && text.back() == ' ') text.pop_back();
            text += kTruncationMarker;
            return true;
        }
        if (nl == std::string::npos) {
            // Not even a marked first line fits.  Nothing readable can be
            // drawn, so draw the marker alone rather than half a character.
            text.assign(kTruncationMarker,
                        max_bytes < kMarkerLen ? max_bytes : kMarkerLen);
            return true;
        }
        text.resize(nl);
    }
}

// Physical lines a wrapped buffer draws, counted after something has cut it.
std::size_t LinesIn(const std::string& text) {
    std::size_t lines = 1;
    for (char c : text)
        if (c == '\n') ++lines;
    return lines;
}

}  // namespace

bool FitsUnmeasuredSurface(const std::string& cp932, std::size_t pitch) {
    // Need a spare line in case the engine re-breaks.
    if (LinesAtPitch(pitch) < kMinLinesForUnmeasuredSurface) return false;
    std::size_t begin = 0;
    if (!cp932.empty()) {
        const unsigned char b = static_cast<unsigned char>(cp932[0]);
        if (b >= kMinControlPrefix && b <= kMaxControlPrefix) begin = 1;
    }
    std::size_t end = cp932.size();
    while (end > begin && (cp932[end - 1] == '\n' || cp932[end - 1] == '\r')) {
        --end;
    }
    if (end <= begin) return false;
    // A break of our own would draw a line the surface may not have.
    if (cp932.find_first_of("\r\n", begin) < end) return false;
    // One CP932 byte = one column.
    return end - begin <= ColumnsAtPitch(pitch);
}

void MarkLastLineTruncated(std::string& text, std::size_t reserved_columns,
                           std::size_t start_columns) {
    std::size_t nl = text.find_last_of('\n');
    if (nl == std::string::npos) {
        const std::size_t used = reserved_columns + start_columns;
        MarkTruncated(text, 0,
                      SINGLE_LINE_MAX > used ? SINGLE_LINE_MAX - used : 0);
        return;
    }
    // Skip past the continuation indent.
    std::size_t line_start = nl + 1;
    std::size_t limit = line_start + WRAP_INDENT;
    while (line_start < text.size() && line_start < limit &&
           text[line_start] == ' ')
        ++line_start;
    MarkTruncated(text, line_start, WRAP_AT - reserved_columns);
}

WrappedText WrapForMessageBox(const std::string& value, std::size_t max_lines,
                              std::size_t reserved_columns, std::size_t start_columns,
                              bool continue_line) {
    WrappedText out;
    const std::size_t wrap_at = WRAP_AT > reserved_columns
                           ? WRAP_AT - reserved_columns : 1;
    const std::size_t single_line_max = SINGLE_LINE_MAX > reserved_columns
                                   ? SINGLE_LINE_MAX - reserved_columns : 1;

    // Trailing breaks chain the box; they cost no line.
    std::size_t core_end = value.size();
    while (core_end > 0 &&
           (value[core_end - 1] == '\n' || value[core_end - 1] == '\r'))
        --core_end;
    const std::string tail = value.substr(core_end);
    const std::string core = value.substr(0, core_end);

    if (max_lines == 0) {
        out.text = tail;
        out.lines = 0;
        out.truncated = !core.empty();
        return out;
    }

    if (!continue_line && core.size() <= single_line_max &&
        core.find('\n') == std::string::npos) {
        out.text = value;
        out.end_columns = core.size();
        return out;
    }

    // Room on the n-th line.  When continuing, line one is the line the box
    // left open; the engine's indent never reaches it, the pen does.
    auto capacity = [&](std::size_t n) -> std::size_t {
        if (n > 1) return wrap_at;
        if (!continue_line) return single_line_max;
        return SINGLE_LINE_MAX > start_columns ? SINGLE_LINE_MAX - start_columns
                                               : 0;
    };

    out.lines = 0;
    std::size_t line_start = 0;  // where the last line's content begins in out.text
    std::size_t pos = 0;         // next byte of `core` still to be drawn
    bool   dropped = false;

    // Embedded 0x0A = break the translation asked for.
    while (!dropped) {
        std::size_t para_end = core.find('\n', pos);
        if (para_end == std::string::npos) para_end = core.size();

        for (;;) {
            if (out.lines == max_lines) { dropped = true; break; }
            if (out.lines > 0) {
                out.text += '\n';
                out.text.append(WRAP_INDENT, ' ');
            }
            line_start = out.text.size();
            ++out.lines;
            const std::size_t limit = capacity(out.lines);

            std::size_t remaining = para_end - pos;
            if (remaining <= limit) {
                out.text.append(core, pos, remaining);
                pos = para_end;
                break;
            }
            if (limit == 0) continue;  // the open line is already full
            // Walk back to the last space (0x20 is never a CP932 trail byte).
            std::size_t brk = pos + limit;
            while (brk > pos && core[brk] != ' ') --brk;
            if (brk == pos) {  // one word wider than the room this line has
                // The open line has a fresh one behind it; a word that would
                // have to split against the stub moves down whole instead.
                if (out.lines == 1 && continue_line) continue;
                brk = Cp932CharStart(core, pos, pos + limit);
                if (brk <= pos) brk = pos + limit;
            }
            out.text.append(core, pos, brk - pos);
            pos = (core[brk] == ' ') ? brk + 1 : brk;
        }

        if (dropped || para_end == core.size()) break;
        pos = para_end + 1;  // step over the break itself
    }

    if (pos < core.size()) {
        out.truncated = true;
        MarkTruncated(out.text, line_start,
                      out.lines > 1 ? wrap_at : capacity(1));
    }
    const std::size_t nl = out.text.find_last_of('\n');
    const std::size_t last_len =
        out.text.size() - (nl == std::string::npos ? 0 : nl + 1);
    out.end_columns = last_len +
        (nl != std::string::npos ? reserved_columns
                                 : (continue_line ? start_columns : 0));
    out.text += tail;
    return out;
}

// Shared box state across chained segments.

struct BoxLineBudget::Impl {
    struct Box {
        std::uintptr_t   id = 0;
        BoxState    state;
        BoxState    before_last;  // what the box held before its last segment
        std::string last_wire;    // that segment's full wire buffer
        bool        has_last = false;
        bool        open = false;
        bool        positioned = false;   // opened by iMessage* ("　", 0x81 0x40)
        bool        redraw_pass = false;  // msgwndctrl._bp redraw (no 0x05)
        int         last_raw_x = -1;
        int         last_raw_y = -1;
        int         before_last_raw_x = -1;
        int         before_last_raw_y = -1;
        bool        used = false;
    };

    // Fixed-size ring buffer; the engine only uses a few boxes at a time.
    static const std::size_t kSlots = 8;
    Box   slots[kSlots];
    std::size_t next = 0;
    bool  saw_save_load_preview = false;

    Box& Find(std::uintptr_t id) {
        for (Box& b : slots)
            if (b.used && b.id == id) return b;
        Box& b = slots[next];
        next = (next + 1) % kSlots;
        b = Box();
        b.id = id;
        b.used = true;
        return b;
    }

    void OpenPositioned(std::uintptr_t id, const char* wire) {
        Box& b = Find(id);
        b.state = BoxState();
        b.before_last = b.state;
        b.last_wire = wire ? wire : "";
        b.has_last = wire != nullptr;
        b.open = true;
        b.positioned = true;
        b.redraw_pass = false;
        b.last_raw_x = -1;
        b.last_raw_y = -1;
        b.before_last_raw_x = -1;
        b.before_last_raw_y = -1;
    }
};

BoxLineBudget::BoxLineBudget() : impl_(std::make_unique<Impl>()) {}
BoxLineBudget::~BoxLineBudget() = default;

BoxState BoxLineBudget::Begin(std::uintptr_t box, bool continues,
                              const char* wire) {
    Impl::Box& b = impl_->Find(box);
    // A re-render of the segment just laid out: replay its starting state,
    // so the layout comes out byte for byte the same.  Checked before the
    // open flag, because the box may have closed on that segment.
    if (continues && b.has_last && wire && b.last_wire == wire) {
        b.state = b.before_last;
        return b.state;
    }
    if (!continues || !b.open) {
        b.state = BoxState();
        b.positioned = false;
        b.redraw_pass = false;
    }
    b.before_last = b.state;
    b.last_wire = wire ? wire : "";
    b.has_last = wire != nullptr;
    return b.state;
}

void BoxLineBudget::Commit(std::uintptr_t box, const BoxState& state,
                           bool stays_open) {
    Impl::Box& b = impl_->Find(box);
    b.state = state;
    b.open = stays_open;
    if (!stays_open) {
        b.positioned = false;
        b.redraw_pass = false;
    }
}

BoxState BoxLineBudget::BeginPositionedBox(std::uintptr_t box, const char* wire,
                                           const int* cursor_xy,
                                           int left_x, int top_y) {
    Impl::Box& b = impl_->Find(box);
    const bool cursor_not_at_top_left =
        cursor_xy != nullptr &&
        (cursor_xy[0] != left_x || cursor_xy[1] != top_y);
    if (cursor_not_at_top_left && b.positioned && b.open &&
        b.state.lines > 0 && !impl_->saw_save_load_preview) {
        b.before_last = b.state;
        b.last_wire = wire ? wire : "";
        b.has_last = wire != nullptr;
        b.redraw_pass = false;
        return b.state;
    }
    impl_->saw_save_load_preview = false;
    impl_->OpenPositioned(box, wire);
    return b.state;
}

void BoxLineBudget::NoteNoClearNewline() {
    impl_->saw_save_load_preview = false;
}

void BoxLineBudget::NoteSaveLoadPreview() {
    impl_->saw_save_load_preview = true;
}

BoxState BoxLineBudget::BeginSegment(std::uintptr_t box, bool has_prefix,
                                     const char* wire, const int* cursor_xy,
                                     int left_x, int top_y, bool flows,
                                     bool& out_is_positioned,
                                     bool& out_keep_positioned) {
    out_is_positioned = false;
    out_keep_positioned = false;

    const int raw_x = cursor_xy ? cursor_xy[0] : left_x;
    const int raw_y = cursor_xy ? cursor_xy[1] : top_y;

    if (!flows) return Begin(box, has_prefix, wire);

    Impl::Box& b = impl_->Find(box);
    if (b.positioned && b.open) {
        const bool is_rerender =
            b.has_last && wire && b.last_wire == wire &&
            (!cursor_xy || (raw_x == b.last_raw_x && raw_y == b.last_raw_y));
        if (is_rerender) {
            b.state = b.before_last;
            out_is_positioned = true;
            out_keep_positioned = true;
            return b.state;
        }
        bool continues_pos = false;
        if (b.state.lines == 0) {
            continues_pos = true;
            b.redraw_pass = !has_prefix;
        } else if (!b.redraw_pass) {
            continues_pos = has_prefix;
        } else {
            continues_pos =
                !has_prefix && cursor_xy != nullptr && b.last_raw_y >= 0 &&
                (raw_y > b.last_raw_y ||
                 (raw_y == b.last_raw_y && raw_x > b.last_raw_x));
        }
        if (continues_pos) {
            b.before_last = b.state;
            b.before_last_raw_x = b.last_raw_x;
            b.before_last_raw_y = b.last_raw_y;
            b.last_wire = wire ? wire : "";
            b.has_last = wire != nullptr;
            b.last_raw_x = raw_x;
            b.last_raw_y = raw_y;
            out_is_positioned = true;
            out_keep_positioned = true;
            return b.state;
        }
        b.positioned = false;
        b.redraw_pass = false;
        b.open = false;
        b.state = BoxState();
        b.last_raw_x = -1;
        b.last_raw_y = -1;
    }

    if (!has_prefix && cursor_xy != nullptr &&
        (raw_x != left_x || raw_y != top_y)) {
        // Standalone SET_CURSOR segment (e.g. MessageCenter without iMessage*).
        b.state = BoxState();
        b.before_last = b.state;
        b.before_last_raw_x = -1;
        b.before_last_raw_y = -1;
        b.last_wire = wire ? wire : "";
        b.has_last = wire != nullptr;
        b.last_raw_x = raw_x;
        b.last_raw_y = raw_y;
        out_is_positioned = true;
        out_keep_positioned = false;
        return b.state;
    }

    return Begin(box, has_prefix, wire);
}

void BoxLineBudget::CommitSegment(std::uintptr_t box, const BoxState& state,
                                  bool stays_open, bool keep_positioned) {
    if (keep_positioned) {
        Impl::Box& b = impl_->Find(box);
        b.state = state;
        b.open = true;
        b.positioned = true;
    } else {
        Commit(box, state, stays_open);
    }
}

void BoxLineBudget::Clear() {
    for (Impl::Box& b : impl_->slots) b = Impl::Box();
    impl_->next = 0;
    impl_->saw_save_load_preview = false;
}

// Substitution.

namespace {

// Look up `effective` in `translations` with fallbacks for yuzu_2g / backlog:
// 1. Exact match on (effective, len) or (effective, key_len).
// 2. Strip leading CP932 fullwidth space "　" (0x81 0x40) (logwnd._bp
//    concatenates the dummy "　" segment from iMessage* before the real text).
// 3. Greedy multi-segment prefix decomposition (logwnd._bp concatenates all
//    segments of a multi-call yuzu_2g box into a single string).
bool LookupTranslation(const char* effective, std::size_t len, std::size_t key_len,
                       const TranslationMap& translations,
                       std::string& out_value) {
    auto it = translations.find(std::string(effective, len));
    if (it == translations.end() && key_len != len) {
        it = translations.find(std::string(effective, key_len));
    }
    if (it != translations.end()) {
        out_value = it->second;
        return true;
    }

    auto starts_with_fw_space = [](const char* s, std::size_t n) {
        return n > kCp932PairBytes &&
               static_cast<unsigned char>(s[0]) == kFwSpaceLeadByte &&
               static_cast<unsigned char>(s[1]) == kFwSpaceTrailByte;
    };

    if (starts_with_fw_space(effective, key_len)) {
        it = translations.find(
            std::string(effective + kCp932PairBytes, len - kCp932PairBytes));
        if (it == translations.end() && key_len != len) {
            it = translations.find(std::string(
                effective + kCp932PairBytes, key_len - kCp932PairBytes));
        }
        if (it != translations.end()) {
            out_value = it->second;
            return true;
        }
    }

    // Multi-segment backlog concatenation fallback.
    const char* cur = effective;
    std::size_t rem = key_len;
    if (starts_with_fw_space(cur, rem)) {
        cur += kCp932PairBytes;
        rem -= kCp932PairBytes;
    }
    std::string joined;
    std::size_t segments_matched = 0;
    constexpr std::size_t kMaxConcatSegments = 8;
    while (rem > 0 && segments_matched < kMaxConcatSegments) {
        if (starts_with_fw_space(cur, rem)) {
            cur += kCp932PairBytes;
            rem -= kCp932PairBytes;
            if (rem == 0) break;
        }
        std::size_t best_len = 0;
        const std::string* best_val = nullptr;
        for (std::size_t pos = 0; pos < rem; ) {
            const unsigned char b = static_cast<unsigned char>(cur[pos]);
            const std::size_t w =
                (IsCp932LeadByte(b) && pos + 1 < rem) ? kCp932PairBytes : 1;
            pos += w;
            auto seg_it = translations.find(std::string(cur, pos));
            if (seg_it != translations.end()) {
                best_len = pos;
                best_val = &seg_it->second;
            }
        }
        if (!best_val || best_len == 0) {
            segments_matched = 0;
            break;
        }
        std::string part = *best_val;
        while (!part.empty() && (part.back() == '\n' || part.back() == '\r')) {
            part.pop_back();
        }
        if (!part.empty()) {
            if (!joined.empty() && joined.back() != ' ' && part.front() != ' ') {
                joined += ' ';
            }
            joined += part;
        }
        cur += best_len;
        rem -= best_len;
        ++segments_matched;
    }
    if (segments_matched >= kMinSegmentsForConcatMatch && rem == 0 &&
        !joined.empty()) {
        out_value = std::move(joined);
        return true;
    }
    return false;
}

// Engine line counting for untranslated buffers.

// Kinsoku shori: characters forbidden at line start (from the engine's table).
const std::uint16_t kNoLineStart[] = {
    0x0022, 0x0027, 0x003A, 0x003B, 0x003F, 0x0021,  // " ' : ; ? !
    0x00DE, 0x00DF, 0x00A5,                          // halfwidth ﾞ ﾟ ･
    0x8143, 0x8144, 0x8141, 0x8142,                  // ， ． 、 。
    0x8146, 0x8147, 0x8148, 0x8149,                  // ： ； ？ ！
    0x8168, 0x814A, 0x814B, 0x815D,                  // ” ゛ ゜ ‐
    0x005D, 0x007D, 0x0029,                          // ] } )
    0x816A, 0x816C, 0x816E, 0x8170, 0x8172, 0x81E2,  // ） 〕 ］ ｝ 〉 ≫
    0x8174, 0x8176, 0x8178, 0x817A,                  // 》 」 』 】
    0x8152, 0x8153, 0x8154, 0x8155, 0x8158,          // ヽ ヾ ゝ ゞ 々
    0x815B, 0x8160,                                  // ー ～
    0x82C1, 0x82E1, 0x82E3, 0x82E5,                  // っ ゃ ゅ ょ
    0x8362, 0x8383, 0x8385, 0x8387,                  // ッ ャ ュ ョ
};

// Characters forbidden at line end (openers that drag the next character along).
const std::uint16_t kNoLineEnd[] = {
    0x005B, 0x007B, 0x0028,                          // [ { (
    0x8169, 0x816B, 0x816D, 0x816F, 0x8171,          // （ 〔 ［ ｛ 〈
    0x81E1, 0x8173, 0x8175, 0x8177, 0x8179, 0x8167,  // ≪ 《 「 『 【 “
};

// CP932 character at `i` as a std::uint16_t (lead<<8|trail for two-byte), plus width.
std::uint16_t Cp932CharAt(const char* text, std::size_t len, std::size_t i, std::size_t& width) {
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (IsCp932LeadByte(b) && i + 1 < len) {
        width = kCp932PairBytes;
        return static_cast<std::uint16_t>(
            (static_cast<unsigned>(b) << kByteShiftBits) |
            static_cast<unsigned char>(text[i + 1]));
    }
    width = 1;
    return b;
}

bool InTable(const std::uint16_t* table, std::size_t n, std::uint16_t c) {
    for (std::size_t k = 0; k < n; ++k)
        if (table[k] == c) return true;
    return false;
}

// Extra columns the kinsoku rules add: the no-line-start run after this
// character, or the next character if this one is a no-line-end opener.
std::size_t KinsokuLookahead(const char* text, std::size_t len, std::size_t at, std::size_t width) {
    std::size_t next = at + width;
    std::size_t held = 0;
    while (next < len) {
        std::size_t w = 0;
        const std::uint16_t c = Cp932CharAt(text, len, next, w);
        if (!InTable(kNoLineStart, sizeof(kNoLineStart) / sizeof(*kNoLineStart), c))
            break;
        held += (w == 2) ? 2 : 1;
        next += w;
    }
    if (held > 0) return held;

    std::size_t own = 0;
    const std::uint16_t here = Cp932CharAt(text, len, at, own);
    if (!InTable(kNoLineEnd, sizeof(kNoLineEnd) / sizeof(*kNoLineEnd), here))
        return 0;
    if (at + width >= len) return 0;
    std::size_t after = 0;
    Cp932CharAt(text, len, at + width, after);
    return (after == 2) ? 2 : 1;
}

// Lines the engine draws for a buffer, reproducing its per-character word
// wrap with kinsoku lookahead.  Engine markup ("<...>") costs no column.
std::size_t EngineDrawnLines(const char* text, std::size_t len, std::size_t reserved_columns) {
    const std::size_t limit = BOX_COLUMNS > reserved_columns
                         ? BOX_COLUMNS - reserved_columns : 1;
    std::size_t lines = 1;
    std::size_t col = 0;   // columns already drawn on the line being drawn
    for (std::size_t i = 0; i < len; ) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        const bool wide = IsCp932LeadByte(b) && i + 1 < len;
        const std::size_t w = wide ? 2 : 1;

        if (!wide && b == '<') {
            const std::string_view rest(text + i, len - i);
            const std::size_t close = rest.find('>', 1);
            // "<>" and a bracket with no '>' after it are drawn as characters.
            if (close != std::string_view::npos && close > 1) {
                i += close + 1;
                continue;
            }
        }

        const std::size_t at = i;
        i += w;

        if (!wide && b == '\n') { ++lines; col = 0; continue; }
        if (!wide && b == '\r') continue;

        if (col + w + KinsokuLookahead(text, len, at, w) > limit) {
            ++lines;
            col = 0;
            if (b == ' ') continue;  // the break stands in for the space
        }
        col += w;
    }
    return lines;
}

SubstitutionStatus Build(const char* wire_text,
                         const TranslationMap& translations,
                         BoxLineBudget* budget, std::uintptr_t box,
                         const Surface& surface, Substitution& out,
                         int* cursor_xy = nullptr,
                         const int* box_rect = nullptr,
                         int line_pitch = DIALOGUE_LINE_PITCH_PX) {
    const std::size_t surface_lines = surface.lines;
    out = Substitution();
    if (!wire_text) return SubstitutionStatus::EmptyKey;

    const char* effective = wire_text;
    const unsigned char lead = static_cast<unsigned char>(*wire_text);
    const bool has_prefix =
        (lead >= kMinControlPrefix && lead <= kMaxControlPrefix);
    if (has_prefix) ++effective;

    const std::size_t len = std::strlen(effective);

    std::size_t key_len = len;
    while (key_len > 0) {
        const unsigned char b =
            static_cast<unsigned char>(effective[key_len - 1]);
        if (b != kAsciiLf && b != kAsciiCr) break;
        --key_len;
    }
    out.tail = len - key_len;
    out.continues = has_prefix;
    out.more_follows = out.tail > 0;

    if (key_len == 0 && out.tail == 0) {
        // No text and no chain -- leave the box state untouched.
        out.lines = 0;
        return SubstitutionStatus::EmptyKey;
    }

    const int left_x   = box_rect ? box_rect[0] : DIALOGUE_BOX_LEFT_X;
    const int top_y    = box_rect ? box_rect[1] : DIALOGUE_BOX_TOP_Y;
    const int pitch_px = line_pitch > 0 ? line_pitch : DIALOGUE_LINE_PITCH_PX;
    const int raw_x    = cursor_xy ? cursor_xy[0] : left_x;
    const int raw_y    = cursor_xy ? cursor_xy[1] : top_y;

    // yuzu_2g's iMessage* helpers open every positioned box with a dummy
    // op140(0, 0, 0, speaker, "　") (CP932 0x81 0x40) at (48, 24) before calling
    // SET_CURSOR (0x146) and op140(1, ...) for the actual segments.
    // After a *NC ("No Clear") wait, the next iMessage* helper emits "　"
    // without clearing the box, so cursor_xy arrives below top_y (e.g. (48, 54)
    // or (48, 84)) and continues the already-open positioned box.
    const bool is_dummy_fw_space =
        surface.flows && !has_prefix && out.tail == 0 &&
        key_len == kCp932PairBytes &&
        static_cast<unsigned char>(effective[0]) == kFwSpaceLeadByte &&
        static_cast<unsigned char>(effective[1]) == kFwSpaceTrailByte &&
        translations.find(std::string(effective, key_len)) == translations.end();
    if (is_dummy_fw_space) {
        BoxState pos_st;
        if (budget) {
            pos_st = budget->BeginPositionedBox(box, wire_text, cursor_xy,
                                                left_x, top_y);
        }
        if (cursor_xy) {
            cursor_xy[0] = left_x;
            cursor_xy[1] = top_y + static_cast<int>(pos_st.lines) * pitch_px;
        }
        out.bytes.clear();
        out.lines = 0;
        out.more_follows = true;
        out.box_lines_before = pos_st.lines;
        out.box_lines_after = pos_st.lines;
        return SubstitutionStatus::Substituted;
    }

    if (budget && !surface.flows && surface_lines == 1) {
        budget->NoteSaveLoadPreview();
    }

    const bool flow = surface.flows && budget != nullptr;
    bool is_positioned = false;
    bool keep_positioned = false;
    BoxState st;

    if (budget) {
        st = budget->BeginSegment(box, has_prefix, wire_text, cursor_xy,
                                  left_x, top_y, surface.flows,
                                  is_positioned, keep_positioned);
    } else if (surface.flows && !has_prefix && cursor_xy != nullptr &&
               (raw_x != left_x || raw_y != top_y)) {
        is_positioned = true;
    }

    // Positioned segments strip the bogus 0x05 prefix induced by "　",
    // reclaiming the full 71-column width and left margin.
    const std::size_t reserved = (has_prefix && !is_positioned) ? CHAIN_INDENT : 0;

    const std::size_t used = st.lines;
    // A normal continuation keeps filling the line its box left open.
    const bool joins_line = flow && !is_positioned && has_prefix && st.line_open;
    const std::size_t start_col = joins_line ? st.column : 0;
    const std::size_t avail = used >= surface_lines ? 0 : surface_lines - used;
    out.box_lines_before = used;

    // Every path must commit (miss or hit): lines added and the pen after.
    auto commit = [&](std::size_t added, std::size_t column, bool line_open) {
        out.lines = added;
        out.box_lines_after = used + added;
        if (budget) {
            BoxState next;
            next.lines = used + added;
            next.column = column;
            next.line_open = line_open;
            budget->CommitSegment(box, next, out.more_follows, keep_positioned);
        }
    };

    if (key_len == 0) {
        if (budget && surface.flows && !has_prefix && out.tail > 0) {
            budget->NoteNoClearNewline();
        }
        // Control bytes only: the break ends the open line, or draws a blank
        // one when no line was open.
        commit(flow && st.line_open ? 0 : 1, 0, false);
        return SubstitutionStatus::EmptyKey;
    }

    std::string value;
    if (!LookupTranslation(effective, len, key_len, translations, value)) {
        // Miss: strip furigana (would overlap at this pitch); otherwise leave
        // the engine's own buffer alone.
        const std::string wire(effective, len);
        if (CarriesRuby(wire)) {
            out.bytes.clear();
            if (has_prefix) out.bytes.assign(1, *wire_text);
            out.bytes += StripRubyForDisplay(wire);
        }
        commit(EngineDrawnLines(effective, key_len, reserved), 0, false);
        return SubstitutionStatus::NotFound;
    }

    // Strip value's trailing breaks; the wire's own 0x0A chains the box.
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();

    if (is_positioned) {
        // SET_CURSOR-positioned segment (yuzu_2g): flow multi-segment boxes as
        // a continuous paragraph by joining the open line with a 1-column space
        // gap when room remains, or start a fresh left-aligned 30 px row when
        // opening the box (or when the previous line filled all 71 columns).
        const bool can_join_open =
            used > 0 && st.line_open && st.column + 2 <= SINGLE_LINE_MAX;

        if (avail == 0 && !can_join_open) {
            out.dropped = true;
            out.truncated = true;
            out.bytes.append(effective + key_len, out.tail);
            commit(0, 0, false);
            return SubstitutionStatus::Substituted;
        }

        bool join_open = can_join_open;
        WrappedText wrapped =
            join_open
                ? WrapForMessageBox(value, avail + 1, 0, st.column + 1, true)
                : WrapForMessageBox(value, avail, 0, 0, false);

        if (join_open && !wrapped.text.empty() && wrapped.text[0] == '\n') {
            // The first word was wider than the open line's remaining columns,
            // so WrapForMessageBox moved the entire segment to the next row.
            wrapped.text.erase(0, 1);
            if (wrapped.lines > 0) --wrapped.lines;
            join_open = false;
        }

        if (cursor_xy) {
            if (join_open) {
                cursor_xy[0] = left_x +
                               static_cast<int>(st.column + 1) * HALFWIDTH_GLYPH_PX;
                cursor_xy[1] = top_y + static_cast<int>(used - 1) * pitch_px;
            } else {
                cursor_xy[0] = left_x;
                cursor_xy[1] = top_y + static_cast<int>(used) * pitch_px;
            }
        } else if (join_open) {
            out.bytes += ' ';
        } else if (used > 0) {
            out.bytes += '\n';
        }

        out.truncated = wrapped.truncated;
        out.bytes.append(wrapped.text);

        std::size_t lines_drawn = wrapped.lines;
        if (surface.max_bytes) {
            const std::size_t room = surface.max_bytes > out.tail
                                ? surface.max_bytes - out.tail : 0;
            if (TrimToBytes(out.bytes, room)) {
                out.truncated = true;
                lines_drawn = LinesIn(out.bytes);
            }
        }
        out.bytes.append(effective + key_len, out.tail);

        const std::size_t added = lines_drawn - (join_open ? 1 : 0);
        const std::size_t column = wrapped.end_columns;
        const bool line_open = !out.truncated && (column + 2 <= SINGLE_LINE_MAX);
        commit(added, column, line_open);
        return SubstitutionStatus::Substituted;
    }

    if (has_prefix) out.bytes.assign(1, *wire_text);

    if (avail == 0 && !joins_line) {
        // Box full -- drop this segment entirely.
        out.dropped = true;
        out.truncated = true;
        out.bytes.append(effective + key_len, out.tail);
        commit(0, 0, false);
        return SubstitutionStatus::Substituted;
    }

    const std::size_t wrap_lines = avail + (joins_line ? 1 : 0);
    WrappedText wrapped = WrapForMessageBox(value, wrap_lines, reserved,
                                            start_col, joins_line);
    out.truncated = wrapped.truncated;

    if (!flow) {
        if (!wrapped.truncated && out.more_follows &&
            used + wrapped.lines >= surface_lines) {
            // Fills the box and another segment follows -- mark the loss.
            MarkLastLineTruncated(wrapped.text, reserved);
            out.truncated = true;
        }
        out.bytes.append(wrapped.text);

        // Trim to the surface's byte budget (the tail must survive).
        std::size_t lines = wrapped.lines;
        if (surface.max_bytes) {
            const std::size_t room = surface.max_bytes > out.tail
                                ? surface.max_bytes - out.tail : 0;
            if (TrimToBytes(out.bytes, room)) {
                out.truncated = true;
                lines = LinesIn(out.bytes);
            }
        }

        out.bytes.append(effective + key_len, out.tail);
        commit(lines, 0, false);
        return SubstitutionStatus::Substituted;
    }

    // A flowing surface draws the chain as one paragraph: the wire's trailing
    // 0x0A becomes a joining space, and the pen column carries to the next
    // segment so it keeps filling the line.
    out.bytes.append(wrapped.text);

    std::size_t lines_drawn = wrapped.lines;
    if (surface.max_bytes) {
        // One byte of the budget is kept for the joining space.
        const std::size_t reserve = out.more_follows ? 1u : 0u;
        const std::size_t room = surface.max_bytes > reserve
                            ? surface.max_bytes - reserve : 1;
        if (TrimToBytes(out.bytes, room)) {
            out.truncated = true;
            lines_drawn = LinesIn(out.bytes);
        }
    }
    std::size_t added = lines_drawn - (joins_line ? 1 : 0);

    std::size_t column = 0;
    bool   line_open = false;
    if (out.more_follows && !out.truncated) {
        if (wrapped.end_columns + 2 <= SINGLE_LINE_MAX) {
            out.bytes += ' ';
            column = wrapped.end_columns + 1;
            line_open = true;
        } else if (used + added < surface_lines) {
            // The line is full; open the next one for the segment to come.
            out.bytes += '\n';
            ++added;
            column = reserved;
            line_open = true;
        } else {
            // Box and line both full with more coming -- mark the loss.
            MarkLastLineTruncated(out.bytes, reserved,
                                  lines_drawn > 1 ? 0 : start_col);
            out.truncated = true;
        }
    }
    commit(added, column, line_open);
    return SubstitutionStatus::Substituted;
}

}  // namespace

SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     Substitution& out) {
    return Build(wire_text, translations, nullptr, 0, DIALOGUE_PANEL, out);
}

SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     BoxLineBudget& budget, std::uintptr_t box,
                                     Substitution& out) {
    return Build(wire_text, translations, &budget, box, DIALOGUE_PANEL, out);
}

SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     BoxLineBudget& budget, std::uintptr_t box,
                                     const Surface& surface, Substitution& out) {
    return Build(wire_text, translations, &budget, box, surface, out);
}

SubstitutionStatus BuildSubstitution(const char* wire_text,
                                     const TranslationMap& translations,
                                     BoxLineBudget& budget, std::uintptr_t box,
                                     const Surface& surface, Substitution& out,
                                     int* cursor_xy,
                                     const int* box_rect,
                                     int line_pitch) {
    return Build(wire_text, translations, &budget, box, surface, out,
                 cursor_xy, box_rect, line_pitch);
}

}  // namespace translator_logic
