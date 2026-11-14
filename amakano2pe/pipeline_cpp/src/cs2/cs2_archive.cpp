// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cs2_archive.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// The low-level BF_* API is deprecated in OpenSSL 3 but still shipped; it is
// the only way to get raw single-block ECB Blowfish without going through the
// legacy provider, which would need extra runtime configuration.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/blowfish.h>

#include <zlib.h>

namespace ama::cs2 {

// ---- MT19937 ---------------------------------------------------------------

namespace {

constexpr std::uint32_t MT_M = 397;
constexpr std::uint32_t MT_F = 69069;
constexpr std::uint32_t MT_MATRIX_A = 0x9908b0dfu;
constexpr int MT_SHIFT_U = 11, MT_SHIFT_S = 7, MT_SHIFT_T = 15, MT_SHIFT_L = 18;
constexpr std::uint32_t MT_MASK_B = 0x9d2c5680u, MT_MASK_C = 0xefc60000u;

std::uint32_t mt_temper(std::uint32_t y) {
    y ^= (y >> MT_SHIFT_U);
    y ^= (y << MT_SHIFT_S) & MT_MASK_B;
    y ^= (y << MT_SHIFT_T) & MT_MASK_C;
    y ^= (y >> MT_SHIFT_L);
    return y;
}

std::uint32_t rd_u16(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8);
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

}  // namespace

std::uint32_t mt_genrand(std::uint32_t seed) {
    const std::uint32_t mag01[2] = {0u, MT_MATRIX_A};

    // state[0]
    std::uint32_t y = seed & 0x80000000u;
    seed = MT_F * seed + 1;
    seed = MT_F * seed + 1;
    // state[1]
    y |= seed & 0x7fff0000u;
    seed = MT_F * seed + 1;
    y |= (seed & 0xffff0000u) >> 16;
    seed = MT_F * seed + 1;

    y = (y >> 1) ^ mag01[y & 0x1u];
    for (std::uint32_t i = 2; i < MT_M; ++i) {
        seed = MT_F * seed + 1;
        seed = MT_F * seed + 1;
    }
    y ^= seed & 0xffff0000u;
    seed = MT_F * seed + 1;
    y ^= (seed & 0xffff0000u) >> 16;

    return mt_temper(y);
}

// ---- KIF archive -----------------------------------------------------------

struct KifArchive::Cipher {
    BF_KEY key{};

