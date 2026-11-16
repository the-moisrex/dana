#include "keccak.hpp"
#include <cstring>

namespace prompt::ai::crypto {

namespace {

// Keccak-f[1600] round constants; round 0 is deliberately skipped.
constexpr std::array<std::uint64_t, 24> kRoundConstants{
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL, 0x8000000080008000ULL, 0x000000000000808BULL,
    0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL, 0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

// The pi permutation, indexed by the source lane; the rotation amounts are the
// rho offsets for the same lane order.
constexpr std::array<int, 25> kPi{
    0, 6, 12, 18, 24, 3, 9, 10, 16, 22, 1, 7, 13, 19, 20, 4, 5, 11, 17, 23, 2, 8, 14, 15, 21,
};
constexpr std::array<int, 25> kRho{
    0, 44, 43, 21, 14, 28, 20, 3, 45, 61, 1, 6, 25, 8, 18, 27, 36, 10, 15, 56, 62, 55, 39, 41, 2,
};

constexpr std::size_t kRate = 136;
constexpr std::size_t kLanes = kRate / 8;

std::uint64_t rotate_left(std::uint64_t value, int bits) {
    if (bits == 0) return value;
    auto const shift = static_cast<unsigned>(bits);
    return (value << shift) | (value >> (64U - shift));
}

std::uint64_t load_little_endian(std::uint8_t const* bytes) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) value = (value << 8U) | bytes[index];
    return value;
}

void store_little_endian(std::uint8_t* bytes, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

// Keccak-f[1600] over rounds 1..23 (round 0 is what SHA3-256 and this hash
// disagree about).
void permute(std::array<std::uint64_t, 25>& state) {
    for (std::size_t round = 1; round < kRoundConstants.size(); ++round) {
        std::array<std::uint64_t, 5> column{};
        for (std::size_t x = 0; x < 5; ++x)
            column[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
        std::array<std::uint64_t, 5> delta{};
        for (std::size_t x = 0; x < 5; ++x) delta[x] = column[(x + 4) % 5] ^ rotate_left(column[(x + 1) % 5], 1);

        std::array<std::uint64_t, 25> permuted{};
        for (std::size_t lane = 0; lane < 25; ++lane) {
            auto const source = static_cast<std::size_t>(kPi[lane]);
            permuted[lane] = rotate_left(state[source] ^ delta[source % 5], kRho[lane]);
        }

        for (std::size_t base = 0; base < 25; base += 5) {
            for (std::size_t index = 0; index < 5; ++index) {
                state[base + index] =
                    permuted[base + index] ^ (~permuted[base + ((index + 1) % 5)] & permuted[base + ((index + 2) % 5)]);
            }
        }
        state[0] ^= kRoundConstants[round];
    }
}

} // namespace

std::array<std::uint8_t, 32> deepseek_hash_v1(std::string_view data) {
    std::array<std::uint64_t, 25> state{};
    auto const* bytes = reinterpret_cast<std::uint8_t const*>(data.data());
    std::size_t offset = 0;
    while (offset + kRate <= data.size()) {
        for (std::size_t lane = 0; lane < kLanes; ++lane)
            state[lane] ^= load_little_endian(bytes + offset + (lane * 8));
        permute(state);
        offset += kRate;
    }

    std::array<std::uint8_t, kRate> block{};
    std::size_t const remainder = data.size() - offset;
    if (remainder != 0) std::memcpy(block.data(), bytes + offset, remainder);
    block[remainder] = 0x06;
    block[kRate - 1] |= 0x80U;
    for (std::size_t lane = 0; lane < kLanes; ++lane) state[lane] ^= load_little_endian(block.data() + (lane * 8));
    permute(state);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t lane = 0; lane < 4; ++lane) store_little_endian(digest.data() + (lane * 8), state[lane]);
    return digest;
}

} // namespace prompt::ai::crypto
