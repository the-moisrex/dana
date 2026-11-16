#pragma once

// A random RFC 4122 version-4 UUID, used by the web providers that create a
// chat (or a message) before the server assigns an id.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace dana::ai {

    inline std::string uuid4() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uint64_t                       hi = rng();
        std::uint64_t                       lo = rng();
        hi                                     = (hi & 0xFFFF'FFFF'FFFF'0FFFULL) | 0x0000'0000'0000'4000ULL;
        lo                                     = (lo & 0x3FFF'FFFF'FFFF'FFFFULL) | 0x8000'0000'0000'0000ULL;
        char buffer[37];
        std::snprintf(
          buffer,
          sizeof(buffer),
          "%08x-%04x-%04x-%04x-%012llx",
          static_cast<unsigned>((hi >> 32) & 0xFFFF'FFFFU),
          static_cast<unsigned>((hi >> 16) & 0xFFFFU),
          static_cast<unsigned>(hi & 0xFFFFU),
          static_cast<unsigned>((lo >> 48) & 0xFFFFU),
          static_cast<unsigned long long>(lo & 0xFFFF'FFFF'FFFFULL));
        return std::string(buffer);
    }

} // namespace dana::ai
