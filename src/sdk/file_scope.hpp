#pragma once

#include "facts.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace prompt {

    // The canonical key for a file path: the absolute, symlink-resolved form when
    // the filesystem can answer, otherwise the absolute lexically-normal path. Two
    // spellings of the same file (`src/main.cpp`, `./src/main.cpp`, an absolute
    // path) therefore collapse to one key.
    [[nodiscard]] std::string file_key(std::filesystem::path const& path);

    // The chain-wide record of files whose content has already been emitted. A file
    // prompt builds one from `v.prior`, skips `seen()` files, `mark()`s the ones it
    // emits, and publishes `fact()` — the union, because artifact merging keeps the
    // last writer and a prompt must not drop what an earlier segment recorded.
    //
    // The dispatcher runs a batch containing an `emits_files` prompt sequentially,
    // so a later file prompt sees the earlier one's `keys::files` artifact.
    class file_scope {
      public:
        explicit file_scope(facts const& prior);

        [[nodiscard]] bool        seen(std::filesystem::path const& path) const;
        void                      mark(std::filesystem::path const& path);
        [[nodiscard]] std::string fact() const;

      private:
        std::vector<std::string>        order_;
        std::unordered_set<std::string> set_;
    };

} // namespace prompt