    // Decrypt one 8-byte block.  BF_ecb_encrypt reads and writes the block as
    // two BIG-endian uint32s, so callers must byteswap the archive's
    // little-endian (L, R) pairs around this call.
    void decrypt_block(const std::uint8_t* in, std::uint8_t* out) const {
        BF_ecb_encrypt(in, out, &key, BF_DECRYPT);
    }
};

namespace {

void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t get_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

constexpr const char KEYDAT[] = "__key__.dat";

// Case-insensitive "__key__.dat" followed by its NUL terminator.
bool is_keydat(const std::uint8_t* name, std::size_t n) {
    const std::size_t len = std::strlen(KEYDAT);
    if (n < len + 1) return false;
    for (std::size_t i = 0; i < len; ++i)
        if (std::tolower(name[i]) != KEYDAT[i]) return false;
    return name[len] == 0;
}

std::string decode_name(const std::uint8_t* name, std::size_t n) {
    std::size_t end = 0;
    while (end < n && name[end] != 0) ++end;
    auto s = cp932_to_utf8_strict(name, end);
    if (!s) throw std::runtime_error("KIF entry name is not valid CP932");
    return *s;
}

// ---- filename rotation -------------------------------------------------------

constexpr char kForwardAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr char kReverseAlphabet[] = "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA";
constexpr std::uint32_t kAlphabetSize = sizeof(kForwardAlphabet) - 1;
constexpr std::uint32_t kTocSeedInit = 0xFFFFFFFFu;
constexpr std::uint32_t kTocSeedPolynomial = 0x04C11DB7u;
constexpr std::uint32_t kTocSeedTopBit = 0x80000000u;
constexpr int kTocSeedByteShift = 24;
constexpr int kBitsPerByte = 8;
constexpr std::uint32_t kByteMask = 0xFFu;

int alphabet_index(const char* alphabet, char c) {
    for (std::uint32_t i = 0; i < kAlphabetSize; ++i) {
        if (alphabet[i] == c) return static_cast<int>(i);
    }
    return -1;
}

std::uint32_t initial_name_shift(std::uint32_t toc_seed, std::uint32_t entry_index) {
    // The shift is the byte sum of the first MT19937 word.
    const std::uint32_t k = mt_genrand(toc_seed + entry_index);
    std::uint32_t sum = 0;
    for (int byte = 0; byte < static_cast<int>(sizeof(k)); ++byte) {
        sum += k >> (byte * kBitsPerByte);
    }
    return sum & kByteMask;
}

// ---- executable resources ----------------------------------------------------

constexpr std::uint8_t kKeyCodeXor = 0xCD;
constexpr std::size_t kBlowfishBlock = 8;
constexpr std::size_t kBlowfishHalf = 4;

struct ModuleDeleter {
    void operator()(HMODULE module) const noexcept {
        if (module != nullptr) FreeLibrary(module);
    }
};
using ScopedModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

Bytes load_resource(HMODULE module, const wchar_t* type, const wchar_t* name) {
    HRSRC info = FindResourceW(module, name, type);
    if (info == nullptr) return {};
    HGLOBAL handle = LoadResource(module, info);
    if (handle == nullptr) return {};
    const auto* data = static_cast<const std::uint8_t*>(LockResource(handle));
    const DWORD size = SizeofResource(module, info);
    if (data == nullptr || size == 0) return {};
    return Bytes(data, data + size);
}

std::wstring utf8_path_to_wide(const std::string& s) {
    return std::filesystem::u8path(s).wstring();
}

}  // namespace

KifNameCipher::KifNameCipher(const std::string& vcode2) {
    std::uint32_t seed = kTocSeedInit;
    for (const char c : vcode2) {
        seed ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << kTocSeedByteShift;
        for (int bit = 0; bit < kBitsPerByte; ++bit) {
            const bool carry = (seed & kTocSeedTopBit) != 0;
            seed <<= 1;
            if (carry) seed ^= kTocSeedPolynomial;
        }
        seed = ~seed;
    }
    toc_seed_ = seed;
}

std::string KifNameCipher::decode(const std::string& stored, std::uint32_t entry_index) const {
    std::uint32_t shift = initial_name_shift(toc_seed_, entry_index);
    std::string out;
    out.reserve(stored.size());
    for (const char c : stored) {
        if (c == '\0') break;
        const int idx = alphabet_index(kReverseAlphabet, c);
        if (idx < 0) {
            out.push_back(c);
        } else {
            const std::uint32_t back = shift % kAlphabetSize;
            out.push_back(kForwardAlphabet[(static_cast<std::uint32_t>(idx) + kAlphabetSize - back) %
                                           kAlphabetSize]);
        }
        ++shift;
    }
    return out;
}

std::string KifNameCipher::encode(const std::string& plain, std::uint32_t entry_index) const {
    std::uint32_t shift = initial_name_shift(toc_seed_, entry_index);
    std::string out;
    out.reserve(plain.size());
    for (const char c : plain) {
        if (c == '\0') break;
        const int idx = alphabet_index(kForwardAlphabet, c);
        if (idx < 0) {
            out.push_back(c);
        } else {
            out.push_back(kReverseAlphabet[(static_cast<std::uint32_t>(idx) + shift) % kAlphabetSize]);
        }
        ++shift;
    }
    return out;
}

std::string read_vcode2_from_exe(const std::string& exe_path) {
    const ScopedModule module(LoadLibraryExW(utf8_path_to_wide(exe_path).c_str(), nullptr,
                                             LOAD_LIBRARY_AS_DATAFILE |
                                                 LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (!module) throw std::runtime_error("cannot load resources of " + exe_path);
    Bytes key = load_resource(module.get(), L"KEY_CODE", L"KEY");
    Bytes code = load_resource(module.get(), L"V_CODE2", L"DATA");
    if (key.empty() || code.empty())
        throw std::runtime_error("KEY_CODE / V_CODE2 resources missing in " + exe_path);
    for (auto& b : key) b ^= kKeyCodeXor;

    BF_KEY bf{};
    BF_set_key(&bf, static_cast<int>(key.size()), key.data());
    for (std::size_t i = 0; i + kBlowfishBlock <= code.size(); i += kBlowfishBlock) {
        std::uint8_t block[kBlowfishBlock];
        std::uint8_t plain[kBlowfishBlock];
        put_be32(block, rd_u32(code, i));
        put_be32(block + kBlowfishHalf, rd_u32(code, i + kBlowfishHalf));
        BF_ecb_encrypt(block, plain, &bf, BF_DECRYPT);
        const std::uint32_t l = get_be32(plain);
        const std::uint32_t r = get_be32(plain + kBlowfishHalf);
        for (std::size_t k = 0; k < kBlowfishHalf; ++k) {
            code[i + k] = static_cast<std::uint8_t>(l >> (k * kBitsPerByte));
            code[i + kBlowfishHalf + k] = static_cast<std::uint8_t>(r >> (k * kBitsPerByte));
        }
    }
    const auto nul = std::find(code.begin(), code.end(), std::uint8_t{0});
    return std::string(code.begin(), nul);
}

Bytes read_kif_raw_slice(const std::string& archive_path, std::uint64_t offset,
                         std::uint32_t length) {
    std::ifstream f(std::filesystem::u8path(archive_path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + archive_path);
    f.seekg(static_cast<std::streamoff>(offset));
    if (!f) throw std::runtime_error("seek past end of " + archive_path);
    Bytes buf(length);
    if (length == 0) return buf;
    f.read(reinterpret_cast<char*>(buf.data()), length);
    if (static_cast<std::uint32_t>(f.gcount()) != length)
        throw std::runtime_error("short read of " + std::to_string(length) + " bytes at " +
                                 std::to_string(offset) + " in " + archive_path);
    return buf;
}

KifArchive::KifArchive(const std::string& path) : path_(path) { open(nullptr); }

KifArchive::KifArchive(const std::string& path, const KifNameCipher& names) : path_(path) {
    open(&names);
}

void KifArchive::open(const KifNameCipher* names) {
    const std::string& path = path_;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);

    std::uint8_t hdr[8];
    f.read(reinterpret_cast<char*>(hdr), 8);
    if (!f) throw std::runtime_error("truncated KIF header: " + path);
    if (std::memcmp(hdr, "KIF\x00", 4) != 0)
        throw std::runtime_error("not a KIF archive: " + path);
    const std::uint32_t count =
        static_cast<std::uint32_t>(hdr[4]) | (static_cast<std::uint32_t>(hdr[5]) << 8) |
        (static_cast<std::uint32_t>(hdr[6]) << 16) | (static_cast<std::uint32_t>(hdr[7]) << 24);
    if (count == 0) return;

    // KIF v2 = 64-byte names.  Amakano 2 PE uses it.
    constexpr std::size_t ENTRY_SIZE = 64 + 8;
    Bytes raw(static_cast<std::size_t>(count) * ENTRY_SIZE);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!f) throw std::runtime_error("truncated KIF entry table: " + path);

    // First entry must be __key__.dat -- derive the Blowfish key from its
    // length field.
    if (!is_keydat(raw.data(), 64))
        throw std::runtime_error("first entry is not __key__.dat: " + path);
    const std::uint32_t first_len = rd_u32(raw, 64 + 4);

    const std::uint32_t key_int = mt_genrand(first_len);
    std::uint8_t key_le[4] = {
        static_cast<std::uint8_t>(key_int), static_cast<std::uint8_t>(key_int >> 8),
        static_cast<std::uint8_t>(key_int >> 16), static_cast<std::uint8_t>(key_int >> 24)};
    cipher_ = std::make_unique<Cipher>();
    BF_set_key(&cipher_->key, 4, key_le);

    entries_.reserve(count - 1);
    for (std::uint32_t i = 1; i < count; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * ENTRY_SIZE;
        const std::uint32_t offset = rd_u32(raw, base + 64);
        const std::uint32_t length = rd_u32(raw, base + 68);

        std::uint8_t block[8], plain[8];
        put_be32(block, offset + i);
        put_be32(block + 4, length);
        cipher_->decrypt_block(block, plain);

        KifEntry e;
        if (names == nullptr) {
            e.name = decode_name(raw.data() + base, 64);
        } else {
            const auto* stored = reinterpret_cast<const char*>(raw.data() + base);
            const std::string plain_name = names->decode(std::string(stored, stored + 64), i);
            e.name = decode_name(reinterpret_cast<const std::uint8_t*>(plain_name.data()),
                                 plain_name.size());
        }
        e.offset = get_be32(plain);
        e.length = get_be32(plain + 4);
        e.index = i;
        entries_.push_back(std::move(e));
    }
}

KifArchive::~KifArchive() = default;

const KifEntry* KifArchive::find(const std::string& name) const {
    const auto same = [&name](const KifEntry& e) {
        if (e.name.size() != name.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto a = static_cast<unsigned char>(e.name[i]);
            const auto b = static_cast<unsigned char>(name[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    };
    const auto it = std::find_if(entries_.begin(), entries_.end(), same);
    return it == entries_.end() ? nullptr : &*it;
}

Bytes KifArchive::raw_slice(const KifEntry& entry) const {
    return read_kif_raw_slice(path_, entry.offset, entry.length);
}

Bytes KifArchive::extract(const KifEntry& entry) const {
    if (entry.length == 0) return {};
    std::ifstream f(std::filesystem::u8path(path_), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path_);
    f.seekg(entry.offset);
    Bytes buf(entry.length);
    f.read(reinterpret_cast<char*>(buf.data()), entry.length);
    // A short read at EOF must not be fatal: shrink to what actually arrived so
    // a truncated archive still yields every complete block before the cut.
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return decrypt_slice(std::move(buf));
}

Bytes KifArchive::decrypt_slice(Bytes buf) const {
    // CatSystem2 stores each 8-byte payload block as a LITTLE-endian (L, R)
    // uint32 pair, but the Blowfish round expects big-endian -- byteswap each
    // block before and after.
    const std::size_t aligned = buf.size() & ~std::size_t{7};
    for (std::size_t i = 0; i + 8 <= aligned; i += 8) {
        const std::uint32_t l = rd_u32(buf, i), r = rd_u32(buf, i + 4);
        std::uint8_t block[8], plain[8];
        put_be32(block, l);
        put_be32(block + 4, r);
        cipher_->decrypt_block(block, plain);
        const std::uint32_t pl = get_be32(plain), pr = get_be32(plain + 4);
        buf[i + 0] = static_cast<std::uint8_t>(pl);
        buf[i + 1] = static_cast<std::uint8_t>(pl >> 8);
        buf[i + 2] = static_cast<std::uint8_t>(pl >> 16);
        buf[i + 3] = static_cast<std::uint8_t>(pl >> 24);
        buf[i + 4] = static_cast<std::uint8_t>(pr);
        buf[i + 5] = static_cast<std::uint8_t>(pr >> 8);
        buf[i + 6] = static_cast<std::uint8_t>(pr >> 16);
        buf[i + 7] = static_cast<std::uint8_t>(pr >> 24);
    }
    return buf;
}

// ---- CatScene script -------------------------------------------------------

std::string scene_line_type_name(std::uint16_t kind) {
    switch (static_cast<SceneLineType>(kind)) {
    case SceneLineType::NONE: return "NONE";
    case SceneLineType::INPUT: return "INPUT";
    case SceneLineType::PAGE: return "PAGE";
    case SceneLineType::MESSAGE: return "MESSAGE";
    case SceneLineType::NAME: return "NAME";
    case SceneLineType::COMMAND: return "COMMAND";
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04x", kind);
    return buf;
}

namespace {

Bytes zlib_inflate(const std::uint8_t* src, std::size_t n) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw std::runtime_error("inflateInit failed");
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(n);

    Bytes out;
    Bytes chunk(64 * 1024);
    int rc = Z_OK;
    do {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("zlib decompress failed");
        }
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(chunk.size() - zs.avail_out));
        if (rc == Z_BUF_ERROR && zs.avail_out != 0) break;
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

}  // namespace

SceneScript::SceneScript(const Bytes& data) {
    if (data.size() < 16) throw std::runtime_error("not a CatScene script: too small");
    if (std::memcmp(data.data(), "CatScene", 8) != 0)
        throw std::runtime_error("not a CatScene script: bad signature");
    const std::uint32_t comp_len = rd_u32(data, 8);
    if (16 + static_cast<std::size_t>(comp_len) > data.size())
        throw std::runtime_error("CatScene payload truncated");

    // orig_len (offset 12) is informational and deliberately unchecked: the
    // inflated size is authoritative, and a header mismatch is not fatal.
    Bytes script = zlib_inflate(data.data() + 16, comp_len);
    if (script.size() < 16) throw std::runtime_error("CatScene script header truncated");

    const std::uint32_t input_num = rd_u32(script, 4);
    const std::uint32_t table_off = rd_u32(script, 8);
    const std::uint32_t string_off = rd_u32(script, 12);
    (void)input_num;

    const std::size_t offsets_start = 16 + static_cast<std::size_t>(table_off);
    const std::size_t strings_start = 16 + static_cast<std::size_t>(string_off);
    const std::size_t line_count =
        string_off >= table_off ? (string_off - table_off) / 4 : 0;

    lines_.reserve(line_count);
    for (std::size_t i = 0; i < line_count; ++i) {
        const std::size_t ofs_pos = offsets_start + i * 4;
        if (ofs_pos + 4 > script.size()) throw std::runtime_error("CatScene offset table overruns");
        const std::size_t off = strings_start + rd_u32(script, ofs_pos);
        if (off + 2 > script.size()) throw std::runtime_error("CatScene line offset overruns");

        SceneLine line;
        line.idx = static_cast<int>(i);
        line.kind = static_cast<std::uint16_t>(rd_u16(script, off));
        std::size_t end = off + 2;
        while (end < script.size() && script[end] != 0) ++end;
        if (end >= script.size()) throw std::runtime_error("CatScene line is unterminated");
        const std::size_t len = end - (off + 2);
        auto strict = cp932_to_utf8_strict(script.data() + off + 2, len);
        line.content = strict ? *strict : cp932_to_utf8_replace(script.data() + off + 2, len);
        lines_.push_back(std::move(line));
    }
}

}  // namespace ama::cs2
