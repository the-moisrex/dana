// DeepSeek's proof-of-work: the custom hash (SHA3-256 with Keccak round 0
// skipped) against vectors produced by the Python implementation, plus the
// nonce search and the header payload.  Offline: no site is contacted.

#include "ai/pow.hpp"

#include <cstdio>
#include <string>

namespace {

    int failures = 0;

    void check(bool condition, char const* what) {
        if (condition) {
            return;
        }
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }

    void check_hash(std::string const& input, char const* expected, char const* label) {
        std::string const actual = prompt::ai::pow::deepseek_hash_hex(input);
        if (actual == expected) {
            return;
        }
        std::fprintf(stderr, "FAIL: %s\n  expected %s\n  actual   %s\n", label, expected, actual.c_str());
        ++failures;
    }

} // namespace

int main() {
    // bin/ai/pow.py's deepseek_hash_v1 over the same inputs.
    check_hash("", "e594808bc5b7151ac160c6d39a02e0a8e261ed588578403099e3561dc40c26b3", "empty");
    check_hash("abc", "f841106c601ce9be9bc38525e90d4178d47f21dd8eb9f238fc55ffaa4ca94506", "abc");
    check_hash("abc_1700000000_0",
               "262bfabaacdd9c115ae3301ab58a714f3bd5e1f72a7eca314ec223baafdba97d",
               "challenge prefix, nonce 0");
    check_hash("abc_1700000000_12345",
               "66563149ed024f909b0ce50180fe2dea5dcf92b930a36b518bebb06f00be0b33",
               "challenge prefix, nonce 12345");
    // The rate boundary: 135/136/137 bytes exercise the padding and the absorb
    // loop's tail.
    check_hash(std::string(135, 'a'), "1e3eb415bb0b3ecec08a6f71cf7c8d0591b5594752ba52802a2e60357834f998", "135 bytes");
    check_hash(std::string(136, 'a'), "680364b336f77918ed390287a581f96f1371599825acd1e348fa7649fcecbbab", "136 bytes");
    check_hash(std::string(137, 'a'), "dfeb7768d4d48053c083d849570e84cc8120520790f2d09cb56e40038081fab4", "137 bytes");

    // The solver must find the nonce whose digest the challenge carries.
    std::string const challenge = "262bfabaacdd9c115ae3301ab58a714f3bd5e1f72a7eca314ec223baafdba97d";
    check(prompt::ai::pow::solve_deepseek("abc", 1'700'000'000, 5, challenge, 1) == 0, "solve finds nonce 0");

    // A malformed challenge is a hard error, not a silent no-solution loop.
    bool threw = false;
    try {
        prompt::ai::pow::solve_deepseek("abc", 1'700'000'000, 5, "not-hex", 1);
    } catch (prompt::ai::pow::error const&) {
        threw = true;
    }
    check(threw, "malformed challenge throws");

    // The header value is base64 of the compact JSON the site expects.
    std::string const header =
      prompt::ai::pow::deepseek_response("salt", challenge, "sig", 0, "/api/v0/chat/completion");
    check(!header.empty(), "response header is not empty");
    check(header.find('{') == std::string::npos, "response header is base64, not raw JSON");

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai_pow: ok\n");
    return 0;
}
