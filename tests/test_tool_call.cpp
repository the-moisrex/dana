// The tool-call layer: `schema::parse` (the execution contract) and
// `tool_call::extract` (finding calls in an AI answer). The end-to-end sink is
// covered by the ctest smokes in tests/CMakeLists.txt.
#include "../src/sdk/errors.hpp"
#include "../src/sdk/tool_call.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace prompt;

namespace {

    int failures = 0;

    void check(bool cond, char const* what) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    // A tool shaped like `.grep`: a boolean, a repeatable value flag, an integer
    // and two operands (one required, one optional).
    schema::tool grep_like() {
        return schema::make_tool(
          "grep",
          "Search.",
          {
            schema::bool_flag("ignore-case", "Case-insensitive", "i"),
            schema::multi_flag("include", "Only these globs", "GLOB"),
            schema::int_flag("head", "Keep N lines"),
            schema::enum_flag("mode", "Mode", {"fast", "slow"}),
          },
          {
            schema::argument("PATTERN", "The pattern", true),
            schema::argument("PATH", "Where", false),
          });
    }

    void test_parse() {
        schema::tool const tool = grep_like();

        auto const argv = schema::parse(
          tool,
          nlohmann::json{
            {    "pattern",             "TODO"},
            {       "path",              "src"},
            {"ignore-case",               true},
            {    "include", {"*.cpp", "*.hpp"}},
            {       "head",                 10},
            {       "mode",             "fast"},
        });
        std::vector<std::string> const expected = {
          "--ignore-case",
          "--include",
          "*.cpp",
          "--include",
          "*.hpp",
          "--head",
          "10",
          "--mode",
          "fast",
          "TODO",
          "src",
        };
        check(argv == expected, "parse turns JSON into flags then operands");

        // A false boolean is omitted; an optional operand may be absent.
        auto const minimal = schema::parse(
          tool,
          nlohmann::json{
            {    "pattern",   "x"},
            {"ignore-case", false}
        });
        check(minimal == std::vector<std::string>{"x"}, "parse omits false flags and optional operands");

        // Missing required operand.
        bool threw = false;
        try {
            (void) schema::parse(tool,
                                 nlohmann::json{
                                   {"path", "src"}
            });
        } catch (usage_error const&) {
            threw = true;
        }
        check(threw, "parse rejects a missing required operand");

        // Unknown key.
        threw = false;
        try {
            (void) schema::parse(tool,
                                 nlohmann::json{
                                   {"pattern", "x"},
                                   {   "nope",   1}
            });
        } catch (usage_error const&) {
            threw = true;
        }
        check(threw, "parse rejects an unknown key");

        // Wrong type.
        threw = false;
        try {
            (void) schema::parse(tool,
                                 nlohmann::json{
                                   {"pattern",   "x"},
                                   {   "head", "ten"}
            });
        } catch (usage_error const&) {
            threw = true;
        }
        check(threw, "parse rejects a wrong type");

        // Bad enum value.
        threw = false;
        try {
            (void) schema::parse(tool,
                                 nlohmann::json{
                                   {"pattern",      "x"},
                                   {   "mode", "medium"}
            });
        } catch (usage_error const&) {
            threw = true;
        }
        check(threw, "parse rejects a bad enum value");

        // A repeatable flag given a scalar is accepted as one occurrence.
        auto const scalar_repeat = schema::parse(
          tool,
          nlohmann::json{
            {"pattern",     "x"},
            {"include", "*.cpp"}
        });
        check(scalar_repeat == std::vector<std::string>{"--include", "*.cpp", "x"},
              "parse accepts a scalar for a repeatable flag");
    }

    void test_parse_shapes() {
        schema::tool const tool = schema::make_tool(
          "read",
          "Read.",
          {
            schema::multi_flag("include", "Globs", "GLOB"),
          },
          {
            schema::argument("FILE", "Files", true, true),
          });

        // A variadic operand accepts both a scalar and an array.
        check(schema::parse(tool,
                            nlohmann::json{
                              {"file", "a.cpp"}
        }) == std::vector<std::string>{"a.cpp"},
              "parse accepts a scalar for a variadic operand");
        check(schema::parse(tool,
                            nlohmann::json{
                              {"file", {"a.cpp", "b.cpp"}}
        }) == std::vector<std::string>{"a.cpp", "b.cpp"},
              "parse accepts an array for a variadic operand");

        // A non-variadic operand accepts a one-element array.
        schema::tool const single =
          schema::make_tool("glob", "Glob.", {}, {schema::argument("PATTERN", "Pattern", true)});
        check(schema::parse(single,
                            nlohmann::json{
                              {"pattern", {"*.cpp"}}
        }) == std::vector<std::string>{"*.cpp"},
              "parse accepts a one-element array for a scalar operand");
    }

