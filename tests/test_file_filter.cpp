#include "../src/sdk/facts.hpp"
#include "../src/sdk/file_filter.hpp"
#include "../src/sdk/stage.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

using namespace dana;

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

    // A repository-shaped scratch tree: src/, src/sub/, tests/, and a couple of
    // files, so a git-relative path and a scope-relative path differ.
    struct scratch {
        std::filesystem::path root;

        scratch() {
            root = std::filesystem::temp_directory_path() / "prompt-file-filter-test";
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "src" / "sub");
            std::filesystem::create_directories(root / "tests");
            for (auto const& rel : {"src/a.cpp", "src/b.test.cpp", "src/sub/c.cpp", "tests/t.cpp", "README.md"}) {
                std::FILE* f = std::fopen((root / rel).c_str(), "w");
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

    environment env_for(scratch const& s) {
        environment env;
        env.git_root = s.root;
        return env;
    }

    file_filter make(scratch const& s, std::string_view exclusions, std::string_view root = {}) {
        facts prior;
        if (!exclusions.empty()) {
            prior.set(keys::excluded_files, exclusions);
        }
        if (!root.empty()) {
            prior.set(keys::file_root, root);
        }
        return file_filter::from(prior, env_for(s));
    }

    void test_empty(scratch const& s) {
        auto const filter = make(s, "");
        check(filter.empty(), "an empty fact makes an empty filter");
        check(!filter.has_root(), "an empty fact has no scope");
        check(!filter.excluded(s.root / "src/a.cpp"), "nothing is excluded by an empty filter");
        check_eq(filter.exclude_fact({"x"}), "x\n", "exclude_fact adds a pattern");
    }

    void test_basename(scratch const& s) {
        auto const filter = make(s, "*.test.cpp\n");
        check(filter.excluded(s.root / "src/b.test.cpp"), "a basename glob excludes the file");
        check(!filter.excluded(s.root / "src/a.cpp"), "a basename glob leaves other files");
        check(!filter.excluded(s.root / "tests/t.cpp"), "a basename glob is not a substring match");
    }

    void test_path_pattern(scratch const& s) {
        auto const filter = make(s, "src/*\n");
        check(filter.excluded(s.root / "src/a.cpp"), "a git-relative path glob excludes its files");
        check(filter.excluded(s.root / "src/sub/c.cpp"), "`*` crosses `/`");
        check(!filter.excluded(s.root / "tests/t.cpp"), "a git-relative path glob leaves other trees");
    }

    void test_directory(scratch const& s) {
        auto const dir = make(s, "tests\n");
        check(dir.excluded(s.root / "tests/t.cpp"), "a bare directory name excludes its subtree");
        check(!dir.excluded(s.root / "src/a.cpp"), "a bare directory name leaves siblings");

        auto const trailing = make(s, "tests/\n");
        check(trailing.excluded(s.root / "tests/t.cpp"), "a trailing slash names a directory too");

        auto const nested = make(s, "sub\n");
        check(nested.excluded(s.root / "src/sub/c.cpp"), "a directory component anywhere matches");
    }

    void test_absolute(scratch const& s) {
        auto const filter = make(s, (s.root / "src/a.cpp").generic_string() + "\n");
        check(filter.excluded(s.root / "src/a.cpp"), "an absolute pattern matches");
        check(!filter.excluded(s.root / "src/b.test.cpp"), "an absolute pattern leaves siblings");
    }

    void test_scope(scratch const& s) {
        auto const filter = make(s, "", (s.root / "src").generic_string());
        check(filter.has_root(), "the scope fact sets the root");
        check_eq(filter.root().generic_string(), (s.root / "src").generic_string(), "the root is the absolute fact");

        // A relative argument resolves under the scope first.
        auto const resolved = filter.resolve(env_for(s), "a.cpp");
        check(resolved.has_value(), "a relative argument resolves under the scope");
        if (resolved) {
            check_eq(resolved->generic_string(), (s.root / "src/a.cpp").generic_string(), "and to the scoped path");
        }
    }

    void test_engine_flags(scratch const& s) {
        auto const filter = make(s, "*.test.cpp\ntests/\n");
        auto const globs  = filter.rg_globs();
        check(globs.size() == 2, "one ripgrep glob per exclusion");
        check_eq(globs[0], "!*.test.cpp", "a basename exclusion is a negated glob");
        check_eq(globs[1], "!tests/**", "a directory exclusion negates its subtree");

        auto const flags       = filter.grep_excludes();
        bool       has_exclude = false;
        for (auto const& flag : flags) {
            if (flag == "--exclude=*.test.cpp") {
                has_exclude = true;
            }
        }
        check(has_exclude, "the GNU grep fallback gets --exclude for a basename");
    }

    void test_union(scratch const& s) {
        auto const filter = make(s, "a\nb\n");
        check_eq(filter.exclude_fact({"c", "a"}), "a\nb\nc\n", "exclude_fact unions in order without duplicates");
        check_eq(filter.exclude_fact({}), "a\nb\n", "an empty addition keeps the prior patterns");
    }

} // namespace

int main() {
    scratch s;
    test_empty(s);
    test_basename(s);
    test_path_pattern(s);
    test_directory(s);
    test_absolute(s);
    test_scope(s);
    test_engine_flags(s);
    test_union(s);

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("file_filter: ok");
    return 0;
}
