#include "promptfile.hpp"

#include "diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace dana::ai {

    namespace {

        constexpr std::array<std::string_view, 17> kTooLargeMarkers{
          "413",
          "request entity too large",
          "payload too large",
          "request body too large",
          "body too large",
          "too large",
          "too long",
          "token_limit",
          "context window",
          "context length",
          "maximum length",
          "length limit",
          "character limit",
          "超过",
          "过长",
          "字数",
          "上下文",
        };

        constexpr std::array<std::string_view, 2> kHandled{
          "200,000-character limit",
          "200000-character limit",
        };

        constexpr std::size_t kBytesPerKiB   = 1024;
        constexpr std::size_t kBytesPerMiB   = kBytesPerKiB * kBytesPerKiB;
        constexpr int         kSizePrecision = 1;

        bool contains_any(std::string_view text, auto const& markers) {
            return std::ranges::any_of(markers, [text](std::string_view marker) {
                return text.contains(marker);
            });
        }

        std::string lower(std::string_view value) {
            std::string out(value);
            for (char& character : out) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character + ('a' - 'A'));
                }
            }
            return out;
        }

        std::string format_size(double const amount, std::string_view const unit) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(kSizePrecision) << amount << ' ' << unit;
            return stream.str();
        }

    } // namespace

    std::size_t prompt_size(std::string_view text) {
        return text.size();
    }

    std::string human_size(std::size_t count) {
        if (count == 1) {
            return "1 byte";
        }
        if (count < kBytesPerKiB) {
            return std::to_string(count) + " bytes";
        }
        if (count < kBytesPerMiB) {
            return format_size(static_cast<double>(count) / static_cast<double>(kBytesPerKiB), "KiB");
        }
        return format_size(static_cast<double>(count) / static_cast<double>(kBytesPerMiB), "MiB");
    }

    std::size_t limit_for(std::size_t provider_limit, std::optional<std::int64_t> override) {
        if (override) {
            return *override < 0 ? 0U : static_cast<std::size_t>(*override);
        }
        return provider_limit;
    }

    bool should_attach(std::string_view            prompt,
                       std::size_t                 inline_limit,
                       std::optional<bool>         mode,
                       std::optional<std::int64_t> override) {
        if (prompt.empty() || (mode && !*mode)) {
            return false;
        }
        if (mode && *mode) {
            return true;
        }
        return prompt_size(prompt) > limit_for(inline_limit, override);
    }

    std::filesystem::path write_prompt_file(std::string_view prompt) {
        std::string       pattern = (std::filesystem::temp_directory_path() / "ai-prompt-XXXXXX.txt").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        int const descriptor = ::mkstemps(buffer.data(), 4);
        if (descriptor < 0) {
            throw std::runtime_error("could not create a temporary prompt file");
        }
        std::string path(buffer.data());
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream) {
                ::close(descriptor);
                std::filesystem::remove(path);
                throw std::runtime_error("could not write the temporary prompt file");
            }
            stream.write(prompt.data(), static_cast<std::streamsize>(prompt.size()));
            if (!prompt.empty() && prompt.back() != '\n') {
                stream.put('\n');
            }
        }
        ::close(descriptor);
        return path;
    }

    void remove_prompt_file(std::filesystem::path const& path) {
        if (path.empty()) {
            return;
        }
        std::error_code removal_error;
        std::filesystem::remove(path, removal_error);
        if (removal_error) {
            note("could not remove " + path.string() + ": " + removal_error.message());
        }
    }

    std::string prompt_file_reference(std::filesystem::path const& path, std::size_t count) {
        std::string const name = path.filename().string();
        return "The complete request is in the attached file `" + name + "` (" + human_size(count) +
               "). Read that file and respond to its contents exactly as if it were this message; this message is "
               "only a pointer to it.";
    }

    bool looks_too_large(std::string_view const message) {
        return contains_any(lower(message), kTooLargeMarkers);
    }

    std::string annotate_error(
      std::string_view const            message,
      std::string_view const            provider,
      std::string_view const            label,
      std::string_view const            prompt,
      bool const                        used_file,
      bool const                        disabled,
      bool const                        has_upload,
      bool const                        auto_attach,
      std::optional<std::int64_t> const limit) {
        (void) provider;
        std::string const lowered = lower(message);
        if (!looks_too_large(lowered)) {
            return std::string(message);
        }
        if (contains_any(lowered, kHandled)) {
            return std::string(message);
        }

        std::size_t const count  = prompt_size(prompt);
        std::size_t const budget = limit_for(kDefaultInlineLimit, limit);
        std::string       out(message);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }
        auto const add = [&out](std::string_view const line) {
            out += '\n';
            out += line;
        };

        if (!has_upload) {
            add("[ai] The prompt is " + human_size(count) + " and was sent inline: " + std::string(label) +
                " has no upload endpoint. Raise the model's context length in the server, or trim the prompt (a "
                "prompt's --head option helps).");
        } else if (used_file) {
            add("[ai] The prompt was already uploaded as a text file (" + human_size(count) +
                "), but the site still refused it. Split it into smaller prompts or trim it with the prompt's --head "
                "option.");
        } else if (disabled) {
            add("[ai] The prompt is " + human_size(count) +
                " and was sent inline because of --no-prompt-file. Drop that flag (or add --prompt-file) to upload it "
                "as a text file.");
        } else if (!auto_attach) {
            add("[ai] The prompt is " + human_size(count) + "; " + std::string(label) +
                " already tried the inline and file-upload paths. The server still refused it. Try a --model with a "
                "larger context window, or split the prompt.");
        } else if (count > budget) {
            add("[ai] The prompt is " + human_size(count) + ", above the automatic " + human_size(budget) +
                " file threshold. Re-run with --prompt-file to upload it as a text file, or trim it.");
        } else {
            add("[ai] The prompt is " + human_size(count) + ", but the site's limit is below the automatic " +
                human_size(budget) + " threshold. Re-run with --prompt-file to upload it as a text file, or trim it.");
        }
        return out;
    }

} // namespace dana::ai
