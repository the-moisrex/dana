#include "tse_similarity.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dana::tools::tse {

    namespace {

        using u32 = std::u32string;

        [[nodiscard]] u32 decode(std::string_view text) {
            u32 out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size();) {
                auto const  c     = static_cast<unsigned char>(text[i]);
                char32_t    value = 0;
                std::size_t extra = 0;
                if (c < 0x80) {
                    value = c;
                } else if ((c & 0xE0U) == 0xC0U) {
                    value = static_cast<char32_t>(c & 0x1FU);
                    extra = 1;
                } else if ((c & 0xF0U) == 0xE0U) {
                    value = static_cast<char32_t>(c & 0x0FU);
                    extra = 2;
                } else if ((c & 0xF8U) == 0xF0U) {
                    value = static_cast<char32_t>(c & 0x07U);
                    extra = 3;
                } else {
                    out.push_back(static_cast<char32_t>(c));
                    ++i;
                    continue;
                }
                if (i + extra >= text.size()) {
                    out.push_back(static_cast<char32_t>(c));
                    ++i;
                    continue;
                }
                for (std::size_t j = 1; j <= extra; ++j) {
                    value = (value << 6U) | static_cast<char32_t>(static_cast<unsigned char>(text[i + j]) & 0x3FU);
                }
                out.push_back(value);
                i += extra + 1;
            }
            return out;
        }

        [[nodiscard]] bool is_space(char32_t c) noexcept {
            return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == U'\v' || c == 0x00A0 ||
                   c == 0x2028 || c == 0x2029 || c == 0x200B;
        }

        [[nodiscard]] std::size_t lcs_length(u32 const& a, u32 const& b) noexcept {
            if (a.empty() || b.empty()) {
                return 0;
            }
            if (a.size() < b.size()) {
                return lcs_length(b, a);
            }
            std::vector<std::size_t> prev(b.size() + 1, 0);
            std::vector<std::size_t> curr(b.size() + 1, 0);
            for (std::size_t i = 1; i <= a.size(); ++i) {
                for (std::size_t j = 1; j <= b.size(); ++j) {
                    if (a[i - 1] == b[j - 1]) {
                        curr[j] = prev[j - 1] + 1;
                    } else {
                        curr[j] = std::max(prev[j], curr[j - 1]);
                    }
                }
                std::swap(prev, curr);
                std::fill(curr.begin(), curr.end(), 0);
            }
            return prev[b.size()];
        }

        [[nodiscard]] double ratio_u32(u32 const& a, u32 const& b) noexcept {
            std::size_t const lensum = a.size() + b.size();
            if (lensum == 0) {
                return 100.0;
            }
            std::size_t const lcs = lcs_length(a, b);
            return 200.0 * static_cast<double>(lcs) / static_cast<double>(lensum);
        }

        [[nodiscard]] std::vector<u32> split_tokens(u32 const& text) {
            std::vector<u32> tokens;
            u32              current;
            for (char32_t const c : text) {
                if (is_space(c)) {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(c);
                }
            }
            if (!current.empty()) {
                tokens.push_back(current);
            }
            return tokens;
        }

        [[nodiscard]] u32 join_tokens(std::vector<u32> const& tokens) {
            u32 out;
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                if (i != 0) {
                    out.push_back(U' ');
                }
                out += tokens[i];
            }
            return out;
        }

        [[nodiscard]] u32 sorted_token_text(u32 const& text) {
            auto tokens = split_tokens(text);
            std::sort(tokens.begin(), tokens.end());
            return join_tokens(tokens);
        }

        // Mirrors rapidfuzz's `_partial_ratio_impl(shorter, longer)`: only the
        // longer string's prefixes, full-length windows and suffixes are tried.
        [[nodiscard]] double partial_impl(u32 const& shorter, u32 const& longer) noexcept {
            std::size_t const len1     = shorter.size();
            std::size_t const len2     = longer.size();
            double            best     = 0.0;
            auto const        consider = [&](u32 const& window) {
                double const score = ratio_u32(shorter, window);
                if (score > best) {
                    best = score;
                }
            };
            for (std::size_t i = 1; i < len1; ++i) {
                consider(longer.substr(0, i));
                if (best >= 100.0) {
                    return 100.0;
                }
            }
            for (std::size_t i = 0; i + len1 <= len2 && i < len2 - len1; ++i) {
                consider(longer.substr(i, len1));
                if (best >= 100.0) {
                    return 100.0;
                }
            }
            for (std::size_t i = len2 - len1; i < len2; ++i) {
                consider(longer.substr(i));
                if (best >= 100.0) {
                    return 100.0;
                }
            }
            return best;
        }

        [[nodiscard]] double partial_ratio_u32(u32 const& a, u32 const& b) noexcept {
            if (a.empty() || b.empty()) {
                return 0.0;
            }
            bool const same_length = a.size() == b.size();
            u32 const& shorter     = a.size() <= b.size() ? a : b;
            u32 const& longer      = a.size() <= b.size() ? b : a;
            double     best        = partial_impl(shorter, longer);
            if (best != 100.0 && same_length) {
                best = std::max(best, partial_impl(longer, shorter));
            }
            return best;
        }

        [[nodiscard]] double token_sort_ratio(u32 const& a, u32 const& b) noexcept {
            return ratio_u32(sorted_token_text(a), sorted_token_text(b));
        }

        [[nodiscard]] double token_set_ratio(u32 const& a, u32 const& b) noexcept {
            auto const tokens_a = split_tokens(a);
            auto const tokens_b = split_tokens(b);
            if (tokens_a.empty() || tokens_b.empty()) {
                return 0.0;
            }
            auto contains = [](std::vector<u32> const& list, u32 const& value) {
                return std::find(list.begin(), list.end(), value) != list.end();
            };
            std::vector<u32> intersect;
            std::vector<u32> diff_ab;
            std::vector<u32> diff_ba;
            for (auto const& token : tokens_a) {
                if (contains(tokens_b, token)) {
                    if (!contains(intersect, token)) {
                        intersect.push_back(token);
                    }
                } else if (!contains(diff_ab, token)) {
                    diff_ab.push_back(token);
                }
            }
            for (auto const& token : tokens_b) {
                if (!contains(tokens_a, token) && !contains(diff_ba, token)) {
                    diff_ba.push_back(token);
                }
            }
            if (!intersect.empty() && (diff_ab.empty() || diff_ba.empty())) {
                return 100.0;
            }
            std::sort(diff_ab.begin(), diff_ab.end());
            std::sort(diff_ba.begin(), diff_ba.end());
            std::sort(intersect.begin(), intersect.end());
            u32 const         diff_ab_joined = join_tokens(diff_ab);
            u32 const         diff_ba_joined = join_tokens(diff_ba);
            std::size_t const ab_len         = diff_ab_joined.size();
            std::size_t const ba_len         = diff_ba_joined.size();
            std::size_t const sect_len       = join_tokens(intersect).size();
            std::size_t const sect_ab_len    = sect_len + (sect_len != 0 ? 1 : 0) + ab_len;
            std::size_t const sect_ba_len    = sect_len + (sect_len != 0 ? 1 : 0) + ba_len;

            auto const norm = [](std::size_t dist, std::size_t lensum) -> double {
                return lensum != 0 ? 100.0 - 100.0 * static_cast<double>(dist) / static_cast<double>(lensum) : 100.0;
            };

            std::size_t const dist   = ab_len + ba_len - 2 * lcs_length(diff_ab_joined, diff_ba_joined);
            double            result = norm(dist, sect_ab_len + sect_ba_len);
            if (sect_len == 0) {
                return result;
            }
            std::size_t const sect_ab_dist  = (sect_len != 0 ? 1 : 0) + ab_len;
            double const      sect_ab_ratio = norm(sect_ab_dist, sect_len + sect_ab_len);
            std::size_t const sect_ba_dist  = (sect_len != 0 ? 1 : 0) + ba_len;
            double const      sect_ba_ratio = norm(sect_ba_dist, sect_len + sect_ba_len);
            return std::max({result, sect_ab_ratio, sect_ba_ratio});
        }

        [[nodiscard]] double token_ratio(u32 const& a, u32 const& b) noexcept {
            return std::max(token_set_ratio(a, b), token_sort_ratio(a, b));
        }

        [[nodiscard]] double partial_token_ratio(u32 const& a, u32 const& b) noexcept {
            auto const tokens_a = split_tokens(a);
            auto const tokens_b = split_tokens(b);
            if (tokens_a.empty() || tokens_b.empty()) {
                return 0.0;
            }
            auto contains = [](std::vector<u32> const& list, u32 const& value) {
                return std::find(list.begin(), list.end(), value) != list.end();
            };
            for (auto const& token : tokens_a) {
                if (contains(tokens_b, token)) {
                    return 100.0;
                }
            }
            std::vector<u32> diff_ab;
            std::vector<u32> diff_ba;
            for (auto const& token : tokens_a) {
                if (!contains(tokens_b, token) && !contains(diff_ab, token)) {
                    diff_ab.push_back(token);
                }
            }
            for (auto const& token : tokens_b) {
                if (!contains(tokens_a, token) && !contains(diff_ba, token)) {
                    diff_ba.push_back(token);
                }
            }
            auto sorted_a = tokens_a;
            auto sorted_b = tokens_b;
            std::sort(sorted_a.begin(), sorted_a.end());
            std::sort(sorted_b.begin(), sorted_b.end());
            double result = partial_ratio_u32(join_tokens(sorted_a), join_tokens(sorted_b));
            if (tokens_a.size() == diff_ab.size() && tokens_b.size() == diff_ba.size()) {
                return result;
            }
            std::sort(diff_ab.begin(), diff_ab.end());
            std::sort(diff_ba.begin(), diff_ba.end());
            return std::max(result, partial_ratio_u32(join_tokens(diff_ab), join_tokens(diff_ba)));
        }

    } // namespace

    double ratio(std::string_view a, std::string_view b) noexcept {
        return ratio_u32(decode(a), decode(b));
    }

    double partial_ratio(std::string_view a, std::string_view b) noexcept {
        return partial_ratio_u32(decode(a), decode(b));
    }

    double wratio(std::string_view a, std::string_view b) noexcept {
        auto const s1 = decode(a);
        auto const s2 = decode(b);
        if (s1.empty() || s2.empty()) {
            return 0.0;
        }
        constexpr double unbase_scale = 0.95;
        double const     len_ratio =
          s1.size() > s2.size() ? static_cast<double>(s1.size()) / static_cast<double>(s2.size())
                                : static_cast<double>(s2.size()) / static_cast<double>(s1.size());
        double end_ratio = ratio_u32(s1, s2);
        if (len_ratio < 1.5) {
            return std::max(end_ratio, token_ratio(s1, s2) * unbase_scale);
        }
        double const partial_scale = len_ratio <= 8.0 ? 0.9 : 0.6;
        end_ratio                  = std::max(end_ratio, partial_ratio_u32(s1, s2) * partial_scale);
        return std::max(end_ratio, partial_token_ratio(s1, s2) * unbase_scale * partial_scale);
    }

} // namespace dana::tools::tse
