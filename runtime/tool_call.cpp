#include "tool_call.hpp"

#include "../extract/document.hpp"
#include "errors.hpp"
#include "prompt_registry.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::schema {

    namespace {

        std::string scalar_to_string(nlohmann::json const& value, std::string_view key, std::string_view kind) {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            if (value.is_number_integer()) {
                return std::to_string(value.get<std::int64_t>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<std::uint64_t>());
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "true" : "false";
            }
            throw usage_error{std::format("tool input '{}': expected {}, got {}\n", key, kind, value.type_name())};
        }

        std::string lowercase(std::string_view text) {
            std::string out(text);
            for (char& ch : out) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return out;
        }

        // Operands are documented in upper case (`PATTERN`) but exposed to a model in
        // lower case (`pattern`), the spelling every tool schema uses.
        std::string operand_key(operand const& op) {
            return lowercase(op.name);
        }

        bool is_known_key(tool const& value, std::string_view key) {
            return std::ranges::any_of(value.parameters,
                                       [&](parameter const& par) {
                                           return par.name == key;
                                       }) ||
                   std::ranges::any_of(value.operands, [&](operand const& op) {
                       return operand_key(op) == key;
                   });
        }

    } // namespace

    nlohmann::ordered_json to_json_value(tool const& value) {
        using nlohmann::ordered_json;
        ordered_json out;
        out["name"]        = value.name;
        out["description"] = value.summary;

        ordered_json properties = ordered_json::object();
        ordered_json required   = ordered_json::array();
        for (auto const& par : value.parameters) {
            ordered_json property;
            if (par.repeatable) {
                property["type"]  = "array";
                property["items"] = ordered_json{
                  {"type", to_string(par.kind)}
                };
            } else {
                property["type"] = to_string(par.kind);
            }
            if (!par.description.empty()) {
                property["description"] = par.description;
            }
            if (!par.enum_values.empty()) {
                property["enum"] = par.enum_values;
            }
            property["x-flag"]   = "--" + par.name;
            properties[par.name] = std::move(property);
            if (par.required) {
                required.push_back(par.name);
            }
        }
        for (auto const& op : value.operands) {
            ordered_json property;
            // A variadic operand takes a list; advertise that so a model does not
            // send a bare string for a tool that accepts several files/commands.
            if (op.variadic) {
                property["type"]  = "array";
                property["items"] = ordered_json{
                  {"type", "string"}
                };
            } else {
                property["type"] = "string";
            }
            if (!op.description.empty()) {
                property["description"] = op.description;
            }
            property["x-positional"] = true;
            std::string const key    = operand_key(op);
            properties[key]          = std::move(property);
            if (op.required) {
                required.push_back(key);
            }
        }
        out["input"] = ordered_json{
          {      "type",              "object"},
          {"properties", std::move(properties)},
          {  "required",              required}
        };
        return out;
    }

    std::string to_json(tool const& value) {
        return to_json_value(value).dump(2);
    }

    std::vector<std::string> parse(tool const& value, nlohmann::json const& input) {
        if (!input.is_object()) {
            throw usage_error{"tool input must be a JSON object\n"};
        }

        std::vector<std::string> argv;

        for (auto const& par : value.parameters) {
            auto const it = input.find(par.name);
            if (it == input.end()) {
                if (par.required) {
                    throw usage_error{std::format("tool input: missing required '{}'\n", par.name)};
                }
                continue;
            }

            if (par.kind == type::boolean) {
                if (!it->is_boolean()) {
                    throw usage_error{
                      std::format("tool input '{}': expected boolean, got {}\n", par.name, it->type_name())};
                }
                if (it->get<bool>()) {
                    argv.push_back("--" + par.name);
                }
                continue;
            }

            auto push_one = [&](nlohmann::json const& item) {
                if (par.kind == type::integer && !item.is_number_integer() && !item.is_number_unsigned()) {
                    throw usage_error{
                      std::format("tool input '{}': expected integer, got {}\n", par.name, item.type_name())};
                }
                if (par.kind == type::string && !item.is_string()) {
                    throw usage_error{
                      std::format("tool input '{}': expected string, got {}\n", par.name, item.type_name())};
                }
                std::string scalar = scalar_to_string(item, par.name, to_string(par.kind));
                if (!par.enum_values.empty() && std::ranges::find(par.enum_values, scalar) == par.enum_values.end()) {
                    throw usage_error{
                      std::format("tool input '{}': '{}' is not one of the allowed values\n", par.name, scalar)};
                }
                argv.push_back("--" + par.name);
                argv.push_back(std::move(scalar));
            };

            if (par.repeatable) {
                // A model often sends a lone string for a list-valued flag; treat it
                // as one occurrence instead of rejecting the call.
                if (it->is_array()) {
                    for (auto const& item : *it) {
                        push_one(item);
                    }
                } else {
                    push_one(*it);
                }
            } else {
                push_one(*it);
            }
        }

        for (auto const& op : value.operands) {
            std::string const key = operand_key(op);
            auto const        it  = input.find(key);
            if (it == input.end()) {
                if (op.required) {
                    throw usage_error{std::format("tool input: missing required '{}'\n", key)};
                }
                continue;
            }
            if (op.variadic) {
                // Accept both the advertised array and a lone value, so one file or
                // one command does not need to be wrapped by the model.
                if (it->is_array()) {
                    for (auto const& item : *it) {
                        argv.push_back(scalar_to_string(item, key, "string"));
                    }
                } else {
                    argv.push_back(scalar_to_string(*it, key, "string"));
                }
            } else if (it->is_array()) {
                if (it->size() != 1) {
                    throw usage_error{
                      std::format("tool input '{}': expected a single value, got {} values\n", key, it->size())};
                }
                argv.push_back(scalar_to_string((*it)[0], key, "string"));
            } else {
                argv.push_back(scalar_to_string(*it, key, "string"));
            }
        }

        for (auto const& [key, unused] : input.items()) {
            (void) unused;
            if (!is_known_key(value, key)) {
                throw usage_error{std::format("tool input: unknown parameter '{}'\n", key)};
            }
        }

        return argv;
    }

} // namespace dana::schema

