// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "bgi/bgi_archive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace snu::bgi {

namespace {

constexpr std::size_t kDscMagicBytes = 16;
constexpr std::size_t kDscHeaderBytes = 32;
constexpr std::size_t kDscKeyOffset = 16;
constexpr std::size_t kDscDecSizeOffset = 20;
constexpr int kHuffmanAlphabetSize = 512;
constexpr std::size_t kMaxHuffmanTreeNodes = 1024;
constexpr int kFirstMatchSymbol = 256;
constexpr std::size_t kMinEncodedMatchLen = 2;
constexpr std::size_t kMinCompressMatchLen = 3;
constexpr std::size_t kMaxMatchLen = 257;
constexpr std::size_t kMinMatchOffset = 2;
constexpr std::size_t kMaxMatchOffset = 4097;
constexpr int kMatchOffsetBits = 12;
constexpr int kMaxHuffmanCodeBits = 15;
constexpr std::uint32_t kKraftScaleOne = 1u << kMaxHuffmanCodeBits;
constexpr int kBitsPerByte = 8;
constexpr int kTopBitShift = 7;
constexpr std::uint32_t kByteMask = 0xFFu;

constexpr std::uint32_t kPrngWordMask = 0xFFFFu;
constexpr std::uint32_t kPrngMulLo = 20021u;
constexpr std::uint32_t kPrngMulHi = 346u;
constexpr int kPrngWordShift = 16;
constexpr std::uint32_t kPrngReturnMask = 0x7FFFu;

constexpr std::size_t kLzHashBuckets = 1u << 14;
constexpr std::size_t kLzHashMask = kLzHashBuckets - 1u;
constexpr std::uint32_t kKnuthHashMul = 2654435761u;
constexpr int kLzHashShift = 18;
constexpr std::size_t kLzChainRing = 1u << 13;
constexpr std::size_t kLzChainMask = kLzChainRing - 1u;
constexpr int kMaxChainSteps = 64;

constexpr std::size_t kArcHeaderBytes = 16;
constexpr std::size_t kArcMagicBytes = 12;
constexpr std::size_t kArcRecordBytes = 128;
constexpr std::size_t kArcNameFieldBytes = 96;
constexpr std::size_t kArcOffsetFieldPos = 96;
constexpr std::size_t kArcSizeFieldPos = 100;
constexpr std::size_t kArcExtraFieldPos = 104;

// asmodean PRNG: returns advance byte, updates state in place.
std::uint32_t hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & kPrngWordMask) * kPrngMulLo;
    std::uint32_t edx =
        ((state >> kPrngWordShift) & kPrngWordMask) * kPrngMulLo + state * kPrngMulHi;
    edx = edx + (eax >> kPrngWordShift);
    state = (((edx & kPrngWordMask) << kPrngWordShift) | (eax & kPrngWordMask)) + 1u;
    return edx & kPrngReturnMask;
}

class BitReader {
public:
    BitReader(const Bytes& data, std::size_t pos) : data_(data), pos_(pos) {}

    int read_bit() {
        if (count_ == 0) {
            if (pos_ >= data_.size()) {
                throw std::runtime_error("DSC: bit stream exhausted");
            }
            buf_ = data_[pos_++];
            count_ = kBitsPerByte;
        }
        int bit = (buf_ >> kTopBitShift) & 1;
        buf_ = static_cast<std::uint8_t>(buf_ << 1);
        --count_;
        return bit;
    }

    std::uint32_t read_bits(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            v = (v << 1) | static_cast<std::uint32_t>(read_bit());
        }
        return v;
    }

private:
    const Bytes& data_;
    std::size_t pos_;
    std::uint8_t buf_ = 0;
    int count_ = 0;
};

class BitWriter {
public:
    explicit BitWriter(Bytes& dst) : dst_(dst) {}

