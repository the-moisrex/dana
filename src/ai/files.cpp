#include "files.hpp"

#include "../core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace dana::ai {

    namespace {

        std::string lower(std::string_view value) {
            std::string out(value);
            std::ranges::transform(out, out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

    } // namespace

    std::string guess_mime(std::string_view name) {
        auto const        dot = name.rfind('.');
        std::string const ext = dot == std::string_view::npos ? std::string{} : lower(name.substr(dot + 1));
        if (ext == "txt" || ext == "md" || ext == "markdown" || ext == "rst") {
            return "text/plain";
        }
        if (ext == "json" || ext == "jsonl") {
            return "application/json";
        }
        if (ext == "html" || ext == "htm") {
            return "text/html";
        }
        if (ext == "css") {
            return "text/css";
        }
        if (ext == "csv") {
            return "text/csv";
        }
        if (ext == "js" || ext == "mjs") {
            return "text/javascript";
        }
        if (ext == "ts") {
            return "text/typescript";
        }
        if (ext == "py") {
            return "text/x-python";
        }
        if (ext == "c") {
            return "text/x-c";
        }
        if (ext == "h" || ext == "hpp" || ext == "hh") {
            return "text/x-c++";
        }
        if (ext == "cpp" || ext == "cc" || ext == "cxx") {
            return "text/x-c++";
        }
        if (ext == "sh" || ext == "bash") {
            return "text/x-shellscript";
        }
        if (ext == "xml") {
            return "application/xml";
        }
        if (ext == "yaml" || ext == "yml") {
            return "application/yaml";
        }
        if (ext == "pdf") {
            return "application/pdf";
        }
        if (ext == "png") {
            return "image/png";
        }
        if (ext == "jpg" || ext == "jpeg") {
            return "image/jpeg";
        }
        if (ext == "gif") {
            return "image/gif";
        }
        if (ext == "webp") {
            return "image/webp";
        }
        if (ext == "svg") {
            return "image/svg+xml";
        }
        if (ext == "mp3") {
            return "audio/mpeg";
        }
        if (ext == "mp4") {
            return "video/mp4";
        }
        if (ext == "zip") {
            return "application/zip";
        }
        if (ext == "gz" || ext == "tgz") {
            return "application/gzip";
        }
        return "application/octet-stream";
    }

    attachment read_attachment(std::string_view path) {
        std::filesystem::path full(path);
        if (full.string().starts_with("~/")) {
            if (char const* home = std::getenv("HOME"); home != nullptr) {
                full = std::filesystem::path(home) / full.string().substr(2);
            }
        }
        std::error_code ec;
        if (!std::filesystem::exists(full, ec)) {
            throw file_error(std::string(path) + ": no such file");
        }
        if (!std::filesystem::is_regular_file(full, ec)) {
            throw file_error(std::string(path) + ": not a regular file");
        }

        std::ifstream stream(full, std::ios::binary);
        if (!stream) {
            throw file_error(std::string(path) + ": cannot read the file");
        }
        std::string data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (data.empty()) {
            throw file_error(std::string(path) + ": the file is empty");
        }
        if (data.size() > kMaxAttachmentSize) {
            throw file_error(
              std::string(path) + ": larger than " + std::to_string(kMaxAttachmentSize / (1024UL * 1024UL)) + " MiB");
        }
        std::string const name = full.filename().string().empty() ? std::string{"file"} : full.filename().string();
        return attachment{.path = full.string(), .name = name, .data = std::move(data), .mime = guess_mime(name)};
    }

    std::vector<attachment> read_attachments(text::string_views paths) {
        std::vector<attachment> out;
        out.reserve(paths.size());
        for (auto const& path : paths) {
            out.push_back(read_attachment(path));
        }
        return out;
    }

    std::string base64_encode(std::string_view data) {
        static constexpr std::string_view kAlphabet =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        std::size_t i = 0;
        while (i + 2 < data.size()) {
            auto const a = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
            auto const b = static_cast<unsigned>(static_cast<unsigned char>(data[i + 1]));
            auto const c = static_cast<unsigned>(static_cast<unsigned char>(data[i + 2]));
            out.push_back(kAlphabet[a >> 2U]);
            out.push_back(kAlphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
            out.push_back(kAlphabet[((b & 0x0FU) << 2U) | (c >> 6U)]);
            out.push_back(kAlphabet[c & 0x3FU]);
            i += 3;
        }
        if (i < data.size()) {
            auto const a = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
            auto const b = i + 1 < data.size() ? static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) : 0U;
            out.push_back(kAlphabet[a >> 2U]);
            out.push_back(kAlphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
            out.push_back(i + 1 < data.size() ? kAlphabet[(b & 0x0FU) << 2U] : '=');
            out.push_back('=');
        }
        return out;
    }

} // namespace dana::ai
