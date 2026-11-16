#include "alias.hpp"

#include "../core/sanitize.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace prompt::alias {

    namespace {

        bool is_path_char(char c) {
            auto const byte = static_cast<unsigned char>(c);
            return std::isalnum(byte) != 0 || c == '_' || c == '-' || c == '.' || c == '/' || c == '~';
        }

        fs::path normalize(fs::path const& path) {
            std::error_code ec;
            auto            canonical = fs::weakly_canonical(path, ec);
            if (!ec) {
                return canonical;
            }
            return path.lexically_normal();
        }

        // `path` is strictly below `base` (not equal, not `..`).
        bool is_under(fs::path const& path, fs::path const& base) {
            auto const rel = path.lexically_relative(base);
            if (rel.empty() || rel == "." || rel.is_absolute()) {
                return false;
            }
            return *rel.begin() != "..";
        }

        std::uint64_t fnv1a(std::string_view text) {
            std::uint64_t hash = 1'469'598'103'934'665'603ULL;
            for (char const c : text) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1'099'511'628'211ULL;
            }
            return hash;
        }

        std::string hex12(std::uint64_t value) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string                out(12, '0');
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[out.size() - 1 - i]   = digits[static_cast<std::size_t>(value & 0xFULL)];
                value                   >>= 4U;
            }
            return out;
        }

        void append_field(std::string& out, std::string_view value) {
            out += std::to_string(value.size());
            out += '\n';
            out += value;
        }

        std::optional<std::string_view> read_field(std::string_view text, std::size_t& pos) {
            auto const end = text.find('\n', pos);
            if (end == std::string_view::npos) {
                return std::nullopt;
            }

            std::size_t length   = 0;
            auto const [ptr, ec] = std::from_chars(text.data() + pos, text.data() + end, length);
            if (ec != std::errc{} || ptr != text.data() + end) {
                return std::nullopt;
            }

            pos = end + 1;
            if (text.size() - pos < length) {
                return std::nullopt;
            }
            auto const value  = text.substr(pos, length);
            pos              += length;
            return value;
        }

    } // namespace

    void table::add(fs::path const& real, std::string_view shown) {
        auto const canonical = normalize(real);
        auto const real_text = canonical.generic_string();
        if (real_text.empty() || shown.empty()) {
            return;
        }

        auto const known = std::any_of(entries_.begin(), entries_.end(), [&](entry const& existing) {
            return existing.real == real_text;
        });
        if (!known) {
            entries_.push_back(entry{.real = real_text, .shown = std::string(shown)});
        }

        auto const add_form = [&](std::string form) {
            if (form.empty()) {
                return;
            }
            auto const duplicate = std::any_of(forms_.begin(), forms_.end(), [&](hide_form const& existing) {
                return existing.form == form && existing.shown == shown;
            });
            if (duplicate) {
                return;
            }
            bool const bare = !form.contains('/');
            forms_.push_back(hide_form{.form = std::move(form), .shown = std::string(shown), .bare = bare});
        };

        add_form(real_text);
        auto const given = real.generic_string();
        if (given != real_text) {
            add_form(given);
        }
    }

    void table::add_hide_only(std::string_view form, std::string_view shown) {
        if (form.empty() || shown.empty()) {
            return;
        }
        auto const duplicate = std::any_of(forms_.begin(), forms_.end(), [&](hide_form const& existing) {
            return existing.form == form && existing.shown == shown;
        });
        if (duplicate) {
            return;
        }
        forms_.push_back(
          hide_form{.form = std::string(form), .shown = std::string(shown), .bare = !form.contains('/')});
    }

    void table::add_path(fs::path const& path) {
        add(path, placeholder_for(path).generic_string());
    }

    std::string table::hide(std::string_view text) const {
        if (forms_.empty() || text.empty()) {
            return std::string(text);
        }

        std::vector<hide_form const*> order;
        order.reserve(forms_.size());
        for (auto const& form : forms_) {
            order.push_back(&form);
        }
        std::sort(order.begin(), order.end(), [](hide_form const* left, hide_form const* right) {
            return left->form.size() > right->form.size();
        });

        std::string out;
        out.reserve(text.size());
        std::size_t pos = 0;
        while (pos < text.size()) {
            hide_form const* match = nullptr;
            for (auto const* form : order) {
                if (form->form.size() > text.size() - pos) {
                    continue;
                }
                if (text.compare(pos, form->form.size(), form->form) != 0) {
                    continue;
                }
                if (pos > 0 && is_path_char(text[pos - 1])) {
                    continue;
                }

                std::size_t const after = pos + form->form.size();
                if (after < text.size()) {
                    char const next = text[after];
                    if (form->bare ? next != '/' : (next != '/' && is_path_char(next))) {
                        continue;
                    }
                }
                match = form;
                break;
            }

            if (match != nullptr) {
                out += match->shown;
                pos += match->form.size();
            } else {
                out += text[pos];
                ++pos;
            }
        }
        return out;
    }

    std::string table::reveal(std::string_view text) const {
        if (entries_.empty() || text.empty()) {
            return std::string(text);
        }

        std::vector<entry const*> order;
        order.reserve(entries_.size());
        for (auto const& item : entries_) {
            order.push_back(&item);
        }
        std::sort(order.begin(), order.end(), [](entry const* left, entry const* right) {
            return left->shown.size() > right->shown.size();
        });

        std::string out;
        out.reserve(text.size());
        std::size_t pos = 0;
        while (pos < text.size()) {
            entry const* match = nullptr;
            for (auto const* item : order) {
                if (item->shown.size() > text.size() - pos) {
                    continue;
                }
                if (text.compare(pos, item->shown.size(), item->shown) != 0) {
                    continue;
                }
                match = item;
                break;
            }

            if (match != nullptr) {
                out += match->real;
                pos += match->shown.size();
            } else {
                out += text[pos];
                ++pos;
            }
        }
        return out;
    }

    std::string table::show_path(fs::path const& path) const {
        auto const absolute = normalize(path);

        entry const* best = nullptr;
        fs::path     best_rel;
        for (auto const& candidate : entries_) {
            fs::path const root(candidate.real);
            if (absolute != root && !is_under(absolute, root)) {
                continue;
            }
            if (best == nullptr || candidate.real.size() > best->real.size()) {
                best     = &candidate;
                best_rel = absolute.lexically_relative(root);
            }
        }

        if (best == nullptr) {
            return absolute.generic_string();
        }
        if (best_rel.empty() || best_rel == ".") {
            return best->shown;
        }
        return (fs::path(best->shown) / best_rel).generic_string();
    }

    std::string table::describe() const {
        std::string out;
        for (auto const& item : entries_) {
            out += "prompt: workspace " + item.shown + " -> " + item.real + "\n";
        }
        return out;
    }

    fs::path placeholder_for(fs::path const& root) {
        auto const canonical = normalize(root).generic_string();
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            std::string seed = canonical;
            if (attempt != 0) {
                seed += '#';
                seed += std::to_string(attempt);
            }
            auto const      candidate = fs::path("/prompt-ws-" + hex12(fnv1a(seed)));
            std::error_code ec;
            if (!fs::exists(candidate, ec)) {
                return candidate;
            }
        }
        return {"/prompt-ws-" + hex12(fnv1a(canonical))};
    }

    std::string workspace_header(fs::path const& cwd, fs::path const& git_root, table const& aliases) {
        std::string out;
        auto const  shown_cwd  = cwd.empty() ? std::string{} : aliases.show_path(cwd);
        auto const  shown_root = git_root.empty() ? std::string{} : aliases.show_path(git_root);

        if (!cwd.empty() && !git_root.empty() && normalize(cwd) == normalize(git_root)) {
            out += "Working directory (git root): " + shown_cwd + "\n";
            return out;
        }
        if (!cwd.empty()) {
            out += "Working directory: " + shown_cwd + "\n";
        }
        if (!git_root.empty()) {
            out += "Git root: " + shown_root + "\n";
        }
        return out;
    }

    table for_environment(environment const& env) {
        table aliases;

        fs::path cwd = env.working_dir;
        if (cwd.empty()) {
            std::error_code ec;
            cwd = fs::current_path(ec);
            if (ec) {
                cwd.clear();
            }
        }

        auto const root = env.git_root.empty() ? fs::path{} : normalize(env.git_root);
        if (!root.empty() && root != root.root_path()) {
            aliases.add(root, placeholder_for(root).generic_string());
        }

        if (!cwd.empty() && cwd != cwd.root_path()) {
            auto const absolute_cwd = normalize(cwd);
            if (root.empty() || (absolute_cwd != root && !is_under(absolute_cwd, root))) {
                aliases.add(absolute_cwd, placeholder_for(absolute_cwd).generic_string());
            }
        }

        // The sanitizer rewrites `$HOME/Projects/x` to `x`, so already-redacted text
        // carries a spelling that is not the real path; register it too.
        for (auto const& item : aliases.entries()) {
            auto const clean = sanitize::clean_privacy(item.real);
            if (clean != item.real && !clean.empty()) {
                aliases.add_hide_only(clean, aliases.show_path(item.real));
            }
        }
        return aliases;
    }

    std::string encode(table const& aliases) {
        std::string out;
        out += std::to_string(aliases.entries().size());
        out += '\n';
        for (auto const& item : aliases.entries()) {
            append_field(out, item.real);
            append_field(out, item.shown);
        }
        return out;
    }

    std::optional<table> decode(std::string_view text) {
        auto const end = text.find('\n');
        if (end == std::string_view::npos) {
            return std::nullopt;
        }

        std::size_t count    = 0;
        auto const [ptr, ec] = std::from_chars(text.data() + 0, text.data() + end, count);
        if (ec != std::errc{} || ptr != text.data() + end) {
            return std::nullopt;
        }

        std::size_t pos = end + 1;
        table       aliases;
        for (std::size_t i = 0; i < count; ++i) {
            auto const real = read_field(text, pos);
            if (!real) {
                return std::nullopt;
            }
            auto const shown = read_field(text, pos);
            if (!shown) {
                return std::nullopt;
            }
            aliases.add(fs::path(std::string(*real)), *shown);
        }
        return aliases;
    }

} // namespace prompt::alias
