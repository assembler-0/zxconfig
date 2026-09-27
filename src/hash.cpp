// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/hash.hpp>

#include <cstdint>

namespace zxconfig {
namespace {

/// @brief sha256, one block at a time.  No heap beyond the caller's buffer.
class Sha256 {
public:
    void update(const unsigned char* data, std::size_t len) noexcept {
        for (std::size_t i = 0; i < len; ++i) {
            block_[filled_++] = data[i];
            if (filled_ == 64) {
                transform();
                filled_ = 0;
            }
        }
        bits_ += static_cast<std::uint64_t>(len) * 8;
    }

    void update(std::string_view text) noexcept {
        update(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    }

    std::string hex() noexcept {
        const auto total = bits_;
        update(reinterpret_cast<const unsigned char*>("\x80"), 1);
        unsigned char zero = 0;
        while (filled_ != 56) update(&zero, 1);

        for (int shift = 56; shift >= 0; shift -= 8) {
            const auto byte = static_cast<unsigned char>((total >> shift) & 0xFF);
            update(&byte, 1);
        }

        char out[65];
        for (int i = 0; i < 8; ++i) {
            const auto word = state_[i];
            for (int b = 0; b < 4; ++b) {
                write_byte(out + (i * 8) + (b * 2),
                           static_cast<unsigned char>((word >> (24 - b * 8)) & 0xFF));
            }
        }
        out[64] = '\0';
        return std::string{out};
    }

private:
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    static constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
        return (x >> n) | (x << (32 - n));
    }

    void transform() noexcept {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{block_[i * 4]} << 24) | (std::uint32_t{block_[i * 4 + 1]} << 16)
                 | (std::uint32_t{block_[i * 4 + 2]} << 8) | std::uint32_t{block_[i * 4 + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    static void write_byte(char* out, unsigned char byte) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        out[0] = digits[byte >> 4];
        out[1] = digits[byte & 0xF];
    }

    std::uint32_t state_[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    unsigned char block_[64]{};
    std::size_t filled_{0};
    std::uint64_t bits_{0};
};

}  // namespace

std::string sha256_hex(std::string_view bytes) {
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.hex();
}

std::string digest_parts(std::span<const std::string_view> parts) {
    Sha256 hasher;
    for (const auto part : parts) {
        hasher.update(part);
        hasher.update(std::string_view{"\0", 1});
    }
    return hasher.hex();
}

}  // namespace zxconfig
