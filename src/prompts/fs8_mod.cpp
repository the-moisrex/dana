#include "../sdk/args.hpp"
#include "../sdk/embed.hpp"
#include "../sdk/prompt_registry.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace dana {

    namespace {

        // The embed_file shape this prompt uses: warns to stderr and emits nothing for
        // a missing file; otherwise a blank line, "File: <label>", ```lang fence, the
        // head-capped body, and a closing fence. infer_lang keys off the basename.
        void embed_file(std::string&                 out,
                        std::string&                 err,
                        std::filesystem::path const& path,
                        std::string_view             label,
                        std::optional<std::size_t>   head_lines) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) [[unlikely]] {
                err += std::format("Warning: context file not found: {}\n", path.string());
                return;
            }
            std::string const base  = path.filename().string();
            std::string       name  = label.empty() ? base : std::string(label);
            out                    += '\n';
            out                    += std::format("File: {}\n", name);
            out                    += std::format("```{}\n", infer_lang(std::string_view{base}));
            out                    += trim_context_nl(read_file(path), head_lines);
            out                    += "```\n";
        }

        // Port of find_foresight_root: walk from $PWD to / looking for AGENTS.md + mods/.
        std::filesystem::path find_foresight_root() {
            auto dir = std::filesystem::current_path();
            while (dir != dir.root_path() && dir != std::filesystem::path("/")) {
                if (std::filesystem::is_regular_file(dir / "AGENTS.md") && std::filesystem::is_directory(dir / "mods"))
                {
                    return dir;
                }
                auto parent = dir.parent_path();
                if (parent == dir) {
                    break;
                }
                dir = parent;
            }
            return {};
        }

    } // namespace

    static void execute_fs8_mod(view const& v, result& r) {
        auto head_lines = head_lines_or_throw(v);

        auto root = find_foresight_root();
        if (root.empty()) {
            throw soft_error{
              "Error: not inside the foresight git repo (could not find AGENTS.md + mods/).\n"
              "Change to the foresight directory or a subdirectory and try again.\n"};
        }

        std::string out;
        std::string err;

        out +=
          "You are an expert C++26 developer writing a new pipeline mod for the foresight project.\n\nForesight is a "
          "Linux input manager: a C++26 library (libforesight) plus the foresight CLI.\nMods are consteval-copyable "
          "callables in the fs8 namespace that chain via operator| in a pipeline.\n\nYour task: generate a complete, "
          "correct, buildable mod that follows all project conventions.\n\n";

        out += "## Project overview\n";
        embed_file(out, err, root / "AGENTS.md", "foresight/AGENTS.md", head_lines);

        out += "\n## How to write a mod (guide)\n\n";
        out +=
          "### Checklist for a new mod\n"
          "\n"
          "1. Create `mods/<name>.ixx` as `export module fs8.mods:<name>;`\n"
          "   - Define `basic_<name>` in `export namespace fs8`, inheriting `consteval_copyable`\n"
          "     (or `pimpl_idiom<basic_<name>>` if it holds runtime state).\n"
          "   - Expose a constexpr instance `<name>`.\n"
          "2. Create `mods/<name>.cxx` as `module fs8.mods;` with the `impl` definition and method bodies.\n"
          "   - Skip this file if the mod is header-only (no state, all methods inline).\n"
          "3. Register both files in the root `CMakeLists.txt`:\n"
          "   - `.cxx` \xE2\x86\x92 `target_sources(... PRIVATE ...)`\n"
          "   - `.ixx` \xE2\x86\x92 `PUBLIC FILE_SET foresight TYPE CXX_MODULES FILES`\n"
          "4. Add `export import :<name>;` to `mods/mods.ixx`.\n"
          "5. `static_assert` the `Modifier`/`OutputModifier` concept where relevant and\n"
          "   any inter-mod dependency.\n"
          "6. Handle lifecycle tags via `operator()(special_event const&)`:\n"
          "   - Return `next`/`drop_event` for ordinary events.\n"
          "   - All invocations must be `noexcept`.\n"
          "\n"
          "### Mod invocation forms\n"
          "\n"
          "- `mod(v)` \xE2\x80\x94 sees the whole context (event + sibling mods).\n"
          "- `mod(event)` \xE2\x80\x94 only needs the current event.\n"
          "- `mod(v, tag)` \xE2\x80\x94 a tag request (start, load_event, next_event, etc.).\n"
          "\n"
          "### context_action return values\n"
          "\n"
          "| Action       | Meaning                                |\n"
          "|--------------|----------------------------------------|\n"
          "| `next`       | Pass the event to the next mod.        |\n"
          "| `drop_event` | Drop this event.                       |\n"
          "| `recovery`   | Restart / enter watch mode.            |\n"
          "| `exit`       | Exit the pipeline.                     |\n"
          "\n"
          "### Key invariants\n"
          "\n"
          "- Mods derive from `consteval_copyable`: runtime copies abort.\n"
          "- Every mod must be `nothrow`-invocable.\n"
          "- `pimpl_idiom`-based mods allocate lazily at `start`; handlers must outlive the pipeline.\n";

        out += "\n## Example: header-only mod (stopper.ixx)\n";
        embed_file(out, err, root / "mods/stopper.ixx", "mods/stopper.ixx", head_lines);

        out += "\n## Example: mod with state + .cxx (scale.ixx)\n";
        embed_file(out, err, root / "mods/scale.ixx", "mods/scale.ixx", head_lines);

        out += "\n## Example: .cxx implementation (scale.cxx)\n";
        embed_file(out, err, root / "mods/scale.cxx", "mods/scale.cxx", head_lines);

        out += "\n## Example: mod with pimpl (io_manager.ixx header)\n";
        embed_file(out, err, root / "mods/io_manager.ixx", "mods/io_manager.ixx", head_lines);

        out += "\n## Registration: mods/mods.ixx (export imports)\n";
        embed_file(out, err, root / "mods/mods.ixx", "mods/mods.ixx", head_lines);

        r.out = std::move(out);
        r.err = std::move(err);
    }

    static void render_help_fs8_mod(std::ostream& os) {
        os << R"EOF(Usage: prompt fs8.mod [--head N]
       prompt fs8.mod | prompt note "Create a mod that ..."

Builds a prompt for writing a new pipeline mod for the foresight project.
Must be run from inside the foresight git repo (or a subdirectory of it).

The prompt embeds AGENTS.md, a mod-writing guide, and example mod files
so the AI has enough context to generate correct, buildable code.

Options:
  --head N   Keep only the first N lines of each embedded context file
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "fs8.mod",
      "Builds a prompt for writing a new pipeline mod for the foresight project. Must be run from "
      "inside the foresight git repo (or a subdirectory of it).",
      &execute_fs8_mod,
      &render_help_fs8_mod);

} // namespace dana
