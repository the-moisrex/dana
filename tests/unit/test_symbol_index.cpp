// The C/C++ symbol index behind `.spp`: the scanner fallback, the on-disk cache
// (including an incremental rescan after a file changes), dependency/caller
// extraction, and the completion candidates. ctags is optional, so these tests
// only rely on the built-in scanner and never on a particular ctags version.
#include "../../core/strings.hpp"
#include "../../runtime/completion.hpp"
#include "../../runtime/prompt_registry.hpp"
#include "../../tools/symbol_index.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace dana;

namespace {

    int failures = 0;

    void check(bool cond, char const* what) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    bool has_candidate(std::string_view text, std::string_view value) {
        std::string const body(text);
        std::string const bare{value};
        return body.starts_with(bare + "\t") || body.contains("\n" + bare + "\t") || body.contains("\n" + bare + "\n");
    }

    // True when `line` appears as a complete line, description included.
    bool has_line(std::string_view text, std::string_view line) {
        std::string const body(text);
        std::string const needle = "\n" + std::string(line) + "\n";
        return body.starts_with(std::string(line) + "\n") || body.contains(needle);
    }

    void write_file(std::filesystem::path const& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    struct temp_tree {
        std::filesystem::path root;

        explicit temp_tree(std::string_view name)
          : root(std::filesystem::temp_directory_path() /
                 (std::string(name) + "-" + std::to_string(static_cast<long>(::getpid())))) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root);
        }

        temp_tree(temp_tree const&)            = delete;
        temp_tree& operator=(temp_tree const&) = delete;

        ~temp_tree() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    // Point the cache at the scratch tree so the real user cache is never touched.
    struct cache_scope {
        std::string saved;
        bool        had = false;

        explicit cache_scope(std::filesystem::path const& dir) {
            if (char const* previous = std::getenv("PROMPT_SYMBOL_CACHE_DIR"); previous != nullptr) {
                saved = previous;
                had   = true;
            }
            ::setenv("PROMPT_SYMBOL_CACHE_DIR", dir.c_str(), 1);
            tools::reset_symbol_index();
        }

        ~cache_scope() {
            if (had) {
                ::setenv("PROMPT_SYMBOL_CACHE_DIR", saved.c_str(), 1);
            } else {
                ::unsetenv("PROMPT_SYMBOL_CACHE_DIR");
            }
            tools::reset_symbol_index();
        }
    };

    // Prepend `dir` to PATH for the lifetime of the scope.
    struct path_scope {
        std::string saved;
        bool        had = false;

        explicit path_scope(std::filesystem::path const& dir) {
            if (char const* previous = std::getenv("PATH"); previous != nullptr) {
                saved = previous;
                had   = true;
            }
            std::string const value = dir.string() + ":" + (had ? saved : "");
            ::setenv("PATH", value.c_str(), 1);
        }

        ~path_scope() {
            if (had) {
                ::setenv("PATH", saved.c_str(), 1);
            } else {
                ::unsetenv("PATH");
            }
        }
    };

    void write_fixture(std::filesystem::path const& root) {
        write_file(root / "a.hpp",
                   "#pragma once\n"
                   "struct Widget {\n"
                   "    int size();\n"
                   "};\n");
        write_file(
          root / "a.cpp",
          "#include \"a.hpp\"\n"
          "\n"
          "int helper() {\n"
          "    return 1;\n"
          "}\n"
          "\n"
          "int Widget::size() {\n"
          "    return helper();\n"
          "}\n");
        write_file(root / "b.cpp",
                   "#include \"a.hpp\"\n"
                   "\n"
                   "int use_widget(Widget& widget) {\n"
                   "    return widget.size();\n"
                   "}\n");
    }

    bool has_symbol(std::vector<tools::symbol_entry> const& entries, std::string_view name) {
        for (auto const& entry : entries) {
            if (entry.name == name) {
                return true;
            }
        }
        return false;
    }

    // The scanner is the fallback source, and it records the closing line so the
    // dependency/caller range is known.
    void test_scanner_index(temp_tree const& tree) {
        cache_scope const cache(tree.root / "cache");
        write_fixture(tree.root);

        auto const& entries = tools::symbol_index(tree.root);
        check(has_symbol(entries, "Widget"), "index: the struct is found");
        check(has_symbol(entries, "helper"), "index: the free function is found");
        check(has_symbol(entries, "size"), "index: the member function is found");
        check(has_symbol(entries, "use_widget"), "index: the second file is scanned");

        auto const helper = tools::find_symbol("helper", tree.root);
        check(helper.has_value(), "index: find_symbol resolves a name");
        if (helper) {
            check(helper->path == "a.cpp", "index: paths are repository-relative");
            check(helper->line == 3, "index: the declaration line is recorded");
            check(helper->end_line >= helper->line, "index: the closing line is recorded");
        }

        check(!tools::find_symbol("no_such_symbol", tree.root).has_value(), "index: an unknown name is absent");
    }

    // The cache is written, and a changed file is the only thing rescanned.
    void test_cache_round_trip(temp_tree const& tree) {
        cache_scope const cache(tree.root / "cache");
        write_fixture(tree.root);

        auto const& first = tools::symbol_index(tree.root);
        check(has_symbol(first, "helper"), "cache: the initial index is built");

        bool            cache_exists = false;
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(tree.root / "cache", ec)) {
            if (entry.path().extension() == ".idx") {
                cache_exists = true;
            }
        }
        check(cache_exists, "cache: an .idx file is written");

