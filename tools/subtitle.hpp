#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dana::tools {

    struct [[nodiscard]] subtitle_options {
        std::string lang    = "eng";
        std::string format  = "srt";
        bool        verbose = false;
        bool        cache   = false;
    };

    // Download subtitles for a video URL. Tries yt-dlp directly, then with the
    // Firefox and Vivaldi cookie stores, and finally youtube_transcript_api when
    // it is installed. Returns the subtitle text, or nullopt when every source
    // failed.
    [[nodiscard]] std::optional<std::string> fetch_subtitles(std::string_view        url,
                                                             subtitle_options const& options = {});

} // namespace dana::tools