    void put_bits(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            cur_ = static_cast<std::uint8_t>((cur_ << 1) | ((value >> i) & 1u));
            if (++count_ == kBitsPerByte) {
                dst_.push_back(cur_);
                cur_ = 0;
                count_ = 0;
            }
        }
    }

    void flush() {
        if (count_ > 0) {
            dst_.push_back(static_cast<std::uint8_t>(cur_ << (kBitsPerByte - count_)));
            cur_ = 0;
            count_ = 0;
        }
    }

private:
    Bytes& dst_;
    std::uint8_t cur_ = 0;
    int count_ = 0;
};

std::uint32_t rd32(const Bytes& d, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + off, sizeof(v));
    return v;
}

void wr32(Bytes& d, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    d.insert(d.end(), p, p + sizeof(v));
}

constexpr char ARC_MAGIC[kArcMagicBytes] = {'B', 'U', 'R', 'I', 'K', 'O',
                                            ' ', 'A', 'R', 'C', '2', '0'};
constexpr char DSC_MAGIC[kDscMagicBytes] = "DSC FORMAT 1.00";

struct DscToken {
    std::uint16_t symbol = 0;
    std::uint16_t encoded_offset = 0;
};

std::array<std::uint8_t, kHuffmanAlphabetSize> BuildKraftCompleteCodeLengths(
    const std::array<std::uint64_t, kHuffmanAlphabetSize>& freq) {
    std::array<std::uint8_t, kHuffmanAlphabetSize> lengths{};

    struct ActiveSym {
        std::uint64_t count = 0;
        int symbol = 0;
        int len = 0;
    };
    std::vector<ActiveSym> active;
    active.reserve(kHuffmanAlphabetSize);
    for (int sym = 0; sym < kHuffmanAlphabetSize; ++sym) {
        if (freq[static_cast<std::size_t>(sym)] > 0u) {
            active.push_back(ActiveSym{freq[static_cast<std::size_t>(sym)], sym, 0});
        }
    }

    if (active.empty()) {
        lengths[0] = 1;
        lengths[1] = 1;
        return lengths;
    }
    if (active.size() == 1u) {
        const int only_sym = active[0].symbol;
        const int dummy_sym = (only_sym == 0) ? 1 : 0;
        lengths[static_cast<std::size_t>(only_sym)] = 1;
        lengths[static_cast<std::size_t>(dummy_sym)] = 1;
        return lengths;
    }

    struct BuildNode {
        int left = -1;
        int right = -1;
        int leaf_index = -1;
    };
    std::vector<BuildNode> nodes;
    nodes.reserve(active.size() * 2u);

    using HeapItem = std::pair<std::uint64_t, int>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> pq;
    for (std::size_t i = 0; i < active.size(); ++i) {
        nodes.push_back(BuildNode{-1, -1, static_cast<int>(i)});
        pq.emplace(active[i].count, static_cast<int>(i));
    }

    while (pq.size() > 1u) {
        const auto [w1, n1] = pq.top();
        pq.pop();
        const auto [w2, n2] = pq.top();
        pq.pop();
        const int parent_id = static_cast<int>(nodes.size());
        nodes.push_back(BuildNode{n1, n2, -1});
        pq.emplace(w1 + w2, parent_id);
    }

    const int root = pq.top().second;
    int max_depth = 0;
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
        const auto [nid, depth] = stack.back();
        stack.pop_back();
        const BuildNode& nd = nodes[static_cast<std::size_t>(nid)];
        if (nd.leaf_index >= 0) {
            active[static_cast<std::size_t>(nd.leaf_index)].len = depth;
            max_depth = std::max(max_depth, depth);
            continue;
        }
        stack.emplace_back(nd.left, depth + 1);
        stack.emplace_back(nd.right, depth + 1);
    }

    if (max_depth > kMaxHuffmanCodeBits) {
        std::sort(active.begin(), active.end(), [](const ActiveSym& a, const ActiveSym& b) {
            if (a.count != b.count) {
                return a.count > b.count;
            }
            return a.symbol < b.symbol;
        });
        std::uint32_t kraft = 0;
        for (ActiveSym& item : active) {
            item.len = std::clamp(item.len, 1, kMaxHuffmanCodeBits);
            kraft += (1u << (kMaxHuffmanCodeBits - item.len));
        }
        while (kraft > kKraftScaleOne) {
            for (std::size_t i = active.size(); i > 0u; --i) {
                ActiveSym& item = active[i - 1u];
                if (item.len < kMaxHuffmanCodeBits) {
                    ++item.len;
                    kraft -= (1u << (kMaxHuffmanCodeBits - item.len));
                    break;
                }
            }
        }
        while (kraft < kKraftScaleOne) {
            const std::uint32_t deficit = kKraftScaleOne - kraft;
            for (ActiveSym& item : active) {
                if (item.len <= 1) {
                    continue;
                }
                const std::uint32_t gain = (1u << (kMaxHuffmanCodeBits - item.len));
                if (gain <= deficit) {
                    kraft += gain;
                    --item.len;
                    break;
                }
            }
        }
    }

    std::uint32_t verify_kraft = 0;
    for (const ActiveSym& item : active) {
        lengths[static_cast<std::size_t>(item.symbol)] = static_cast<std::uint8_t>(item.len);
        verify_kraft += (1u << (kMaxHuffmanCodeBits - item.len));
    }
    if (verify_kraft != kKraftScaleOne) {
        throw std::logic_error("DSC: Kraft completeness invariant violated");
    }
    return lengths;
}

}  // namespace