namespace dana::tool_call {

    namespace {

        // The call's parameters. Models disagree on the envelope: the documented form
        // nests them under `input`, some use the OpenAI-style `arguments`/`parameters`,
        // and some put them beside `tool` directly. Accept all three, and a JSON string
        // holding the object (a provider that double-encodes).
        nlohmann::json input_of(nlohmann::json const& value) {
            for (char const* key : {"input", "arguments", "parameters"}) {
                auto const it = value.find(key);
                if (it == value.end() || it->is_null()) {
                    continue;
                }
                if (it->is_object()) {
                    return *it;
                }
                if (it->is_string()) {
                    if (auto parsed = nlohmann::json::parse(it->get<std::string>(), nullptr, false); parsed.is_object())
                    {
                        return parsed;
                    }
                }
                return *it;
            }

            // Flat shape: everything except the call envelope is the input.
            nlohmann::json input = nlohmann::json::object();
            for (auto const& [key, item] : value.items()) {
                if (key == "tool" || key == "id" || key == "type") {
                    continue;
                }
                input[key] = item;
            }
            return input;
        }

        // Models often run the opening fence straight into the prose
        // ("…then I will.```json"), but the Markdown parser is line-oriented and
        // would miss it, so the loop would stop as if the answer had no call. Put
        // every fence run on its own line before parsing. A fence already at the
        // start of a line is untouched.
        std::string break_inline_fences(std::string_view text) {
            std::string out;
            out.reserve(text.size() + 8);
            for (std::size_t i = 0; i < text.size();) {
                char const marker = text[i];
                bool const fence  = (marker == '`' || marker == '~') && i + 2 < text.size() && text[i + 1] == marker &&
                                    text[i + 2] == marker;
                if (fence && !out.empty() && out.back() != '\n') {
                    out += '\n';
                }
                out += text[i++];
            }
            return out;
        }

