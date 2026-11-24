#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dana::tools {

    enum struct [[nodiscard]] yt_method : unsigned char {
        file,       // filename only
        meta,       // ffprobe metadata only
        both,       // filename first, then metadata
        meta_first, // metadata first, then filename
    };

    struct [[nodiscard]] yt_link_options {
        yt_method method  = yt_method::both;
        bool      check   = false;
        int       timeout = 5;
        bool      verbose = false;
    };

    struct [[nodiscard]] yt_link_result {
        std::string url;            // empty when nothing was found
        bool        failed = false; // a file yielded no URL (the script's exit 1)
    };

    // A YouTube video id is 11 characters of [A-Za-z0-9_-] and contains a letter.
    [[nodiscard]] bool is_valid_youtube_id(std::string_view id) noexcept;

    // The id from a `?v=`/`&v=` query or a `youtu.be/<id>` path, if any.
    [[nodiscard]] std::optional<std::string> youtube_id_from_url(std::string_view url) noexcept;

    // `youtu.be/<id>` for a recognizable YouTube URL, otherwise the URL unchanged.
    [[nodiscard]] std::string clean_youtube_url(std::string_view url);

    // Resolve a URL, a file name, or ffprobe metadata into a clean video link.
    [[nodiscard]] yt_link_result extract_link(std::string_view input, yt_link_options const& options = {});

} // namespace dana::tools