Bytes dsc_decompress(const Bytes& data) {
    if (!is_dsc(data)) {
        throw std::runtime_error("not a DSC FORMAT 1.00 blob");
    }
    if (data.size() < kDscHeaderBytes + static_cast<std::size_t>(kHuffmanAlphabetSize)) {
        throw std::runtime_error("DSC: truncated header or code-length table");
    }

    std::uint32_t hash_key = rd32(data, kDscKeyOffset);
    std::uint32_t dec_size = rd32(data, kDscDecSizeOffset);

    std::uint8_t code_lengths[kHuffmanAlphabetSize];
    std::size_t p = kDscHeaderBytes;
    for (int i = 0; i < kHuffmanAlphabetSize; ++i) {
        std::uint32_t adv = hash_update(hash_key);
        code_lengths[i] = static_cast<std::uint8_t>(data[p + static_cast<std::size_t>(i)] - adv);
    }
    p += kHuffmanAlphabetSize;

    std::vector<std::pair<int, int>> leaves;
    for (int sym = 0; sym < kHuffmanAlphabetSize; ++sym) {
        if (code_lengths[sym] != 0) {
            leaves.emplace_back(code_lengths[sym], sym);
        }
    }
    std::sort(leaves.begin(), leaves.end());

    if (leaves.empty()) {
        return {};
    }

    struct Node {
        int left = 0;
        int right = 0;
        int symbol = 0;
        bool leaf = false;
    };
    std::vector<Node> nodes;
    nodes.reserve(kMaxHuffmanTreeNodes);

    std::vector<std::pair<int, int>> work;
    for (auto [code_size, sym] : leaves) {
        Node n;
        n.leaf = true;
        n.symbol = sym;
        nodes.push_back(n);
        work.emplace_back(code_size, static_cast<int>(nodes.size()) - 1);
    }

    while (work.size() > 1) {
        int max_depth = 0;
        for (auto& [d, id] : work) {
            max_depth = std::max(max_depth, d);
        }
        std::vector<std::pair<int, int>> next;
        bool merged_any = false;
        std::size_t i = 0;
        while (i < work.size()) {
            if (i + 1 < work.size() && work[i].first == max_depth &&
                work[i + 1].first == max_depth) {
                Node parent;
                parent.left = work[i].second;
                parent.right = work[i + 1].second;
                nodes.push_back(parent);
                next.emplace_back(max_depth - 1, static_cast<int>(nodes.size()) - 1);
                i += 2;
                merged_any = true;
                continue;
            }
            next.push_back(work[i]);
            ++i;
        }
        work.swap(next);
        if (!merged_any) {
            throw std::runtime_error("DSC: malformed Huffman code lengths");
        }
    }
    if (work[0].first != 0) {
        throw std::runtime_error("DSC: tree did not reduce to depth 0");
    }
    const int root = work[0].second;

    Bytes out(dec_size, 0);
    std::size_t out_pos = 0;
    BitReader br(data, p);

    while (out_pos < dec_size) {
        int node = root;
        while (!nodes[node].leaf) {
            node = br.read_bit() ? nodes[node].right : nodes[node].left;
        }
        int sym = nodes[node].symbol;
        if (sym < kFirstMatchSymbol) {
            out[out_pos++] = static_cast<std::uint8_t>(sym);
            continue;
        }
        std::uint32_t length =
            static_cast<std::uint32_t>(sym - kFirstMatchSymbol) +
            static_cast<std::uint32_t>(kMinEncodedMatchLen);
        std::uint32_t offset =
            br.read_bits(kMatchOffsetBits) + static_cast<std::uint32_t>(kMinMatchOffset);
        if (offset > out_pos) {
            throw std::runtime_error("DSC: LZ offset before buffer start");
        }
        std::size_t src = out_pos - offset;
        for (std::uint32_t k = 0; k < length && out_pos < dec_size; ++k) {
            out[out_pos++] = out[src++];
        }
    }
    return out;
}