        // A second lookup in a fresh process-memo state reads the cache and keeps
        // every symbol.
        tools::reset_symbol_index();
        auto const& reused = tools::symbol_index(tree.root);
        check(has_symbol(reused, "helper") && has_symbol(reused, "Widget"), "cache: a revalidation keeps the entries");

        // Change one file: the new symbol appears and the untouched files stay.
        write_file(
          tree.root / "b.cpp",
          "#include \"a.hpp\"\n"
          "\n"
          "int use_widget(Widget& widget) {\n"
          "    return widget.size();\n"
          "}\n"
          "\n"
          "int added_later() {\n"
          "    return 0;\n"
          "}\n");
        tools::reset_symbol_index();
        auto const& changed = tools::symbol_index(tree.root);
        check(has_symbol(changed, "added_later"), "cache: a changed file is rescanned");
        check(has_symbol(changed, "helper"), "cache: an unchanged file keeps its entries");
    }

    // Dependencies come from identifiers in the target's range, resolved against
    // the index; callers are the symbols enclosing a reference to the target.
    void test_dependencies_and_callers(temp_tree const& tree) {
        cache_scope const cache(tree.root / "cache");
        write_fixture(tree.root);

        auto const target = tools::find_symbol("use_widget", tree.root);
        check(target.has_value(), "graph: the target is indexed");
        if (target) {
            auto const deps = tools::dependencies(*target, tree.root);
            check(has_symbol(deps, "Widget"), "graph: a type used by the target is a dependency");
            check(has_symbol(deps, "size"), "graph: a member called by the target is a dependency");
            check(!has_symbol(deps, "use_widget"), "graph: the target is not its own dependency");
        }

        auto const helper = tools::find_symbol("helper", tree.root);
        check(helper.has_value(), "graph: the callee is indexed");
        if (helper) {
            auto const callers = tools::callers(*helper, tree.root);
            check(has_symbol(callers, "size"), "graph: the enclosing function is a caller");
            check(!has_symbol(callers, "helper"), "graph: a symbol is not its own caller");
        }
    }

    // Completion emits `name<TAB>path:line`, filters by prefix, and skips names
    // already typed.
    void test_completion(temp_tree const& tree) {
        cache_scope const cache(tree.root / "cache");
        write_fixture(tree.root);

        std::ostringstream                  out;
        std::vector<std::string_view> const prior;
        tools::complete_symbols("use", prior, tree.root, out);
        check(has_line(out.str(), "use_widget\tb.cpp:3"), "completion: a matching symbol carries its location");
        check(!out.str().contains("helper"), "completion: the prefix filters unrelated symbols");

        std::ostringstream                  typed;
        std::vector<std::string_view> const prior_typed{"use_widget"};
        tools::complete_symbols("use", prior_typed, tree.root, typed);
        check(typed.str().empty(), "completion: an already typed symbol is not offered again");
    }

    // A ctags run is merged with the scanner. The fake binary emits one tabbed
    // entry for the first file, which only ctags knows about.
    void test_ctags_merge(temp_tree const& tree) {
        cache_scope const cache(tree.root / "cache");
        write_fixture(tree.root);

        auto const bin = tree.root / "bin";
        write_file(
          bin / "ctags",
          "#!/bin/sh\n"
          "if [ \"$1\" = \"--version\" ]; then\n"
          "  echo 'fake ctags 1.0'\n"
          "  exit 0\n"
          "fi\n"
          "file=$(head -n 1)\n"
          "printf 'ctags_only\\t%s\\t1\\tfunction\\n' \"$file\"\n");
        std::error_code ec;
        std::filesystem::permissions(
          bin / "ctags",
          std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
          std::filesystem::perm_options::add,
          ec);

        path_scope const path(bin);
        tools::reset_symbol_index();
        auto const& entries = tools::symbol_index(tree.root);
        check(has_symbol(entries, "ctags_only"), "ctags: an entry only ctags knows about is merged in");
        check(has_symbol(entries, "helper"), "ctags: the scanner's entries survive the merge");
    }

    // The `spp` prompt registered a complete_fn and offers the `+` modifiers.
    void test_prompt_wiring(temp_tree const& tree) {
        auto const* spp = find_prompt("spp");
        check(spp != nullptr, "wiring: spp is registered");
        if (spp == nullptr || spp->complete_fn == nullptr) {
            check(false, "wiring: spp registered a complete_fn");
            return;
        }

        std::ostringstream       out;
        completion_request const request{.prefix = "+", .prior_args = {}, .git_root = tree.root};
        spp->complete_fn(request, out);
        check(has_candidate(out.str(), "+deps"), "wiring: +deps is offered");
        check(has_candidate(out.str(), "+deps.all.outline"), "wiring: the outline variants are offered");
        check(has_candidate(out.str(), "+callers"), "wiring: +callers is offered");
        check(has_candidate(out.str(), "+file"), "wiring: +file is offered");

        std::ostringstream       empty;
        completion_request const bare{.prefix = "", .prior_args = {}, .git_root = tree.root};
        spp->complete_fn(bare, empty);
        check(has_candidate(empty.str(), "+deps"), "wiring: the empty prefix offers modifiers");
        check(!empty.str().contains("use_widget"), "wiring: the empty prefix does not dump every symbol");
    }

} // namespace

int main() {
    temp_tree const tree("prompt-test-symbol-index");

    test_scanner_index(tree);
    test_cache_round_trip(tree);
    test_dependencies_and_callers(tree);
    test_completion(tree);
    test_ctags_merge(tree);
    test_prompt_wiring(tree);

    if (failures != 0) {
        std::fprintf(stderr, "%d symbol-index test(s) failed\n", failures);
        return 1;
    }
    std::printf("symbol-index tests passed\n");
    return 0;
}
