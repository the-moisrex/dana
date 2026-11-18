#include "facts.hpp"

#include "codec.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dana {
    namespace {

        // The wire name of every key, in the same order as `keys`. Adding a key to the
        // enum without adding its spelling here leaves an empty name, which `decode`
        // rejects instead of silently producing a wrong key.
        constexpr std::array<std::pair<keys, std::string_view>, 19> key_names{
          {
           {keys::save_stem, "save.stem"},
           {keys::expect, "prompt.expect"},
           {keys::save_wants_name, "save.want_name"},
           {keys::save_kind, "save.kind"},
           {keys::tse_symbol, "tse.instrument.symbol"},
           {keys::ai_provider, "ai.provider"},
           {keys::ai_answer, "ai.answer"},
           {keys::mode, "prompt.mode"},
           {keys::focus, "prompt.focus"},
           {keys::files, "files.emitted"},
           {keys::excluded_files, "files.excluded"},
           {keys::file_root, "files.root"},
           {keys::path_alias, "paths.alias"},
           {keys::project_language, "project.language"},
           {keys::project_build, "project.build"},
           {keys::project_build_dir, "project.build_dir"},
           {keys::project_package, "project.package"},
           {keys::project_tests, "project.tests"},
           }
        };

        [[nodiscard]] std::string_view name_of(keys const key) {
            for (auto const& [k, name] : key_names) {
                if (k == key) {
                    return name;
                }
            }
            return {};
        }

        [[nodiscard]] std::optional<keys> key_from_name(std::string_view const name) {
            for (auto const& [k, n] : key_names) {
                if (n == name) {
                    return k;
                }
            }
            return std::nullopt;
        }

    } // namespace

    [[nodiscard]] std::string_view to_string(keys const key) noexcept {
        for (auto const& [k, name] : key_names) {
            if (k == key) {
                return name;
            }
        }
        return {};
    }

    void facts::set(keys const key, std::string_view const value) {
        auto const it = std::ranges::find_if(m_entries, [key](entry const& e) {
            return e.key == key;
        });
        if (it == m_entries.end()) {
            m_entries.push_back(entry{.key = key, .value = std::string(value)});
        } else {
            it->value.assign(value);
        }
    }

    void facts::set(std::string_view const key_str, std::string_view const value) {
        auto const key = key_from_name(key_str);
        if (!key) [[unlikely]] {
            throw std::invalid_argument("invalid key");
        }
        set(*key, value);
    }

    std::optional<std::string_view> facts::get(keys const key) const noexcept {
        auto const it = std::ranges::find_if(m_entries, [key](entry const& e) {
            return e.key == key;
        });
        if (it == m_entries.end()) {
            return std::nullopt;
        }
        return it->value;
    }

    std::optional<std::string_view> facts::get(std::string_view const key_str) const noexcept {
        auto const key = key_from_name(key_str);
        if (!key) [[unlikely]] {
            return std::nullopt;
        }
        return get(*key);
    }

    void facts::erase(keys key) {
        std::erase_if(m_entries, [key](entry const& e) {
            return e.key == key;
        });
    }

    void facts::merge(facts const& other, bool overwrite) {
        for (auto const& item : other.m_entries) {
            if (overwrite) {
                set(item.key, item.value);
                continue;
            }
            if (!get(item.key)) {
                set(item.key, item.value);
            }
        }
    }

    std::string facts::encode() const {
        std::string out;
        out.reserve(m_entries.size() * 32);
        codec::append_u32(out, m_entries.size());
        for (auto const& item : m_entries) {
            std::string_view const key = name_of(item.key);
            codec::append_u32(out, key.size());
            out += key;
            codec::append_u32(out, item.value.size());
            out += item.value;
        }
        return out;
    }

    facts facts::decode(std::string_view blob) {
        facts         out;
        std::size_t   offset = 0;
        std::uint32_t count  = 0;
        if (!codec::read_u32(blob, offset, count)) {
            return out;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key_size = 0;
            if (!codec::read_u32(blob, offset, key_size) || blob.size() - offset < key_size) {
                return out;
            }
            std::string_view const key_name  = blob.substr(offset, key_size);
            offset                          += key_size;

            std::uint32_t value_size = 0;
            if (!codec::read_u32(blob, offset, value_size) || blob.size() - offset < value_size) {
                return out;
            }
            std::string_view const value  = blob.substr(offset, value_size);
            offset                       += value_size;

            // An unknown name means the blob came from a newer build: stop rather than
            // invent a key. Everything read so far is kept.
            auto const key = key_from_name(key_name);
            if (!key) {
                return out;
            }
            out.set(*key, value);
        }
        return out;
    }

} // namespace dana
