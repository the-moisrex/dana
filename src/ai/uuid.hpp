#pragma once

// A random RFC 4122 version-4 UUID, used by the web providers that create a
// chat (or a message) before the server assigns an id.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace prompt::ai {

inline std::string uuid4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFU), static_cast<unsigned>((hi >> 16) & 0xFFFFU),
                  static_cast<unsigned>(hi & 0xFFFFU), static_cast<unsigned>((lo >> 48) & 0xFFFFU),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(buffer);
}

} // namespace prompt::ai
