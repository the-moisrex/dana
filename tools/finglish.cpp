#include "finglish.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dana::tools {

    namespace {

        using cp = char32_t;

        constexpr cp zwnj = U'\u200C';

        [[nodiscard]] std::vector<cp> decode(std::string_view text) {
            std::vector<cp> out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size();) {
                auto const  c     = static_cast<unsigned>(static_cast<unsigned char>(text[i]));
                cp          value = 0;
                std::size_t extra = 0;
                if (c < 0x80) {
                    value = c;
                } else if ((c & 0xE0U) == 0xC0U) {
                    value = c & 0x1FU;
                    extra = 1;
                } else if ((c & 0xF0U) == 0xE0U) {
                    value = c & 0x0FU;
                    extra = 2;
                } else if ((c & 0xF8U) == 0xF0U) {
                    value = c & 0x07U;
                    extra = 3;
                } else {
                    out.push_back(static_cast<cp>(c));
                    ++i;
                    continue;
                }
                if (i + extra >= text.size()) {
                    out.push_back(static_cast<cp>(c));
                    ++i;
                    continue;
                }
                for (std::size_t j = 1; j <= extra; ++j) {
                    value = (value << 6U) | (static_cast<unsigned>(static_cast<unsigned char>(text[i + j])) & 0x3FU);
                }
                out.push_back(value);
                i += extra + 1;
            }
            return out;
        }

        void encode_into(std::string& out, cp value) {
            if (value < 0x80) {
                out.push_back(static_cast<char>(value));
            } else if (value < 0x800) {
                out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
                out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
            } else if (value < 0x1'0000) {
                out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
                out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
            } else {
                out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
                out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
            }
        }

        [[nodiscard]] std::string encode(std::vector<cp> const& text) {
            std::string out;
            out.reserve(text.size() * 2);
            for (cp const value : text) {
                encode_into(out, value);
            }
            return out;
        }

        [[nodiscard]] bool in_set(cp value, std::u32string_view set) noexcept {
            return set.find(value) != std::u32string_view::npos;
        }

        // The delimiter set the Python tool uses for word boundaries.
        [[nodiscard]] bool is_boundary(cp value) noexcept {
            static constexpr std::u32string_view punct = U" \t\n\r،؛؟!.()[]{}\"'";
            return in_set(value, punct);
        }

        [[nodiscard]] bool is_consonant(cp value) noexcept {
            static constexpr std::u32string_view consonants = U"بپتثجچحخدذرزژسشصضطظعغفقکگلمنه";
            return in_set(value, consonants);
        }

        [[nodiscard]] bool is_alef(cp value) noexcept {
            return value == U'ا' || value == U'آ';
        }

        [[nodiscard]] bool is_yeh(cp value) noexcept {
            return value == U'ی' || value == U'ي';
        }

        [[nodiscard]] bool is_alpha(cp value) noexcept {
            if ((value >= U'a' && value <= U'z') || (value >= U'A' && value <= U'Z')) {
                return true;
            }
            return (value >= 0x0600 && value <= 0x06FF) || (value >= 0xFB50 && value <= 0xFDFF) ||
                   (value >= 0xFE70 && value <= 0xFEFF);
        }

        // Arabic -> Persian, expanding the lam-alef ligatures to two letters.
        void normalize_into(std::vector<cp>& out, cp value) {
            switch (value) {
                case U'ك': out.push_back(U'ک'); break;
                case U'ي':
                case U'ى': out.push_back(U'ی'); break;
                case U'ة': out.push_back(U'ه'); break;
                case U'ئ': out.push_back(U'ی'); break;
                case U'ؤ': out.push_back(U'و'); break;
                case U'أ':
                case U'إ': out.push_back(U'ا'); break;
                case 0xFEFB:
                case 0xFEF7:
                case 0xFEF9:
                case 0xFEF5:
                    out.push_back(U'ل');
                    out.push_back(U'ا');
                    break;
                case 0xFB90:
                case 0xFB91: out.push_back(U'ک'); break;
                case 0xFB94:
                case 0xFB93: out.push_back(U'گ'); break;
                case 0xFBFC:
                case 0xFBFD: out.push_back(U'ی'); break;
                default: out.push_back(value); break;
            }
        }

        struct tables {
            std::unordered_map<cp, std::string>          fa_to_finglish;
            std::unordered_map<std::string, std::string> finglish_to_fa;
            std::vector<std::string>                     finglish_keys;
        };

        [[nodiscard]] tables const& get_tables() {
            static tables const instance = [] {
                tables t;

                auto add_fa = [&t](std::u32string_view keys, std::string_view value) {
                    for (cp const key : keys) {
                        t.fa_to_finglish[key] = std::string(value);
                    }
                };

                // Consonants.
                add_fa(U"ب", "b");
                add_fa(U"پ", "p");
                add_fa(U"ت", "t");
                add_fa(U"ث", "s");
                add_fa(U"ج", "j");
                add_fa(U"چ", "ch");
                add_fa(U"ح", "h");
                add_fa(U"خ", "kh");
                add_fa(U"د", "d");
                add_fa(U"ذ", "z");
                add_fa(U"ر", "r");
                add_fa(U"ز", "z");
                add_fa(U"ژ", "zh");
                add_fa(U"س", "s");
                add_fa(U"ش", "sh");
                add_fa(U"ص", "s");
                add_fa(U"ض", "z");
                add_fa(U"ط", "t");
                add_fa(U"ظ", "z");
                add_fa(U"ع", "'");
                add_fa(U"غ", "gh");
                add_fa(U"ف", "f");
                add_fa(U"ق", "gh");
                add_fa(U"ک", "k");
                add_fa(U"گ", "g");
                add_fa(U"ل", "l");
                add_fa(U"م", "m");
                add_fa(U"ن", "n");
                add_fa(U"ه", "h");
                add_fa(U"ی", "y");

                // Vowels and diacritics.
                add_fa(U"آ", "aa");
                add_fa(U"ا", "a");
                add_fa(U"َ", "a");
                add_fa(U"ِ", "e");
                add_fa(U"ُ", "o");
                add_fa(U"ً", "an");
                add_fa(U"ٍ", "en");
                add_fa(U"ٌ", "un");
                add_fa(U"ْ", "");
                add_fa(U"ّ", "");

                // Special forms.
                add_fa(U"ء", "'");
                add_fa(U"ئ", "i");
                add_fa(U"ؤ", "u");
                add_fa(U"أ", "a");
                add_fa(U"إ", "e");
                add_fa(U"ة", "h");
                add_fa(U"ﻻ", "la");
                add_fa(U"ﻷ", "la");
                add_fa(U"ﻹ", "le");
                add_fa(U"ﻵ", "laa");
                t.fa_to_finglish[zwnj] = "-";

                // Digits.
                add_fa(U"۰", "0");
                add_fa(U"۱", "1");
                add_fa(U"۲", "2");
                add_fa(U"۳", "3");
                add_fa(U"۴", "4");
                add_fa(U"۵", "5");
                add_fa(U"۶", "6");
                add_fa(U"۷", "7");
                add_fa(U"۸", "8");
                add_fa(U"۹", "9");

                // Punctuation.
                add_fa(U"،", ",");
                add_fa(U"؛", ";");
                add_fa(U"؟", "?");
                add_fa(U"٪", "%");
                add_fa(U"«", "\"");
                add_fa(U"»", "\"");
                add_fa(U"۔", ".");
                add_fa(U"٬", ",");

                // Finglish -> Persian. Later categories override earlier keys.
                auto add_rev = [&t](std::string_view key, std::string_view value) {
                    t.finglish_to_fa[std::string(key)] = std::string(value);
                };
                add_rev("b", "ب");
                add_rev("p", "پ");
                add_rev("t", "ت");
                add_rev("s", "س");
                add_rev("j", "ج");
                add_rev("c", "چ");
                add_rev("h", "ه");
                add_rev("d", "د");
                add_rev("z", "ز");
                add_rev("r", "ر");
                add_rev("k", "ک");
                add_rev("g", "گ");
                add_rev("l", "ل");
                add_rev("m", "م");
                add_rev("n", "ن");
                add_rev("f", "ف");
                add_rev("q", "ق");
                add_rev("x", "خ");
                add_rev("'", "ع");
                add_rev("`", "ع");

                add_rev("kh", "خ");
                add_rev("gh", "غ");
                add_rev("sh", "ش");
                add_rev("zh", "ژ");
                add_rev("ch", "چ");

                add_rev("aa", "آ");
                add_rev("a", "ا");
                add_rev("e", "ا");
                add_rev("o", "و");
                add_rev("i", "ی");
                add_rev("u", "و");
                add_rev("ee", "ی");
                add_rev("oo", "و");
                add_rev("ou", "و");
                add_rev("ei", "ی");
                add_rev("ai", "ی");
                add_rev("ay", "ای");
                add_rev("ey", "ی");
                add_rev("oy", "وی");

                add_rev("0", "۰");
                add_rev("1", "۱");
                add_rev("2", "۲");
                add_rev("3", "۳");
                add_rev("4", "۴");
                add_rev("5", "۵");
                add_rev("6", "۶");
                add_rev("7", "۷");
                add_rev("8", "۸");
                add_rev("9", "۹");

                add_rev(",", "،");
                add_rev(";", "؛");
                add_rev("?", "؟");
                add_rev("%", "٪");
                add_rev(".", ".");
                add_rev("!", "!");
                add_rev(":", ":");
                add_rev("(", "(");
                add_rev(")", ")");
                add_rev("[", "[");
                add_rev("]", "]");
                add_rev("{", "{");
                add_rev("}", "}");
                add_rev("\"", "«");
                add_rev("'", "»");

                for (auto const& [key, _] : t.finglish_to_fa) {
                    t.finglish_keys.push_back(key);
                }
                std::ranges::stable_sort(t.finglish_keys, [](std::string const& a, std::string const& b) {
                    return a.size() > b.size();
                });
                return t;
            }();
            return instance;
        }

        [[nodiscard]] std::string map_vav(std::vector<cp> const& text, std::size_t pos) {
            if (pos == 0 || is_boundary(text[pos - 1])) {
                return "v";
            }
            auto const prev = text[pos - 1];
            if (is_alef(prev)) {
                return "o";
            }
            if (is_consonant(prev)) {
                if (pos + 1 < text.size() && is_yeh(text[pos + 1])) {
                    return "oy";
                }
                return "o";
            }
            return "v";
        }

        [[nodiscard]] std::string map_yeh(std::vector<cp> const& text, std::size_t pos) {
            if (pos == 0 || is_boundary(text[pos - 1])) {
                return "y";
            }
            if (pos + 1 == text.size() || is_boundary(text[pos + 1])) {
                return "i";
            }
            if (is_alef(text[pos - 1])) {
                return "i";
            }
            if (is_consonant(text[pos - 1])) {
                return "i";
            }
            return "y";
        }

        // Regex `اا+ -> ا`, `آا+ -> آ`, `یی+ -> ی`, `وو+ -> و`, and
        // `(?<=[consonant])-(?=[consonant or alef]) -> ZWNJ`.
        [[nodiscard]] std::string post_process_persian(std::vector<cp> const& text) {
            std::vector<cp> out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                auto const value = text[i];
                if (!out.empty()) {
                    auto const last = out.back();
                    if (value == U'ا' && last == U'ا') {
                        continue;
                    }
                    if (value == U'ا' && last == U'آ') {
                        continue;
                    }
                    if (value == U'ی' && last == U'ی') {
                        continue;
                    }
                    if (value == U'و' && last == U'و') {
                        continue;
                    }
                }
                out.push_back(value);
            }
            std::vector<cp> fixed;
            fixed.reserve(out.size());
            for (std::size_t i = 0; i < out.size(); ++i) {
                auto const value = out[i];
                if (value == U'-' && i > 0 && i + 1 < out.size() && is_consonant(out[i - 1]) &&
                    (is_consonant(out[i + 1]) || is_alef(out[i + 1])))
                {
                    fixed.push_back(zwnj);
                } else {
                    fixed.push_back(value);
                }
            }
            return encode(fixed);
        }

    } // namespace

    finglish_transliterator::finglish_transliterator() = default;

    std::string finglish_transliterator::persian_to_finglish(std::string_view text) const {
        auto const& table = get_tables().fa_to_finglish;

        std::vector<cp> source;
        source.reserve(text.size());
        for (cp const value : decode(text)) {
            normalize_into(source, value);
        }

        std::string result;
        result.reserve(source.size());
        std::size_t i = 0;
        while (i < source.size()) {
            auto const ch = source[i];

            if (ch == zwnj) {
                result += '-';
                ++i;
                continue;
            }

            bool const word_start = i == 0 || is_boundary(source[i - 1]);

            // خو at a word start -> kho (checked before the خوا/خوی forms).
            if (ch == U'خ' && i + 1 < source.size() && source[i + 1] == U'و' && word_start) {
                result += "kho";
                i      += 2;
                continue;
            }
            // خوا -> khwa (only reachable when not at a word start).
            if (ch == U'خ' && i + 2 < source.size() && source[i + 1] == U'و' && source[i + 2] == U'ا' && word_start) {
                result += "khwa";
                i      += 3;
                continue;
            }
            // خوی -> khoy.
            if (ch == U'خ' && i + 2 < source.size() && source[i + 1] == U'و' && is_yeh(source[i + 2]) && word_start) {
                result += "khoy";
                i      += 3;
                continue;
            }

            // Tanvin: اً -> an.
            if (ch == U'ا' && i + 1 < source.size() && source[i + 1] == U'ً') {
                result += "an";
                i      += 2;
                continue;
            }

            if (ch == U'و') {
                result += map_vav(source, i);
                ++i;
                continue;
            }
            if (is_yeh(ch)) {
                result += map_yeh(source, i);
                ++i;
                continue;
            }

            auto const it = table.find(ch);
            if (it != table.end()) {
                result += it->second;
            } else {
                encode_into(result, ch);
            }
            ++i;
        }
        return result;
    }

    std::string finglish_transliterator::finglish_to_persian(std::string_view text) const {
        auto const& tables = get_tables();
        auto        source = decode(text);

        std::vector<cp> lower;
        lower.reserve(source.size());
        for (cp const value : source) {
            lower.push_back(value >= U'A' && value <= U'Z' ? static_cast<cp>(value - U'A' + U'a') : value);
        }

        auto const starts_with = [&lower](std::size_t pos, std::string_view key) {
            if (pos + key.size() > lower.size()) {
                return false;
            }
            for (std::size_t j = 0; j < key.size(); ++j) {
                if (lower[pos + j] != static_cast<cp>(static_cast<unsigned char>(key[j]))) {
                    return false;
                }
            }
            return true;
        };

        auto const append_utf8 = [](std::vector<cp>& out, std::string const& mapped) {
            for (cp const value : decode(mapped)) {
                out.push_back(value);
            }
        };

        std::vector<cp> result;
        result.reserve(source.size());
        std::size_t i = 0;
        while (i < source.size()) {
            bool matched = false;

            // Digraphs first.
            if (i + 1 < lower.size()) {
                auto const two = std::string{static_cast<char>(lower[i]), static_cast<char>(lower[i + 1])};
                if (two == "kh" || two == "gh" || two == "sh" || two == "zh" || two == "ch") {
                    append_utf8(result, tables.finglish_to_fa.at(two));
                    i       += 2;
                    matched  = true;
                }
            }

            // 'aa' -> آ at a word start, ا otherwise.
            if (!matched && i + 1 < lower.size() && lower[i] == U'a' && lower[i + 1] == U'a') {
                bool const initial = i == 0 || !is_alpha(source[i - 1]);
                result.push_back(initial ? U'آ' : U'ا');
                i       += 2;
                matched  = true;
            }

            if (!matched &&
                (lower[i] == U'a' || lower[i] == U'e' || lower[i] == U'o' || lower[i] == U'i' || lower[i] == U'u'))
            {
                cp prev = cp{0};
                if (!result.empty()) {
                    prev = result.back();
                } else if (i > 0) {
                    prev = source[i - 1];
                }
                bool const prev_is_consonant = is_consonant(prev);
                switch (lower[i]) {
                    case U'a': result.push_back(U'ا'); break;
                    case U'e':
                        if (prev_is_consonant) {
                            if (i + 1 < lower.size()) {
                                auto const next = lower[i + 1];
                                if (next != U'a' && next != U'e' && next != U'o' && next != U'i' && next != U'u') {
                                    result.push_back(U'ی');
                                }
                            }
                        } else {
                            result.push_back(U'ا');
                        }
                        break;
                    case U'o': result.push_back(U'و'); break;
                    case U'i': result.push_back(U'ی'); break;
                    case U'u': result.push_back(U'و'); break;
                    default: break;
                }
                ++i;
                matched = true;
            }

            if (!matched) {
                for (auto const& key : tables.finglish_keys) {
                    if (!starts_with(i, key)) {
                        continue;
                    }
                    append_utf8(result, tables.finglish_to_fa.at(key));
                    i       += key.size();
                    matched  = true;
                    break;
                }
            }

            if (!matched) {
                result.push_back(source[i]);
                ++i;
            }
        }
        return post_process_persian(result);
    }

} // namespace dana::tools