    void test_extract() {
        // One fenced json block among prose.
        auto const one = tool_call::extract(R"(Here you go:
```json
{"tool": "glob", "input": {"pattern": "*.cpp"}}
```
)");
        check(one.size() == 1, "extract finds one fenced call");
        check(one[0].name == "glob", "extract keeps the tool name");
        check(one[0].input["pattern"] == "*.cpp", "extract keeps the input");

        // Several blocks, plus a non-call block that must be ignored.
        auto const many = tool_call::extract(R"(
```json
{"tool": "glob", "input": {"pattern": "*.cpp"}}
```
```json
{"not_a_tool": true}
```
```json
[{"tool": "read", "input": {"file": "a.cpp"}}, {"tool": "grep", "input": {"pattern": "x"}}]
```
)");
        check(many.size() == 3, "extract reads every call and skips non-calls");
        check(many[1].name == "read" && many[2].name == "grep", "extract reads a top-level array of calls");

        // A bare JSON document.
        auto const bare = tool_call::extract(R"({"tool": "read", "input": {"file": "a.cpp"}})");
        check(bare.size() == 1 && bare[0].name == "read", "extract accepts a bare JSON document");

        // Plain prose has no calls.
        check(tool_call::extract("just an answer").empty(), "extract ignores prose");

        // Flat shape: parameters beside `tool`.
        auto const flat = tool_call::extract(R"({"tool": "cli", "command": "cat a.cpp"})");
        check(flat.size() == 1 && flat[0].input["command"] == "cat a.cpp", "extract reads the flat call shape");

        // OpenAI-style `arguments`.
        auto const args = tool_call::extract(R"({"tool": "cli", "arguments": {"command": "ls"}})");
        check(args.size() == 1 && args[0].input["command"] == "ls", "extract reads an `arguments` object");

        // A string-encoded input object.
        auto const encoded = tool_call::extract(R"({"tool": "read", "input": "{\"file\": \"a.cpp\"}"})");
        check(encoded.size() == 1 && encoded[0].input["file"] == "a.cpp", "extract decodes a string input");
    }

    void test_dsml() {
        // DeepSeek V4.1: DSML markup with the `｜126｜` token and a space
        // before the tag name. `string="true"` is raw text, `string="false"` is JSON.
        auto const dsml = tool_call::extract(R"DSML(<｜126｜ calls>
<｜126｜ invoke name="cli">
<｜126｜ parameter name="command" string="false">["git", "status", "--short"]</｜126｜ parameter>
</｜126｜ invoke>
<｜126｜ invoke name="grep">
<｜126｜ parameter name="pattern" string="true">TODO</｜126｜ parameter>
<｜126｜ parameter name="head" string="false">40</｜126｜ parameter>
</｜126｜ invoke>
</｜126｜ calls>)DSML");
        check(dsml.size() == 2, "dsml finds every invoke");
        check(dsml[0].name == "cli", "dsml keeps the tool name");
        check(dsml[0].input["command"].is_array() && dsml[0].input["command"][0] == "git",
              "dsml decodes a JSON array parameter");
        check(dsml[1].input["pattern"] == "TODO", "dsml keeps a raw string parameter");
        check(dsml[1].input["head"] == 40, "dsml decodes a JSON number parameter");

        // The bare `126` token (no fullwidth bars) and no space after it.
        auto const bare_token = tool_call::extract(R"DSML(<126tool_calls>
<126invoke name="glob">
<126parameter name="pattern" string="true">*.cpp</126parameter>
</126invoke>
</126calls>)DSML");
        check(bare_token.size() == 1 && bare_token[0].name == "glob", "dsml accepts the bare token");
        check(bare_token[0].input["pattern"] == "*.cpp", "dsml reads the bare-token parameter");

        // The fused `name="x": value}` shape a model occasionally emits.
        auto const fused = tool_call::extract(R"DSML(<｜126｜ calls>
<｜126｜ invoke name="cli">
<｜126｜ parameter name="command": ["git", "log", "--oneline", "-5"]}</｜126｜ parameter>
</｜126｜ invoke>
</｜126｜ calls>)DSML");
        check(fused.size() == 1, "dsml reads the fused parameter shape");
        check(fused[0].input["command"].is_array() && fused[0].input["command"][0] == "git",
              "dsml repairs the fused value");

        // An Anthropic-style invoke with no wrapper token.
        auto const plain = tool_call::extract(
          R"DSML(<invoke name="read"><parameter name="file" string="true">a.cpp</parameter></invoke>)DSML");
        check(plain.size() == 1 && plain[0].input["file"] == "a.cpp", "dsml reads an unwrapped invoke");

        // Prose that merely mentions an unclosed tag is not a call.
        check(tool_call::extract("use <invoke name=\"read\"> to read").empty(), "dsml ignores unclosed prose");
    }

    void test_format() {
        tool_call::call const call{.name = "glob", .input = nlohmann::json{{"pattern", "*.cpp"}}, .block = {}};
        std::string const     ok = tool_call::format_result(call, "a.cpp\n", 0, true);
        check(ok.contains("### Tool: glob"), "format_result has the heading");
        check(ok.contains("Status: ok"), "format_result marks success");
        check(ok.contains("a.cpp"), "format_result carries the output");

        std::string const failed = tool_call::format_result(call, "boom\n", 2, false);
        check(failed.contains("Status: failed (exit 2)"), "format_result marks failure");
    }

} // namespace

int main() {
    test_parse();
    test_parse_shapes();
    test_extract();
    test_dsml();
    test_format();
    if (failures != 0) {
        std::fprintf(stderr, "%d tool-call check(s) failed\n", failures);
        return 1;
    }
    std::printf("all tool-call tests passed\n");
    return 0;
}
