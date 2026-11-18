#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana {
    // The keys the dispatcher and the built-in prompts agree on. A key is a plain
    // string, so a prompt family can publish its own without touching the SDK; the
    // constants below exist only to keep the names spelled once.
    enum struct [[nodiscard]] keys : std::uint8_t {
        // The basename stem a later `.save` should derive its filename from.
        save_stem,

        // The response shape the AI was asked for, as a plan: newline-separated
        // `[name=]selector` entries (`command:prompt`, `save.stem=field:filename`).
        // Published by `.expect` and `.delegate`, read by `.extract` (all entries) and
        // by `.each`/`.exec` (the unnamed primary entry only) when they are given no
        // selector of their own. See `sdk/agent.hpp`'s `parse_expect_plan`.
        expect,

        // Declared by `.save` (when it has no explicit target) so `.delegate` knows to
        // ask the AI for a file name and publish the `save.stem` binding for it. The
        // value is a marker ("1"); presence is what matters.
        save_wants_name,

        // What the chain is saving, so `.save` can pick a destination other than the
        // current directory. `.skill.gen` publishes `skill`, which makes a nameless
        // `.save` write `$XDG_DATA_HOME/agents/skills/<stem>/SKILL.md` (the layout the
        // `skill` prompt loads). The value is a short kind name; empty means a plain
        // file in the working directory.
        save_kind,

        // The resolved TSE instrument, published once per chain by whichever `tse*`
        // prompt resolved it first.
        tse_symbol,

        // The provider an AI sink just talked to (`gap`, `dseek`, …), so a later
        // `.ai.reply` in the same chain continues that chat without consulting the
        // on-disk terminal marker. `debate` when the sink was `.ai.debate`.
        ai_provider,

        // The plain answer the previous AI sink produced in this chain. `.ai.reply`
        // drops it from the context it sends: the continued chat already contains it,
        // so only the newly appended instruction should be re-sent.
        ai_answer,

        // The chain's intent, published as an artifact by `.fix`/`.debug`/`.run` (and
        // overridable with `.compress --for`). Read by `.compress` so the same input
        // compresses differently when it is being repaired rather than merely read.
        // Values are `general`, `fix`, `debug`, `review` (`dana::signals::intent`).
        mode,

        // What the chain is *about*: newline-separated focal items — failed Google Test
        // cases, `path:line` error sites, or the files under repair. `.compress` never
        // compresses a focal region and its `gtest.units` compressor keeps the focal
        // test bodies while sampling the rest.
        focus,

        // The files whose content this chain has already emitted, as newline-separated
        // canonical absolute paths. A file prompt reads it through `sdk/file_scope.hpp`,
        // skips what is already there and publishes the union, so a chain like
        // `.read.grep X .files.grep X .files X .read X` embeds each file once.
        files,

        // The chain-wide file selection filter: newline-separated exclusion patterns
        // (paths, basenames, globs, directories) published by `.files.exclude`, and the
        // absolute search root published by `.files.scope`. Every file prompt reads
        // them through `sdk/file_filter.hpp`.
        excluded_files,
        file_root,

        // The path-alias mapping `.paths.hide` publishes for the rest of the chain: a
        // length-prefixed list of (real root, placeholder) pairs (`sdk/alias.hpp`'s
        // `encode`/`decode`). Every `.ai.*` sink reads it to hide the context it sends
        // and reveal the answer it gets back.
        path_alias,

        // The detected project shape, published by `.project` and re-derived on demand
        // by `.compress` through `dana::project::detect`. `build_dir` is a path, the
        // rest are `dana::project` enum names.
        project_language,
        project_build,
        project_build_dir,
        project_package,
        project_tests,
    };

    [[nodiscard]] std::string_view to_string(keys key) noexcept;

    // The only inter-prompt channel. Ordered key/value pairs of strings; a value
    // may contain anything (the wire writes it length-prefixed).
    //
    // Two merge rules, both one line:
    //   * declarations (cheap, argv/env-derived, published before a batch runs)
    //     keep the FIRST writer — order-independent;
    //   * artifacts (published by a finished segment) keep the LAST writer — the
    //     nearest successful segment, which is exactly what `.save` means.
    struct [[nodiscard]] facts {
        struct [[nodiscard]] entry {
            keys        key;
            std::string value;
        };

        void set(keys key, std::string_view value);
        void set(std::string_view key, std::string_view value);

        [[nodiscard]] std::optional<std::string_view> get(keys key) const noexcept;
        [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const noexcept;

        // Drop a key (used when a child prompt must not inherit a chain-wide
        // record, e.g. the file dedup scope of an in-process tool call).
        void erase(keys key);

        [[nodiscard]] bool empty() const {
            return m_entries.empty();
        }

        [[nodiscard]] std::size_t size() const {
            return m_entries.size();
        }

        // The ordered pairs, for a serializer (the stash store) and for `--facts`
        // displays. The vector is the store's own order: insertion order with a
        // re-set key staying in place.
        [[nodiscard]] std::vector<entry> const& entries() const {
            return m_entries;
        }

        void clear() {
            m_entries.clear();
        }

        // Merge `other` in. overwrite=false keeps an existing value (declarations,
        // first writer wins); overwrite=true replaces it (artifacts, last wins).
        void merge(facts const& other, bool overwrite);

        // Wire codec: count, then length-prefixed key/value pairs. Values may
        // contain anything, including NUL, so the list encoding survives a fork.
        [[nodiscard]] std::string encode() const;
        static facts              decode(std::string_view blob);

      private:
        std::vector<entry> m_entries;
    };

} // namespace dana
