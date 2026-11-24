#pragma once

// Bidirectional Persian <-> finglish transliteration. The resolver uses the
// literal (no implicit vowel) mode, matching the historical tool's defaults.

#include <string>
#include <string_view>

namespace dana::tools {

    class [[nodiscard]] finglish_transliterator {
      public:
        finglish_transliterator();

        [[nodiscard]] std::string persian_to_finglish(std::string_view text) const;

        [[nodiscard]] std::string finglish_to_persian(std::string_view text) const;
    };

} // namespace dana::tools