Bytes dsc_compress(const Bytes& raw, std::uint32_t key) {
    if (raw.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("DSC: input exceeds 32-bit size limit");
    }

    const std::size_t n = raw.size();
    std::vector<DscToken> tokens;
    tokens.reserve(n);
    std::array<std::uint64_t, kHuffmanAlphabetSize> freq{};

    std::vector<int> head(kLzHashBuckets, -1);
    std::vector<int> prev_chain(kLzChainRing, -1);

    auto hash3 = [&](std::size_t pos) noexcept -> std::size_t {
        const std::uint32_t v = (static_cast<std::uint32_t>(raw[pos]) << 16) |
                                (static_cast<std::uint32_t>(raw[pos + 1]) << 8) |
                                static_cast<std::uint32_t>(raw[pos + 2]);
        return static_cast<std::size_t>((v * kKnuthHashMul) >> kLzHashShift) & kLzHashMask;
    };

    auto insert_pos = [&](std::size_t pos) noexcept {
        if (pos + kMinCompressMatchLen > n) {
            return;
        }
        const std::size_t h = hash3(pos);
        prev_chain[pos & kLzChainMask] = head[h];
        head[h] = static_cast<int>(pos);
    };

    std::size_t pos = 0;
    while (pos < n) {
        std::size_t best_len = 0;
        std::size_t best_offset = 0;

        if (pos >= kMinMatchOffset && pos + kMinCompressMatchLen <= n) {
            const std::size_t max_len = std::min(kMaxMatchLen, n - pos);
            const int min_cand =
                (pos > kMaxMatchOffset) ? static_cast<int>(pos - kMaxMatchOffset) : 0;
            int cand = head[hash3(pos)];
            int steps = 0;
            while (cand >= min_cand && steps < kMaxChainSteps) {
                const auto cand_idx = static_cast<std::size_t>(cand);
                const std::size_t dist = pos - cand_idx;
                if (dist < kMinMatchOffset) {
                    cand = prev_chain[cand_idx & kLzChainMask];
                    continue;
                }
                ++steps;
                if (raw[cand_idx + best_len] == raw[pos + best_len] && raw[cand_idx] == raw[pos]) {
                    std::size_t len = 0;
                    while (len < max_len && raw[cand_idx + len] == raw[pos + len]) {
                        ++len;
                    }
                    if (len >= kMinCompressMatchLen && len > best_len) {
                        best_len = len;
                        best_offset = dist;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
                cand = prev_chain[cand_idx & kLzChainMask];
            }
        }

        if (best_len >= kMinCompressMatchLen) {
            const auto sym =
                static_cast<std::uint16_t>(kFirstMatchSymbol + (best_len - kMinEncodedMatchLen));
            const auto enc_off = static_cast<std::uint16_t>(best_offset - kMinMatchOffset);
            tokens.push_back(DscToken{sym, enc_off});
            ++freq[sym];
            for (std::size_t k = 0; k < best_len; ++k) {
                insert_pos(pos + k);
            }
            pos += best_len;
            continue;
        }

        const auto sym = static_cast<std::uint16_t>(raw[pos]);
        tokens.push_back(DscToken{sym, 0});
        ++freq[sym];
        insert_pos(pos);
        ++pos;
    }

    const std::array<std::uint8_t, kHuffmanAlphabetSize> code_lengths =
        BuildKraftCompleteCodeLengths(freq);

    std::vector<std::pair<int, int>> leaves;
    leaves.reserve(kHuffmanAlphabetSize);
    for (int sym = 0; sym < kHuffmanAlphabetSize; ++sym) {
        const std::uint8_t len = code_lengths[static_cast<std::size_t>(sym)];
        if (len != 0u) {
            leaves.emplace_back(static_cast<int>(len), sym);
        }
    }
    std::sort(leaves.begin(), leaves.end());

    std::array<std::uint32_t, kHuffmanAlphabetSize> codes{};
    std::uint32_t code = 0;
    int prev_len = leaves.front().first;
    for (const auto& [len, sym] : leaves) {
        code <<= (len - prev_len);
        codes[static_cast<std::size_t>(sym)] = code;
        ++code;
        prev_len = len;
    }

    Bytes out;
    out.reserve(kDscHeaderBytes + static_cast<std::size_t>(kHuffmanAlphabetSize) + tokens.size());
    out.insert(out.end(), DSC_MAGIC, DSC_MAGIC + kDscMagicBytes);
    wr32(out, key);
    wr32(out, static_cast<std::uint32_t>(raw.size()));
    wr32(out, static_cast<std::uint32_t>(tokens.size()));
    wr32(out, 0u);

    std::uint32_t state = key;
    for (int i = 0; i < kHuffmanAlphabetSize; ++i) {
        const std::uint32_t adv = hash_update(state);
        const std::uint8_t len = code_lengths[static_cast<std::size_t>(i)];
        out.push_back(static_cast<std::uint8_t>((len + adv) & kByteMask));
    }

    BitWriter bw(out);
    for (const DscToken& tok : tokens) {
        bw.put_bits(codes[tok.symbol], code_lengths[tok.symbol]);
        if (tok.symbol >= kFirstMatchSymbol) {
            bw.put_bits(tok.encoded_offset, kMatchOffsetBits);
        }
    }
    bw.flush();
    return out;
}

std::vector<ArcEntry> list_arc(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open " + path);
    }
    char head[kArcHeaderBytes];
    f.read(head, static_cast<std::streamsize>(kArcHeaderBytes));
    if (!f || std::memcmp(head, ARC_MAGIC, kArcMagicBytes) != 0) {
        throw std::runtime_error(path + ": not a BURIKO ARC20 archive");
    }
    std::uint32_t cnt = 0;
    std::memcpy(&cnt, head + kArcMagicBytes, sizeof(cnt));
    Bytes idx(static_cast<std::size_t>(cnt) * kArcRecordBytes);
    f.read(reinterpret_cast<char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
    if (!f) {
        throw std::runtime_error(path + ": truncated ARC20 index");
    }

    std::vector<ArcEntry> entries;
    entries.reserve(cnt);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::uint8_t* b = idx.data() + static_cast<std::size_t>(i) * kArcRecordBytes;
        std::size_t name_len = 0;
        while (name_len < kArcNameFieldBytes && b[name_len] != 0) {
            ++name_len;
        }
        ArcEntry e;
        e.name = cp932_to_utf8_replace(b, name_len);
        std::memcpy(&e.offset, b + kArcOffsetFieldPos, sizeof(e.offset));
        std::memcpy(&e.size, b + kArcSizeFieldPos, sizeof(e.size));
        entries.push_back(std::move(e));
    }
    return entries;
}

namespace {
Bytes read_raw(const std::string& path, const std::vector<ArcEntry>& entries,
               std::size_t idx) {
    const auto& e = entries.at(idx);
    std::size_t data_start = kArcHeaderBytes + entries.size() * kArcRecordBytes;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open " + path);
    }
    f.seekg(static_cast<std::streamoff>(data_start + e.offset));
    Bytes raw(e.size);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!f) {
        throw std::runtime_error(path + ": truncated entry payload");
    }
    return raw;
}
}  // namespace

