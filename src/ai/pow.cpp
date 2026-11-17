#include "pow.hpp"

#include "crypto/keccak.hpp"
#include "files.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace dana::ai::pow {

    namespace {

        constexpr int       kMaxWorkers        = 16;
        constexpr std::int64_t kSingleThreadLimit = 2048;

        char hex_digit(std::uint8_t value) {
            return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
        }

        std::string to_hex(std::array<std::uint8_t, 32> const& digest) {
            std::string out;
            out.reserve(digest.size() * 2);
            for (auto byte : digest) {
                out.push_back(hex_digit(static_cast<std::uint8_t>(byte >> 4U)));
                out.push_back(hex_digit(static_cast<std::uint8_t>(byte & 0x0FU)));
            }
            return out;
        }

        int nibble(char value) {
            if (value >= '0' && value <= '9') {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f') {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F') {
                return value - 'A' + 10;
            }
            return -1;
        }

        std::array<std::uint8_t, 32> from_hex(std::string_view text) {
            if (text.size() != 64) {
                throw error("malformed DeepSeek challenge: expected 32 bytes of hex");
            }
            std::array<std::uint8_t, 32> out{};
            for (std::size_t index = 0; index < out.size(); ++index) {
                int const high = nibble(text[index * 2]);
                int const low  = nibble(text[(index * 2) + 1]);
                if (high < 0 || low < 0) {
                    throw error("malformed DeepSeek challenge: not hexadecimal");
                }
                out[index] =
                  static_cast<std::uint8_t>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
            }
            return out;
        }

        int auto_workers() {
            if (char const* override = std::getenv("AI_POW_WORKERS"); override != nullptr && *override != '\0') {
                char*      end    = nullptr;
                long const parsed = std::strtol(override, &end, 10);
                if (end != override && *end == '\0' && parsed > 0) {
                    return static_cast<int>(std::min<long>(parsed, kMaxWorkers));
                }
            }
            auto const hardware = std::thread::hardware_concurrency();
            return static_cast<int>(std::max(1U, std::min(hardware == 0 ? 1U : hardware, 16U)));
        }

        // Search `[start, stop)` for the nonce; the first worker to find one stops the
        // rest.  Returns the nonce or -1.
        std::int64_t search_range(
          std::string const&                  prefix,
          std::array<std::uint8_t, 32> const& target,
          std::int64_t                           start,
          std::int64_t                           stop_at,
          std::atomic<std::int64_t>&             found,
          std::atomic<bool>&                  stopped) {
            for (std::int64_t nonce = start; nonce < stop_at; ++nonce) {
                if (stopped.load(std::memory_order_relaxed)) {
                    return -1;
                }
                std::string const candidate = prefix + std::to_string(nonce);
                if (crypto::deepseek_hash_v1(candidate) != target) {
                    continue;
                }
                std::int64_t expected = -1;
                found.compare_exchange_strong(expected, nonce);
                stopped.store(true);
                return nonce;
            }
            return -1;
        }

    } // namespace

    std::string deepseek_hash_hex(std::string_view data) {
        return to_hex(crypto::deepseek_hash_v1(data));
    }

    std::int64_t solve_deepseek(
      std::string const& salt,
      std::int64_t          expire_at,
      std::int64_t          difficulty,
      std::string const& challenge,
      int                workers) {
        std::string const prefix = salt + "_" + std::to_string(expire_at) + "_";
        auto const        target = from_hex(challenge);
        std::int64_t const   limit  = difficulty > 0 ? difficulty : 1;
        int               count  = workers > 0 ? workers : auto_workers();
        if (static_cast<std::int64_t>(count) > limit) {
            count = static_cast<int>(limit);
        }
        if (count < 1) {
            count = std::max(count, 1);
        }

        std::atomic<std::int64_t> found{-1};
        std::atomic<bool>      stopped{false};
        if (count == 1 || limit < kSingleThreadLimit) {
            search_range(prefix, target, 0, limit, found, stopped);
        } else {
            std::int64_t const          size = (limit + count - 1) / count;
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(count));
            for (std::int64_t start = 0; start < limit; start += size) {
                std::int64_t const stop_at = std::min(start + size, limit);
                pool.emplace_back([&prefix, &target, &found, &stopped, start, stop_at] {
                    search_range(prefix, target, start, stop_at, found, stopped);
                });
            }
            for (auto& thread : pool) {
                thread.join();
            }
        }
        std::int64_t const answer = found.load();
        if (answer < 0) {
            throw error("DeepSeekHashV1: no solution within the difficulty range");
        }
        return answer;
    }

    std::string deepseek_response(
      std::string const& salt,
      std::string const& challenge,
      std::string const& signature,
      std::int64_t          answer,
      std::string const& target_path) {
        nlohmann::ordered_json const payload{
          {  "algorithm", "DeepSeekHashV1"},
          {  "challenge",        challenge},
          {       "salt",             salt},
          {     "answer",           answer},
          {  "signature",        signature},
          {"target_path",      target_path},
        };
        return base64_encode(payload.dump());
    }

} // namespace dana::ai::pow
