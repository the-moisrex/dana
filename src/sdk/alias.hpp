#pragma once

// Path aliasing: replace real absolute paths with a stable, non-existent
// placeholder directory before an AI sees them, and translate the placeholder
// back when it names a file or a command.
//
// The placeholder is a hash of the normalized canonical path, so the same
// project always gets the same fake root (a resumed `.ai.reply` chat keeps
// working) and two projects never collide. The mapping is root + relative
// structure: `/prompt-ws-<hash>/src/a.cpp` is the shown form of
// `/home/me/proj/src/a.cpp`.
//
// The table is reusable: `.ai.loop` builds one for its workspace, `.paths.hide`
// publishes it as `keys::path_alias` for every later `.ai.*` sink, and
// `.paths.reveal` translates the context back.

#include "stage.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dana::alias {

    namespace fs = std::filesystem;

    // One real root and the placeholder that stands in for it.
    struct entry {
        std::string real;
        std::string shown;
    };

    class table {
      public:
        // Map `real` (canonicalized) to `shown`, registering the path as it was
        // given too when the two spellings differ.
        void add(fs::path const& real, std::string_view shown);

        // Register a spelling that only hides: the sanitized form of a root, for
        // example, which appears in already-redacted text.
        void add_hide_only(std::string_view form, std::string_view shown);

        // Map a path to its own stable placeholder.
        void add_path(fs::path const& path);

        [[nodiscard]] bool empty() const {
            return entries_.empty();
        }

        // Real path -> placeholder, longest form first, at path-component
        // boundaries only (`/a/b` matches `/a/b` and `/a/b/c`, never `/a/bc`).
        [[nodiscard]] std::string hide(std::string_view text) const;

        // Placeholder -> real path.
        [[nodiscard]] std::string reveal(std::string_view text) const;

        // The placeholder form of a path under a mapped root.
        [[nodiscard]] std::string show_path(fs::path const& path) const;

        // One `prompt: workspace <shown> -> <real>` line per entry, for a
        // `--print-prompt` dry run.
        [[nodiscard]] std::string describe() const;

        [[nodiscard]] std::vector<entry> const& entries() const {
            return entries_;
        }

      private:
        struct hide_form {
            std::string form;
            std::string shown;
            // A form without a `/` (a bare basename) only matches when a path
            // component follows, so prose is never rewritten.
            bool bare = false;
        };

        std::vector<entry>     entries_;
        std::vector<hide_form> forms_;
    };

    // The stable, non-existent absolute directory standing in for `root`:
    // `/prompt-ws-<12 hex of the canonical path>`. Re-hashed when that directory
    // happens to exist.
    [[nodiscard]] fs::path placeholder_for(fs::path const& root);

    // The "Working directory"/"Git root" header, collapsed to one line when the two
    // are the same directory.
    [[nodiscard]] std::string workspace_header(fs::path const& cwd, fs::path const& git_root, table const& aliases);

    // The standard mapping: the git root and/or the working directory (one
    // placeholder when one contains the other), plus the sanitized spelling of each
    // root as a hide-only form.
    [[nodiscard]] table for_environment(environment const& env);

    // `keys::path_alias` codec: a length-prefixed list of (real, shown) pairs, so
    // any path bytes survive the fact store.
    [[nodiscard]] std::string          encode(table const& aliases);
    [[nodiscard]] std::optional<table> decode(std::string_view text);

} // namespace dana::alias