Bytes read_entry(const std::string& path, std::size_t idx, bool decompress) {
    auto entries = list_arc(path);
    Bytes raw = read_raw(path, entries, idx);
    if (decompress && is_dsc(raw)) {
        return dsc_decompress(raw);
    }
    return raw;
}

Bytes read_entry_by_name(const std::string& path, const std::string& name,
                         bool decompress) {
    auto entries = list_arc(path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            Bytes raw = read_raw(path, entries, i);
            if (decompress && is_dsc(raw)) {
                return dsc_decompress(raw);
            }
            return raw;
        }
    }
    throw std::out_of_range(name + " not in " + path);
}

std::size_t extract_all(const std::string& arc_path, const std::string& out_dir,
                        bool decompress) {
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));
    auto entries = list_arc(arc_path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Bytes raw = read_raw(arc_path, entries, i);
        if (decompress && is_dsc(raw)) {
            raw = dsc_decompress(raw);
        }
        std::string safe = entries[i].name;
        for (auto& c : safe) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }
        write_file(out_dir + "/" + safe, raw);
    }
    return entries.size();
}

void repack_arc(const std::string& in_path, const std::string& out_path,
                const std::map<std::string, Bytes>& replacements) {
    Bytes whole = read_file(in_path);
    if (whole.size() < kArcHeaderBytes ||
        std::memcmp(whole.data(), ARC_MAGIC, kArcMagicBytes) != 0) {
        throw std::runtime_error(in_path + ": not a BURIKO ARC20 archive");
    }
    std::uint32_t cnt = rd32(whole, kArcMagicBytes);
    std::size_t data_start = kArcHeaderBytes + static_cast<std::size_t>(cnt) * kArcRecordBytes;

    Bytes out_idx;
    Bytes out_data;
    out_idx.reserve(static_cast<std::size_t>(cnt) * kArcRecordBytes);
    std::uint32_t cursor = 0;

    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::uint8_t* b =
            whole.data() + kArcHeaderBytes + static_cast<std::size_t>(i) * kArcRecordBytes;
        std::size_t name_len = 0;
        while (name_len < kArcNameFieldBytes && b[name_len] != 0) {
            ++name_len;
        }
        std::string name = cp932_to_utf8_replace(b, name_len);
        std::uint32_t off = 0;
        std::uint32_t sz = 0;
        std::memcpy(&off, b + kArcOffsetFieldPos, sizeof(off));
        std::memcpy(&sz, b + kArcSizeFieldPos, sizeof(sz));

        const Bytes* payload = nullptr;
        Bytes original;
        auto it = replacements.find(name);
        if (it != replacements.end()) {
            payload = &it->second;
        } else {
            original.assign(whole.begin() + static_cast<std::ptrdiff_t>(data_start + off),
                            whole.begin() + static_cast<std::ptrdiff_t>(data_start + off + sz));
            payload = &original;
        }

        out_idx.insert(out_idx.end(), b, b + kArcNameFieldBytes);
        wr32(out_idx, cursor);
        wr32(out_idx, static_cast<std::uint32_t>(payload->size()));
        out_idx.insert(out_idx.end(), b + kArcExtraFieldPos, b + kArcRecordBytes);
        out_data.insert(out_data.end(), payload->begin(), payload->end());
        cursor += static_cast<std::uint32_t>(payload->size());
    }

    Bytes out;
    out.reserve(kArcHeaderBytes + out_idx.size() + out_data.size());
    out.insert(out.end(), ARC_MAGIC, ARC_MAGIC + kArcMagicBytes);
    wr32(out, cnt);
    out.insert(out.end(), out_idx.begin(), out_idx.end());
    out.insert(out.end(), out_data.begin(), out_data.end());
    write_file(out_path, out);
}

}  // namespace snu::bgi