        // Models routinely put source code inside a JSON string and escape it as
        // if it were C/C++ (`\v`, `\0`, `\x41`, ...), which JSON rejects. One bad
        // escape used to discard the whole call, so the loop silently stopped.
        // Double the backslash of any escape JSON does not define, leaving valid
        // ones (`\" \\ \/ \b \f \n \r \t \uXXXX`) untouched.
        std::string repair_json_escapes(std::string_view text) {
            std::string out;
            out.reserve(text.size() + 8);
            for (std::size_t i = 0; i < text.size(); ++i) {
                char const c = text[i];
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (i + 1 >= text.size()) {
                    out += "\\\\";
                    continue;
                }
                char const next = text[i + 1];
                switch (next) {
                    case '"':
                    case '\\':
                    case '/':
                    case 'b':
                    case 'f':
                    case 'n':
                    case 'r':
                    case 't':
                        out += c;
                        out += next;
                        ++i;
                        break;
                    case 'u': {
                        bool const hex =
                          i + 5 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 2])) != 0 &&
                          std::isxdigit(static_cast<unsigned char>(text[i + 3])) != 0 &&
                          std::isxdigit(static_cast<unsigned char>(text[i + 4])) != 0 &&
                          std::isxdigit(static_cast<unsigned char>(text[i + 5])) != 0;
                        if (hex) {
                            out += c;
                            out += next;
                            ++i;
                        } else {
                            out += "\\\\";
                        }
                        break;
                    }
                    default: out += "\\\\"; break;
                }
            }
            return out;
        }

        // A top-level array may nest further arrays, so this recurses; the depth is the
        // JSON document's own, which nlohmann already bounded when it parsed.
        // NOLINTNEXTLINE(misc-no-recursion)
        void append_call(std::vector<call>& calls, nlohmann::json const& value, std::string_view block) {
            if (value.is_object()) {
                auto const tool = value.find("tool");
                if (tool == value.end() || !tool->is_string()) {
                    return;
                }
                call found;
                found.name  = tool->get<std::string>();
                found.input = input_of(value);
                found.block = std::string(block);
                calls.push_back(std::move(found));
            } else if (value.is_array()) {
                for (auto const& item : value) {
                    append_call(calls, item, block);
                }
            }
        }

        // DeepSeek V4.1 (and Anthropic-style) inline tool markup. The site emits the
        // calls as text, not as structured `tool_calls`:
        //
        //     <｜｜DSML｜｜ calls>
        //     <｜｜DSML｜｜ invoke name="cli">
        //     <｜｜DSML｜｜ parameter name="command" string="false">["git", "status"]</｜｜DSML｜｜ parameter>
        //     </｜｜DSML｜｜ invoke>
        //     </｜｜DSML｜｜ calls>
        //
        // The token that prefixes every tag varies by dialect (`｜｜DSML｜｜`, a bare
        // `｜DSML｜`/`DSML`, `｜tool▁calls｜`, or nothing at all), so the normaliser strips
        // any `｜…｜` wrapper and the bare markers between `<` and the tag name.

        bool is_fullwidth_bar(std::string_view text, std::size_t pos) {
            return text.substr(pos, 3) == "\xEF\xBD\x9C";
        }

        std::string normalize_dsml(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size()) {
                if (text[i] != '<') {
                    out += text[i++];
                    continue;
                }
                out += '<';
                ++i;
                if (i < text.size() && text[i] == '/') {
                    out += '/';
                    ++i;
                }
                // Drop whitespace and the dialect token before the tag name.
                for (;;) {
                    if (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
                        ++i;
                        continue;
                    }
                    if (is_fullwidth_bar(text, i)) {
                        std::size_t close = i + 3;
                        while (close + 3 <= text.size() && !is_fullwidth_bar(text, close)) {
                            close += 1;
                        }
                        if (close + 3 <= text.size()) {
                            i = close + 3;
                            continue;
                        }
                    }
                    if (text.substr(i, 3) == "126") {
                        i += 3;
                        continue;
                    }
                    if (text.substr(i, 4) == "DSML") {
                        i += 4;
                        continue;
                    }
                    break;
                }
            }
            return out;
        }

        std::string attribute_value(std::string_view tag, std::string_view name) {
            std::string const needle = std::string(name) + "=\"";
            auto const        at     = tag.find(needle);
            if (at == std::string_view::npos) {
                return {};
            }
            auto const start = at + needle.size();
            auto const end   = tag.find('"', start);
            if (end == std::string_view::npos) {
                return {};
            }
            return std::string(tag.substr(start, end - start));
        }

        std::string_view trim_view(std::string_view value) {
            std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }
            std::size_t last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        // A parameter value: `string="true"` is raw text; anything else is JSON, with
        // a literal fallback. The fused `name="x": value}` shape a model occasionally
        // emits is repaired before the JSON attempt.
        nlohmann::json dsml_value(std::string_view raw, std::string_view declared) {
            if (declared == "true") {
                return std::string(raw);
            }
            std::string_view value = trim_view(raw);
            auto             parse = [](std::string_view candidate) -> nlohmann::json {
                return nlohmann::json::parse(candidate, nullptr, false);
            };
            if (auto parsed = parse(value); !parsed.is_discarded()) {
                return parsed;
            }
            if (value.starts_with(':')) {
                value = trim_view(value.substr(1));
                if (value.ends_with('}')) {
                    value = trim_view(value.substr(0, value.size() - 1));
                }
                if (auto parsed = parse(value); !parsed.is_discarded()) {
                    return parsed;
                }
            }
            return std::string(value);
        }

        std::vector<call> parse_dsml(std::string_view text) {
            std::string const normalized = normalize_dsml(text);
            // Require a wrapper or at least one parameter, so prose that merely shows
            // an `<invoke>` example is not mistaken for a call.
            if (!normalized.contains("<tool_calls>") && !normalized.contains("<function_calls>") &&
                !normalized.contains("<parameter"))
            {
                return {};
            }
            std::vector<call> calls;
            std::size_t       pos = 0;
            while (true) {
                auto const invoke = normalized.find("<invoke", pos);
                if (invoke == std::string::npos) {
                    break;
                }
                auto const open_end = normalized.find('>', invoke);
                if (open_end == std::string::npos) {
                    break;
                }
                auto const close = normalized.find("</invoke>", open_end + 1);
                if (close == std::string::npos) {
                    break;
                }
                std::string_view const open_tag(normalized.data() + invoke, open_end - invoke + 1);
                std::string_view const body(normalized.data() + open_end + 1, close - open_end - 1);
                pos = close + 9;

                std::string const name = attribute_value(open_tag, "name");
                if (name.empty()) {
                    continue;
                }

                nlohmann::json input = nlohmann::json::object();
                std::size_t    p     = 0;
                while (true) {
                    auto const param = body.find("<parameter", p);
                    if (param == std::string_view::npos) {
                        break;
                    }
                    auto const param_close = body.find("</parameter>", param);
                    if (param_close == std::string_view::npos) {
                        break;
                    }
                    std::string_view const segment = body.substr(param + 10, param_close - (param + 10));
                    p                              = param_close + 12;

                    std::string_view attrs = segment;
                    std::string_view value;
                    if (auto const gt = segment.find('>'); gt != std::string_view::npos) {
                        attrs = segment.substr(0, gt);
                        value = segment.substr(gt + 1);
                    } else if (auto const name_at = segment.find("name=\""); name_at != std::string_view::npos) {
                        // No `>`: attributes and value are fused; the value starts
                        // after the name attribute's closing quote.
                        if (auto const quote = segment.find('"', name_at + 6); quote != std::string_view::npos) {
                            value = segment.substr(quote + 1);
                        }
                    }

                    std::string const param_name = attribute_value(attrs, "name");
                    if (param_name.empty()) {
                        continue;
                    }
                    input[param_name] = dsml_value(value, attribute_value(attrs, "string"));
                }

                call found;
                found.name  = name;
                found.input = std::move(input);
                found.block = normalized.substr(invoke, close + 9 - invoke);
                calls.push_back(std::move(found));
            }
            return calls;
        }

    } // namespace

    std::vector<call> extract(std::string_view text) {
        std::vector<call> calls;

        // A bare JSON document (the whole answer is one call or an array of them).
        if (auto parsed = nlohmann::json::parse(repair_json_escapes(text), nullptr, false); !parsed.is_discarded()) {
            append_call(calls, parsed, text);
            if (!calls.empty()) {
                return calls;
            }
        }

        // DeepSeek's inline DSML markup (also the Anthropic-style `<invoke>` form).
        if (auto markup = parse_dsml(text); !markup.empty()) {
            return markup;
        }

        // Otherwise every fenced block: `json`-tagged or untagged. A block that is
        // valid JSON without a `tool` key contributes nothing.
        std::string const lined = break_inline_fences(text);
        for (auto const& block : extract::parse_document(lined, "")) {
            if (block.kind != "code") {
                continue;
            }
            if (!block.lang.empty() && block.lang != "json") {
                continue;
            }
            auto parsed = nlohmann::json::parse(repair_json_escapes(block.body), nullptr, false);
            if (parsed.is_discarded()) {
                continue;
            }
            append_call(calls, parsed, block.body);
        }
        return calls;
    }

    std::vector<std::string> to_argv(call const& value) {
        auto const* desc = find_prompt(value.name);
        if (desc == nullptr) {
            throw usage_error{std::format("unknown tool '{}'\n", value.name)};
        }
        if (desc->input == nullptr) {
            throw usage_error{std::format("'{}' is not a callable tool (it declares no input schema)\n", value.name)};
        }
        return schema::parse(*desc->input, value.input);
    }

    std::string format_result(call const& value, std::string_view output, int exit_code, bool ok) {
        std::string out  = "### Tool: " + value.name + "\n";
        out             += "Input: " + value.input.dump() + "\n";
        if (ok) {
            out += "Status: ok\n";
        } else {
            out += std::format("Status: failed (exit {})\n", exit_code);
        }
        out += '\n';
        if (!output.empty()) {
            out += output;
            if (!out.ends_with('\n')) {
                out += '\n';
            }
        }
        return out;
    }

} // namespace dana::tool_call
