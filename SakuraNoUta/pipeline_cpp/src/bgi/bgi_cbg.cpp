// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "bgi_cbg.h"

#include <cstring>
#include <stdexcept>

namespace snu::bgi {

namespace {

// Same asmodean PRNG as DSC; verified against the real archives.
std::uint32_t cbg_hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & 0xFFFF) * 20021u;
    std::uint32_t edx = ((state >> 16) & 0xFFFF) * 20021u + state * 346u;
    edx = edx + (eax >> 16);
    state = (((edx & 0xFFFF) << 16) | (eax & 0xFFFF)) + 1u;
    return edx & 0x7FFF;
}

class BitReader {
public:
    BitReader(const Bytes& data, std::size_t pos) : data_(data), pos_(pos) {}
    int read_bit() {
        if (count_ == 0) {
            if (pos_ >= data_.size())
                throw std::runtime_error("CBG: bit stream exhausted");
            buf_ = data_[pos_++];
            count_ = 8;
        }
        int b = (buf_ >> 7) & 1;
        buf_ = static_cast<std::uint8_t>(buf_ << 1);
        --count_;
        return b;
    }

private:
    const Bytes& data_;
    std::size_t pos_;
    std::uint8_t buf_ = 0;
    int count_ = 0;
};

std::uint16_t rd16(const Bytes& d, std::size_t off) {
    std::uint16_t v;
    std::memcpy(&v, d.data() + off, 2);
    return v;
}
std::uint32_t rd32(const Bytes& d, std::size_t off) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

