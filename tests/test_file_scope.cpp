#include "../src/sdk/facts.hpp"
#include "../src/sdk/file_scope.hpp"

#include <cstdio>
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

    // A scratch directory with three regular files, so `weakly_canonical` has real
    // paths to resolve (a non-existent path still canonicalises, but the test wants
    // the same answer for every spelling of one file).
    struct scratch {
        std::filesystem::path root;

        scratch() {
            root = std::filesystem::temp_directory_path() / "prompt-file-scope-test";
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root);
            for (auto const* name : {"a.txt", "b.txt", "c.txt"}) {
                std::FILE* f = std::fopen((root / name).c_str(), "w");
                if (f) {
                    std::fputs("x\n", f);
                    std::fclose(f);
                }
            }
        }

        ~scratch() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    void test_file_key(scratch const& s) {
        auto const canonical = file_key(s.root / "a.txt");
        check(canonical.starts_with("/"), "file_key returns an absolute path");
        check_eq(file_key(s.root / "." / "a.txt"), canonical, "a `./` spelling collapses to the same key");
        check_eq(file_key(s.root / "sub" / ".." / "a.txt"), canonical, "a `..` spelling collapses to the same key");
        check(file_key(s.root / "a.txt") != file_key(s.root / "b.txt"), "different files have different keys");
    }

    void test_empty_prior(scratch const& s) {
        file_scope scope{facts{}};
        check(!scope.seen(s.root / "a.txt"), "an empty scope has seen nothing");
        check_eq(scope.fact(), "", "an empty scope publishes an empty fact");

        scope.mark(s.root / "a.txt");
        check(scope.seen(s.root / "a.txt"), "a marked file is seen");
        check(scope.fact().ends_with('\n'), "the fact is newline-terminated");
        check_eq(scope.fact(), file_key(s.root / "a.txt") + "\n", "the fact is the canonical key");
    }

    void test_prior_parsing(scratch const& s) {
        auto const a = file_key(s.root / "a.txt");
        auto const b = file_key(s.root / "b.txt");

        facts prior;
        prior.set(keys::files, a + "\n" + b + "\n");
        file_scope scope{prior};

        check(scope.seen(s.root / "a.txt"), "a prior key is seen");
        check(scope.seen(s.root / "b.txt"), "every prior key is seen");
        check(!scope.seen(s.root / "c.txt"), "an unlisted file is not seen");

        // Marking one already in the prior must not duplicate it, and the union is
        // still published in order.
        scope.mark(s.root / "a.txt");
        scope.mark(s.root / "c.txt");
        check_eq(scope.fact(),
                 a + "\n" + b + "\n" + file_key(s.root / "c.txt") + "\n",
                 "the fact is the ordered union with no duplicates");
    }

    void test_blank_lines(scratch const& s) {
        auto const a = file_key(s.root / "a.txt");
        facts      prior;
        prior.set(keys::files, "\n" + a + "\n\n");
        file_scope scope{prior};
        check(scope.seen(s.root / "a.txt"), "blank lines in the fact are ignored");
        check_eq(scope.fact(), a + "\n", "blank lines are not re-published");
    }

} // namespace

int main() {
    scratch s;
    test_file_key(s);
    test_empty_prior(s);
    test_prior_parsing(s);
    test_blank_lines(s);

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("file_scope: ok");
    return 0;
}
