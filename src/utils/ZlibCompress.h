#pragma once
// Minimal DEFLATE (RFC 1951) compressor with zlib framing (RFC 1950).
//
// Purpose: reproduce faster-whisper's compression-ratio hallucination check
// (transcribe.py get_compression_ratio):
//     ratio = len(raw) / len(zlib.compress(raw))
// zlib.compress() defaults to level 6 (dynamic Huffman blocks + lazy LZ77
// matching). Reproducing it bit-exactly would require a full zlib port, so
// this emits a single fixed-Huffman block and uses a 32 KB hash-chain LZ77
// matcher with zlib's level-6 search heuristics (good 8 / nice 128 /
// chain 128). The resulting stream lengths track zlib within a few percent;
// they are only compared against the 2.4 repetition threshold, for which
// this is behaviorally equivalent (normal speech text < 2.4, degenerate
// repetition >> 2.4). No external dependencies.

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace zlib_lite {

class BitWriter {
public:
    // DEFLATE packs bits LSB-first; Huffman codes are written MSB-of-code
    // first, extra bits LSB-first (RFC 1951 3.1.1).
    void append_msb(uint32_t value, int len) {
        for (int i = len - 1; i >= 0; --i) put_bit((value >> i) & 1);
    }
    void append_lsb(uint32_t value, int len) {
        for (int i = 0; i < len; ++i) put_bit((value >> i) & 1);
    }
    std::vector<uint8_t> take() {
        flush_to_byte();
        return std::move(out_);
    }

private:
    void put_bit(int b) {
        bit_buffer_ |= static_cast<uint32_t>(b & 1) << bit_count_++;
        if (bit_count_ == 8) {
            out_.push_back(static_cast<uint8_t>(bit_buffer_));
            bit_buffer_ = 0;
            bit_count_ = 0;
        }
    }
    void flush_to_byte() {
        if (bit_count_ > 0) {
            out_.push_back(static_cast<uint8_t>(bit_buffer_));
            bit_buffer_ = 0;
            bit_count_ = 0;
        }
    }

    std::vector<uint8_t> out_;
    uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;
};

// Fixed Huffman code tables (RFC 1951 3.2.6).
inline void write_literal(BitWriter& bw, int lit) {
    if (lit <= 143) {
        bw.append_msb(0x30 + static_cast<uint32_t>(lit), 8);
    } else if (lit <= 255) {
        bw.append_msb(0x190 + static_cast<uint32_t>(lit - 144), 9);
    } else if (lit <= 279) {
        bw.append_msb(static_cast<uint32_t>(lit - 256), 7);
    } else {
        bw.append_msb(0xC0 + static_cast<uint32_t>(lit - 280), 8);
    }
}

struct TokenTables {
    static constexpr int kLenBase[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
        43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLenExtra[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3,
        3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistBase[] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
        385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
        12289, 16385, 24577};
    static constexpr int kDistExtra[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
        9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
};

inline void write_length_distance(BitWriter& bw, int len, int dist) {
    int li = 28;
    while (TokenTables::kLenBase[li] > len) --li;
    write_literal(bw, 257 + li);
    if (TokenTables::kLenExtra[li] > 0) {
        bw.append_lsb(static_cast<uint32_t>(len - TokenTables::kLenBase[li]),
                      TokenTables::kLenExtra[li]);
    }
    int di = 29;
    while (TokenTables::kDistBase[di] > dist) --di;
    bw.append_msb(static_cast<uint32_t>(di), 5);  // distance codes: 5 fixed bits
    if (TokenTables::kDistExtra[di] > 0) {
        bw.append_lsb(static_cast<uint32_t>(dist - TokenTables::kDistBase[di]),
                      TokenTables::kDistExtra[di]);
    }
}

// Single fixed-Huffman block; 32 KB hash-chain greedy LZ77 with zlib's
// level-6 search heuristics (good_length=8, nice_length=128, max_chain=128).
inline std::vector<uint8_t> deflate_fixed(const uint8_t* data, size_t n) {
    BitWriter bw;
    bw.append_msb(1, 1);  // BFINAL
    bw.append_msb(1, 2);  // BTYPE = 01 (fixed Huffman)

    constexpr size_t kMinMatch = 3;
    constexpr size_t kMaxMatch = 258;
    constexpr size_t kWindowSize = 32768;
    constexpr int kHashBits = 15;
    constexpr int kGood = 8;
    constexpr int kNice = 128;
    constexpr int kMaxChain = 128;

    const size_t hash_mask = (static_cast<size_t>(1) << kHashBits) - 1;
    std::vector<int32_t> head(hash_mask + 1, -1);
    std::vector<int32_t> prev(n, -1);

    auto hash3 = [&](size_t i) -> size_t {
        return ((static_cast<size_t>(data[i]) << 10) ^
                (static_cast<size_t>(data[i + 1]) << 5) ^
                static_cast<size_t>(data[i + 2])) & hash_mask;
    };
    auto insert = [&](size_t i) {
        if (i + kMinMatch > n) return;  // need 3 bytes to hash
        const size_t h = hash3(i);
        prev[i] = head[h];
        head[h] = static_cast<int32_t>(i);
    };

    size_t i = 0;
    int prev_match_len = 0;
    while (i < n) {
        int best_len = 0;
        size_t best_dist = 0;
        if (i + kMinMatch <= n) {
            // zlib reduces the chain depth once a good match was found at the
            // previous position.
            int chain = prev_match_len >= kGood ? (kMaxChain >> 2) : kMaxChain;
            int32_t cand = head[hash3(i)];
            while (cand >= 0 && chain > 0) {
                --chain;
                const size_t dist = i - static_cast<size_t>(cand);
                if (dist == 0 || dist > kWindowSize) break;
                size_t len = 0;
                const size_t max_len = std::min(kMaxMatch, n - i);
                while (len < max_len &&
                       data[static_cast<size_t>(cand) + len] == data[i + len]) {
                    ++len;
                }
                if (static_cast<int>(len) > best_len) {
                    best_len = static_cast<int>(len);
                    best_dist = dist;
                    if (best_len >= kNice) break;
                }
                cand = prev[static_cast<size_t>(cand)];
            }
        }

        if (best_len >= static_cast<int>(kMinMatch)) {
            write_length_distance(bw, best_len, static_cast<int>(best_dist));
            for (size_t j = i; j < i + static_cast<size_t>(best_len); ++j) insert(j);
            i += static_cast<size_t>(best_len);
            prev_match_len = best_len;
        } else {
            write_literal(bw, data[i]);
            insert(i);
            ++i;
            prev_match_len = 0;
        }
    }

    write_literal(bw, 256);  // end of block
    return bw.take();
}

inline uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1;
    uint32_t b = 0;
    size_t i = 0;
    while (i < n) {
        const size_t chunk = std::min<size_t>(n - i, 5552);
        for (size_t k = 0; k < chunk; ++k) {
            a += data[i + k];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += chunk;
    }
    return (b << 16) | a;
}

inline std::vector<uint8_t> zlib_compress(const uint8_t* data, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n / 2 + 16);
    out.push_back(0x78);  // CMF: deflate, 32 KB window
    out.push_back(0x9C);  // FLG: (0x789C % 31 == 0), FLEVEL=2 (default)
    const auto deflated = deflate_fixed(data, n);
    out.insert(out.end(), deflated.begin(), deflated.end());
    const uint32_t adler = adler32(data, n);
    out.push_back(static_cast<uint8_t>((adler >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(adler & 0xFF));
    return out;
}

// faster-whisper transcribe.py:
//   def get_compression_ratio(text):
//       text_bytes = text.encode("utf-8")
//       return len(text_bytes) / len(zlib.compress(text_bytes))
inline double compression_ratio(const std::string& text) {
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    const size_t raw_len = text.size();
    const auto compressed = zlib_compress(data, raw_len);
    if (compressed.empty()) return 0.0;
    return static_cast<double>(raw_len) /
           static_cast<double>(compressed.size());
}

}  // namespace zlib_lite