std::uint32_t read_varint(const Bytes& buf, std::size_t& p) {
    std::uint32_t v = 0;
    int shift = 0;
    for (;;) {
        if (p >= buf.size())
            throw std::runtime_error("CBG: varint stream exhausted");
        std::uint8_t b = buf[p++];
        v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

}  // namespace

CbgImage decode_cbg(const Bytes& data) {
    if (!is_cbg(data))
        throw std::runtime_error("not a CompressedBG file");

    // Header layout (pinned against real archives; several published refs
    // get the key/enc/checksum offsets wrong).
    const int width = rd16(data, 0x10);
    const int height = rd16(data, 0x12);
    const int bpp = rd16(data, 0x14);
    const std::uint32_t inter_size = rd32(data, 0x20);
    std::uint32_t key = rd32(data, 0x24);
    const std::uint32_t enc_len = rd32(data, 0x28);
    const std::uint8_t sum_chk = data[0x2C];
    const std::uint8_t xor_chk = data[0x2D];
    const std::uint16_t version = rd16(data, 0x2E);

    if (version != 1)
        throw std::runtime_error("CBG: unsupported version " +
                                 std::to_string(version) +
                                 " (only v1; v2 is the DCT format -- use "
                                 "TOOLS/garbro/extract_cbg.exe)");
    if (data.size() < 0x30 + static_cast<std::size_t>(enc_len))
        throw std::runtime_error("CBG: truncated leaf table");

    // ---- Step 1: decrypt + checksum the enc_len-byte leaf-table block ----
    Bytes plain(enc_len);
    std::uint32_t state = key;
    std::uint8_t sum_b = 0, xor_b = 0;
    for (std::uint32_t i = 0; i < enc_len; ++i) {
        std::uint32_t adv = cbg_hash_update(state);
        std::uint8_t byte = static_cast<std::uint8_t>(data[0x30 + i] - adv);
        plain[i] = byte;
        sum_b = static_cast<std::uint8_t>(sum_b + byte);
        xor_b = static_cast<std::uint8_t>(xor_b ^ byte);
    }
    if (sum_b != sum_chk || xor_b != xor_chk)
        throw std::runtime_error("CBG: leaf-table checksum mismatch");

    // 256 varint leaf weights from the decrypted block.
    std::uint32_t leaf_weights[256];
    {
        std::size_t p = 0;
        for (int i = 0; i < 256; ++i)
            leaf_weights[i] = read_varint(plain, p);
    }
    std::size_t pos = 0x30 + enc_len;  // Huffman bit stream starts here

    // ---- Step 2: build Huffman tree (merge two smallest) ------------------
    constexpr int NODE_COUNT = 511;
    std::uint32_t weights[NODE_COUNT] = {};
    bool is_leaf[NODE_COUNT] = {};
    int left_c[NODE_COUNT] = {};
    int right_c[NODE_COUNT] = {};

    int valid = 0;
    for (int i = 0; i < 256; ++i) {
        weights[i] = leaf_weights[i];
        is_leaf[i] = true;
        if (leaf_weights[i] > 0) ++valid;
    }

    int next_int = 256;
    while (valid > 1) {
        int min1 = -1, min2 = -1;
        std::uint32_t w1 = 0x7FFFFFFF, w2 = 0x7FFFFFFF;
        for (int i = 0; i < next_int; ++i) {
            std::uint32_t w = weights[i];
            if (w == 0) continue;
            if (w < w1) {
                w2 = w1; min2 = min1;
                w1 = w;  min1 = i;
            } else if (w < w2) {
                w2 = w; min2 = i;
            }
        }
        weights[next_int] = w1 + w2;
        is_leaf[next_int] = false;
        left_c[next_int] = min1;
        right_c[next_int] = min2;
        weights[min1] = 0;
        weights[min2] = 0;
        ++next_int;
        --valid;
    }
    const int root = next_int - 1;

    // ---- Step 3: Huffman-decode inter_size bytes --------------------------
    BitReader br(data, pos);
    Bytes decoded(inter_size);
    for (std::uint32_t out = 0; out < inter_size; ++out) {
        int n = root;
        while (!is_leaf[n])
            n = br.read_bit() ? right_c[n] : left_c[n];
        decoded[out] = static_cast<std::uint8_t>(n & 0xFF);
    }

    // ---- Step 4: RLE expansion (alternating zero / data runs) -------------
    const int pix_bytes = (bpp + 7) / 8;
    const std::size_t expand_size =
        static_cast<std::size_t>(width) * height * pix_bytes;
    Bytes expanded(expand_size, 0);

    std::size_t src = 0, dst = 0;
    std::uint32_t first = read_varint(decoded, src);
    bool is_zero_run = (first & 1) == 1;
    std::uint32_t run_len = first >> 1;

    while (dst < expand_size) {
        if (is_zero_run) {
            dst += run_len;  // already zero
        } else {
            for (std::uint32_t k = 0; k < run_len; ++k) {
                if (src >= decoded.size() || dst >= expand_size) break;
                expanded[dst++] = decoded[src++];
            }
        }
        if (dst >= expand_size) break;
        is_zero_run = !is_zero_run;
        run_len = read_varint(decoded, src);
    }

    // ---- Step 5: differential decode --------------------------------------
    const int px = pix_bytes;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * px;

    // First row: pixel = prev_left + delta
    for (int x = 1; x < width; ++x)
        for (int c = 0; c < px; ++c) {
            std::size_t i = static_cast<std::size_t>(x) * px + c;
            expanded[i] = static_cast<std::uint8_t>(expanded[i] + expanded[i - px]);
        }

    // Subsequent rows
    for (int y = 1; y < height; ++y) {
        std::size_t row = static_cast<std::size_t>(y) * row_bytes;
        std::size_t prev = row - row_bytes;
        for (int c = 0; c < px; ++c)
            expanded[row + c] =
                static_cast<std::uint8_t>(expanded[row + c] + expanded[prev + c]);
        for (int x = 1; x < width; ++x) {
            std::size_t base = row + static_cast<std::size_t>(x) * px;
            for (int c = 0; c < px; ++c) {
                unsigned left = expanded[base - px + c];
                unsigned above = expanded[base - row_bytes + c];
                expanded[base + c] = static_cast<std::uint8_t>(
                    expanded[base + c] + ((left + above) >> 1));
            }
        }
    }

    CbgImage img;
    img.pixels = std::move(expanded);
    img.width = width;
    img.height = height;
    img.bpp = bpp;
    return img;
}

Bytes to_bmp(const Bytes& pixels_bgr, int width, int height, int bpp) {
    const int px = (bpp + 7) / 8;
    const std::size_t row_size = ((static_cast<std::size_t>(width) * px + 3) / 4) * 4;
    const std::size_t image_size = row_size * height;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * px;

    Bytes body;
    body.reserve(image_size);
    for (int y = height - 1; y >= 0; --y) {
        std::size_t start = static_cast<std::size_t>(y) * row_bytes;
        body.insert(body.end(), pixels_bgr.begin() + static_cast<std::ptrdiff_t>(start),
                    pixels_bgr.begin() + static_cast<std::ptrdiff_t>(start + row_bytes));
        body.insert(body.end(), row_size - row_bytes, 0);
    }

    auto put16 = [](Bytes& b, std::uint16_t v) {
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto put32 = [](Bytes& b, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };

    Bytes bmp;
    bmp.reserve(14 + 40 + body.size());
    bmp.push_back('B'); bmp.push_back('M');
    put32(bmp, static_cast<std::uint32_t>(14 + 40 + body.size()));
    put16(bmp, 0); put16(bmp, 0);
    put32(bmp, 14 + 40);
    put32(bmp, 40);
    put32(bmp, static_cast<std::uint32_t>(width));
    put32(bmp, static_cast<std::uint32_t>(height));
    put16(bmp, 1); put16(bmp, static_cast<std::uint16_t>(bpp));
    put32(bmp, 0);                                     // BI_RGB
    put32(bmp, static_cast<std::uint32_t>(image_size));
    put32(bmp, 2835); put32(bmp, 2835);                // ~72 DPI
    put32(bmp, 0); put32(bmp, 0);
    bmp.insert(bmp.end(), body.begin(), body.end());
    return bmp;
}

}  // namespace snu::bgi
