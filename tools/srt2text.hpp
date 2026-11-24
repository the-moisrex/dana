#pragma once

#include <string>
#include <string_view>

namespace dana::tools {

    // Turn SRT/WebVTT-ish subtitle text into flowing prose. Timing lines only
    // separate blocks (a gap of more than two seconds starts a new paragraph),
    // index lines and blank lines are dropped, and inline HTML tags are stripped.
    // The result always ends with exactly one newline.
    [[nodiscard]] std::string srt_to_text(std::string_view input);

} // namespace dana::tools
