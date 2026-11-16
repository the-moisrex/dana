#include "../src/sdk/alias.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

using namespace prompt;

namespace {

    int failures = 0;

    void check(bool cond, char const* what) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    void check_eq(std::string_view got, std::string_view want, char const* what) {
        if (got != want) {
            std::fprintf(
              stderr,
              "FAIL: %s\n  got:  [%.*s]\n  want: [%.*s]\n",
              what,
              static_cast<int>(got.size()),
              got.data(),
              static_cast<int>(want.size()),
              want.data());
            ++failures;
        }
    }

    void test_boundaries() {
        alias::table table;
        table.add("/a/b", "/prompt-ws-test");

        check_eq(table.hide("see /a/b/c here"), "see /prompt-ws-test/c here", "hides a path prefix at a component");
        check_eq(table.hide("see /a/b here"), "see /prompt-ws-test here", "hides the exact path");
        check_eq(table.hide("see /a/bc here"), "see /a/bc here", "does not hide a longer name");
        check_eq(table.hide("see /a/b.txt here"), "see /a/b.txt here", "does not hide a suffixed name");
        check_eq(table.hide("see x/a/b here"), "see x/a/b here", "does not hide inside another path");
        check_eq(table.hide("see /a/b:12:3"), "see /prompt-ws-test:12:3", "hides before a line suffix");
    }

    void test_round_trip() {
        alias::table table;
        table.add("/a/b", "/prompt-ws-test");

        auto const hidden = table.hide("open /a/b/src/main.cpp and /a/b/README.md");
        check_eq(hidden, "open /prompt-ws-test/src/main.cpp and /prompt-ws-test/README.md", "hide two paths");
        check_eq(table.reveal(hidden), "open /a/b/src/main.cpp and /a/b/README.md", "reveal restores them");
    }

    void test_bare_form() {
        alias::table table;
        table.add_hide_only("proj", "/prompt-ws-test");

        check_eq(table.hide("proj/src/a.cpp"), "/prompt-ws-test/src/a.cpp", "hides a bare form before a slash");
        check_eq(table.hide("the proj is nice"), "the proj is nice", "leaves prose alone");
    }

    void test_show_path() {
        alias::table table;
        table.add("/a/b", "/prompt-ws-test");

        check_eq(table.show_path("/a/b"), "/prompt-ws-test", "shows the root");
        check_eq(table.show_path("/a/b/src/a.cpp"), "/prompt-ws-test/src/a.cpp", "shows a child");
        check_eq(table.show_path("/other/a.cpp"), "/other/a.cpp", "leaves an unrelated path alone");
    }

    void test_placeholder() {
        auto const first = alias::placeholder_for("/a/b");
        auto const again = alias::placeholder_for("/a/b");
        auto const other = alias::placeholder_for("/a/c");

        check(first == again, "the placeholder is stable");
        check(first != other, "different roots get different placeholders");
        check(first.string().starts_with("/prompt-ws-"), "the placeholder has the documented prefix");
        check(first.string().size() == std::string("/prompt-ws-").size() + 12, "the placeholder carries 12 hex digits");
        check(!std::filesystem::exists(first), "the placeholder does not exist");
    }

    void test_header() {
        alias::table table;
        table.add("/a/b", "/prompt-ws-test");

        check_eq(alias::workspace_header("/a/b", "/a/b", table),
                 "Working directory (git root): /prompt-ws-test\n",
                 "collapses equal roots");
        check_eq(alias::workspace_header("/a/b/src", "/a/b", table),
                 "Working directory: /prompt-ws-test/src\nGit root: /prompt-ws-test\n",
                 "shows a nested working directory");

        alias::table const real;
        check_eq(alias::workspace_header("/a/b", "/a/b", real),
                 "Working directory (git root): /a/b\n",
                 "an empty table keeps the real path");
    }

    void test_codec() {
        alias::table table;
        table.add("/a/b", "/prompt-ws-one");
        table.add("/c/d", "/prompt-ws-two");

        auto const encoded = alias::encode(table);
        auto const decoded = alias::decode(encoded);
        check(decoded.has_value(), "the mapping decodes");
        if (decoded) {
            check_eq(decoded->hide("/a/b/src/x.cpp"), "/prompt-ws-one/src/x.cpp", "the decoded mapping hides");
            check_eq(decoded->reveal("/prompt-ws-two/y"), "/c/d/y", "the decoded mapping reveals");
        }
        check(!alias::decode("garbage").has_value(), "garbage is rejected");
        check(!alias::decode("2\n").has_value(), "a truncated mapping is rejected");
    }

    void test_environment() {
        char const* home = std::getenv("HOME");
        if (home == nullptr) {
            return;
        }

        std::filesystem::path const root = std::filesystem::path(home) / "Projects" / "alias-test-project";
        environment                 env;
        env.git_root    = root;
        env.working_dir = root;

        auto const table = alias::for_environment(env);
        check(!table.empty(), "the environment maps the git root");

        auto const placeholder = table.show_path(root);
        check(placeholder.starts_with("/prompt-ws-"), "the environment shows a placeholder");
        check_eq(table.hide(root.string() + "/src/a.cpp"), placeholder + "/src/a.cpp", "the real root is hidden");
        check_eq(table.hide("alias-test-project/src/a.cpp"),
                 placeholder + "/src/a.cpp",
                 "the sanitized spelling is hidden too");
    }

} // namespace

int main() {
    test_boundaries();
    test_round_trip();
    test_bare_form();
    test_show_path();
    test_placeholder();
    test_header();
    test_codec();
    test_environment();

    if (failures != 0) {
        std::fprintf(stderr, "%d alias test(s) failed\n", failures);
        return 1;
    }
    std::printf("alias tests passed\n");
    return 0;
}
