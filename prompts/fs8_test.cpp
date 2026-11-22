#include "../runtime/args.hpp"
#include "../runtime/embed.hpp"
#include "../runtime/prompt_registry.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace dana {

    namespace {

        void embed_file(std::string&                 out,
                        std::string&                 err,
                        std::filesystem::path const& path,
                        std::string_view             label,
                        std::optional<std::size_t>   head_lines) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
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

    static void execute_fs8_test(view const& v, result& r) {
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
          "You are an expert C++26 developer writing GoogleTest tests for a foresight pipeline mod.\n\nTests live in "
          "tests/ as *_test.cxx files. Each file is a standalone test suite.\nTests are only built in Debug mode. The "
          "CMakeLists.txt auto-discovers all *_test.cxx files.\n\nYour task: generate a complete, correct test file "
          "that follows all project conventions.\n\n";

        out += "## Build/test reference\n\n";
        out +=
          "To build and run tests:\n"
          "\n"
          "    cmake --preset debug-gcc\n"
          "    cmake --build --preset debug-gcc\n"
          "    ctest --test-dir build-debug-gcc/tests -R test-<name>\n"
          "\n"
          "Or run a single test binary directly:\n"
          "\n"
          "    ./build-debug-gcc/tests/test-<name>\n";

        out += "\n## Test patterns\n\n";
        out +=
          "Every test file follows this structure:\n"
          "\n"
          "1. First include (before any import):\n"
          "   #include \"common/tests_common_pch.hpp\"\n"
          "\n"
          "2. Then system headers, then module import:\n"
          "   #include <linux/input-event-codes.h>\n"
          "   import fs8.mods;\n"
          "\n"
          "3. Basic test with emit_all + record (most common pattern):\n"
          "   - Build a pipeline: context | emit_all[{...}] | <mod-under-test> | record\n"
          "   - Get a reference: auto& col = pipeline.mod<basic_record>();\n"
          "   - Run: pipeline();\n"
          "   - Check results: col.without_syn() returns non-SYN events\n"
          "\n"
          "4. The emit_all initializer list uses this format:\n"
          "   {EV_ABS, ABS_X, 1000},  // {type, code, value}\n"
          "   {EV_SYN, SYN_REPORT, 0},\n"
          "\n"
          "5. For time-dependent tests (like debounce):\n"
          "   - Use a timed_sequence custom load_event provider with explicit timestamps\n"
          "   - Define a consteval timed_ev() helper for event construction\n"
          "   - See debounce_test.cxx for the full pattern\n"
          "\n"
          "6. Common assertions:\n"
          "   - ASSERT_EQ / EXPECT_EQ for values\n"
          "   - ASSERT_EQ(events.size(), NU) for event counts\n"
          "   - Check event.type(), event.code(), event.value()\n"
          "\n"
          "7. The record mod:\n"
          "   - record captures all events that pass through\n"
          "   - col.without_syn() returns events excluding SYN_REPORT\n"
          "   - col.size() gives total event count\n"
          "   - col[i] provides indexed access\n";

        out += "\n## Example test file (scale_test.cxx)\n";
        embed_file(out, err, root / "tests/scale_test.cxx", "tests/scale_test.cxx", head_lines);

        out += "\n## Example: time-dependent test (debounce_test.cxx)\n";
        embed_file(out, err, root / "tests/debounce_test.cxx", "tests/debounce_test.cxx", head_lines);

        out += "\n## Mod source being tested (for reference)\n";
        embed_file(out, err, root / "mods/mods.ixx", "mods/mods.ixx", head_lines);

        r.out = std::move(out);
        r.err = std::move(err);
    }

    static void render_help_fs8_test(std::ostream& os) {
        os << R"EOF(Usage: prompt fs8.test [--head N]
       prompt fs8.test | prompt note "Write tests for ..."

Builds a prompt for writing GoogleTest tests for a foresight pipeline mod.
Must be run from inside the foresight git repo (or a subdirectory of it).

The prompt embeds the test infrastructure, patterns, and example test files
so the AI generates tests that follow the project's conventions.

Options:
  --head N   Keep only the first N lines of each embedded context file
)EOF";
    }

    // This file is the prompt: it implements it and registers it at static-init time
    // (no central list — see sdk/prompt_registry.hpp).
    [[maybe_unused]] bool const registered = dana::register_prompt(
      "fs8.test",
      "Builds a prompt for writing GoogleTest tests for a foresight pipeline mod. Must be run from "
      "inside the foresight git repo (or a subdirectory of it).",
      &execute_fs8_test,
      &render_help_fs8_test);

} // namespace dana
