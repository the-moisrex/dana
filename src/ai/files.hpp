#pragma once

// Local attachments for --file: read, name and type a path.  Each provider
// uploads the bytes itself (the flow differs per site), so this only turns a
// path into the (name, bytes, mime) triple they all need.

#include "../core/strings.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dana::ai {

    //: Refuse anything larger than this up front (the sites' own caps are looser).
    inline constexpr std::size_t kMaxAttachmentSize = 100UL * 1024UL * 1024UL;

    struct [[nodiscard]] file_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct [[nodiscard]] attachment {
        std::string path;
        std::string name;
        std::string data;
        std::string mime;

        [[nodiscard]] std::size_t size() const {
            return data.size();
        }
    };

    // Read one attachment, throwing file_error when it is unusable.
    attachment read_attachment(std::string_view path);

    // Read every path, preserving order.
    std::vector<attachment> read_attachments(text::string_views paths);

    std::string guess_mime(std::string_view name);

    std::string base64_encode(std::string_view data);

} // namespace dana::ai
