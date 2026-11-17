#include "sentinel.hpp"

#include "diagnostics.hpp"
#include "uuid.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dana::ai::sentinel {

    namespace {

        using nlohmann::json;

        // Browser-shaped values the fingerprint draws from, so the array looks like the
        // one a real client produces.  The server hashes, not inspects, these.
        constexpr std::pair<int, int> kScreens[] = {
          {1920, 1080},
          {1440,  900},
          {2560, 1440},
          {3840, 2160}
        };
        constexpr int              kCores[]        = {8, 16, 24, 32};
        constexpr std::string_view kDocumentKeys[] = {
          "__reactContainer$fzelfjyxej8",
          "_reactListening5dehydibo78",
          "location"};
        constexpr std::string_view kNavigatorKeys[] = {
          "registerProtocolHandler−function registerProtocolHandler() { [native code] }",
          "storage−[object StorageManager]",
          "locks−[object LockManager]",
          "appCodeName−Mozilla",
          "permissions−[object Permissions]",
          "share−function share() { [native code] }",
          "webdriver−false",
          "managed−[object NavigatorManagedData]",
          "canShare−function canShare() { [native code] }",
          "vendor−Google Inc.",
          "mediaDevices−[object MediaDevices]",
          "vibrate−function vibrate() { [native code] }",
          "storageBuckets−[object StorageBucketManager]",
          "mediaCapabilities−[object MediaCapabilities]",
          "cookieEnabled−true",
          "virtualKeyboard−[object VirtualKeyboard]",
          "product−Gecko",
          "presentation−[object Presentation]",
          "onLine−true",
          "mimeTypes−[object MimeTypeArray]",
          "credentials−[object CredentialsContainer]",
          "serviceWorker−[object ServiceWorkerContainer]",
          "keyboard−[object Keyboard]",
          "gpu−[object GPU]",
          "doNotTrack",
          "serial−[object Serial]",
          "pdfViewerEnabled−true",
          "language−zh-CN",
          "geolocation−[object Geolocation]",
          "userAgentData−[object NavigatorUAData]",
          "getUserMedia−function getUserMedia() { [native code] }",
          "sendBeacon−function sendBeacon() { [native code] }",
          "hardwareConcurrency−32",
          "windowControlsOverlay−[object WindowControlsOverlay]",
        };
        constexpr std::string_view kWindowKeys[] = {
          "0",
          "window",
          "self",
          "document",
          "name",
          "location",
          "customElements",
          "history",
          "navigation",
          "innerWidth",
          "innerHeight",
          "scrollX",
          "scrollY",
          "visualViewport",
          "screenX",
          "screenY",
          "outerWidth",
          "outerHeight",
          "devicePixelRatio",
          "screen",
          "chrome",
          "navigator",
          "onresize",
          "performance",
          "crypto",
          "indexedDB",
          "sessionStorage",
          "localStorage",
          "scheduler",
          "alert",
          "atob",
          "btoa",
          "fetch",
          "matchMedia",
          "postMessage",
          "queueMicrotask",
          "requestAnimationFrame",
          "setInterval",
          "setTimeout",
          "caches",
          "__NEXT_DATA__",
          "__BUILD_MANIFEST",
          "__NEXT_PRELOADREADY",
        };

        std::mt19937_64& rng() {
            static thread_local std::mt19937_64 engine{std::random_device{}()};
            return engine;
        }

        double random_double() {
            return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
        }

        template <typename T, std::size_t N>
        T const& random_choice(T const (&items)[N]) {
            std::uniform_int_distribution<std::size_t> pick(0, N - 1);
            return items[pick(rng())];
        }

        std::string parse_time() {
            std::time_t const now = std::time(nullptr);
            std::tm           local{};
            localtime_r(&now, &local);

            char stamp[64] = {};
            std::strftime(stamp, sizeof(stamp), "%a %b %d %Y %H:%M:%S", &local);

            char offset[16] = "+0000";
            if (std::strftime(offset, sizeof(offset), "%z", &local) == 0) {
                std::strcpy(offset, "+0000");
            }

            std::string zone;
#ifdef __GLIBC__
            if (local.tm_zone != nullptr && *local.tm_zone != '\0') {
                zone = local.tm_zone;
            }
#endif
            if (zone.empty()) {
                char buffer[64] = {};
                if (std::strftime(buffer, sizeof(buffer), "%Z", &local) != 0) {
                    zone = buffer;
                }
            }
            if (zone.empty()) {
                zone = "UTC";
            }

            return std::string(stamp) + " GMT" + offset + " (" + zone + ")";
        }

        double epoch_millis() {
            return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch())
              .count();
        }

        double monotonic_millis() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
        }

        // Python's str(float): shortest round-trip form, with a `.0` on integral values.
        std::string python_float(double value) {
            if (std::isnan(value)) {
                return "nan";
            }
            if (std::isinf(value)) {
                return value < 0 ? "-inf" : "inf";
            }
            std::string out = std::format("{}", value);
            if (out.find_first_of(".eE") == std::string::npos) {
                out += ".0";
            }
            return out;
        }

        std::string quote_json(std::string_view text) {
            return json(std::string(text)).dump();
        }

        // ------------------------------------------------------------------- the VM

        struct ordered_map;
        struct value;

        struct value {
            enum struct [[nodiscard]] kind : unsigned char {
                none,
                boolean,
                integer,
                real,
                text,
                list,
                object,
                ordered,
                op
            };

            kind                                type    = kind::none;
            bool                                boolean = false;
            std::int64_t                           integer = 0;
            double                              real    = 0.0;
            std::string                         text;
            std::shared_ptr<std::vector<value>> list;
            std::shared_ptr<ordered_map>        ordered;
            json                                object;
            int                                 op = 0;
        };

        struct ordered_map {
            std::vector<std::string>     keys;
            std::map<std::string, value> values;

            void add(std::string const& key, value const& item) {
                if (!values.contains(key)) {
                    keys.push_back(key);
                }
                values[key] = item;
            }
        };

        value make_none() {
            return value{};
        }

        value make_boolean(bool item) {
            value out;
            out.type    = value::kind::boolean;
            out.boolean = item;
            return out;
        }

        value make_integer(std::int64_t item) {
            value out;
            out.type    = value::kind::integer;
            out.integer = item;
            return out;
        }

        value make_real(double item) {
            value out;
            out.type = value::kind::real;
            out.real = item;
            return out;
        }

        value make_text(std::string item) {
            value out;
            out.type = value::kind::text;
            out.text = std::move(item);
            return out;
        }

        value make_list(std::vector<value> items) {
            value out;
            out.type = value::kind::list;
            out.list = std::make_shared<std::vector<value>>(std::move(items));
            return out;
        }

        value make_ordered() {
            value out;
            out.type    = value::kind::ordered;
            out.ordered = std::make_shared<ordered_map>();
            return out;
        }

        value make_op(int id) {
            value out;
            out.type = value::kind::op;
            out.op   = id;
            return out;
        }

        value from_json(json const& input) {
            if (input.is_null()) {
                return make_none();
            }
            if (input.is_boolean()) {
                return make_boolean(input.get<bool>());
            }
            if (input.is_number_integer()) {
                return make_integer(input.get<std::int64_t>());
            }
            if (input.is_number_unsigned()) {
                return make_integer(static_cast<std::int64_t>(input.get<std::uint64_t>()));
            }
            if (input.is_number_float()) {
                return make_real(input.get<double>());
            }
            if (input.is_string()) {
                return make_text(input.get<std::string>());
            }
            if (input.is_array()) {
                std::vector<value> items;
                items.reserve(input.size());
                for (auto const& item : input) {
                    items.push_back(from_json(item));
                }
                return make_list(std::move(items));
            }
            value out;
            out.type   = value::kind::object;
            out.object = input;
            return out;
        }

        std::string python_repr(value const& item);

        std::string python_str(value const& item) {
            if (item.type == value::kind::text) {
                return item.text;
            }
            return python_repr(item);
        }

        std::string python_repr(value const& item) {
            switch (item.type) {
                case value::kind::none: return "None";
                case value::kind::boolean: return item.boolean ? "True" : "False";
                case value::kind::integer: return std::to_string(item.integer);
                case value::kind::real: return python_float(item.real);
                case value::kind::text: return quote_json(item.text);
                case value::kind::list: {
                    std::string out   = "[";
                    bool        first = true;
                    for (auto const& entry : *item.list) {
                        if (!first) {
                            out += ", ";
                        }
                        out   += python_repr(entry);
                        first  = false;
                    }
                    out += "]";
                    return out;
                }
                case value::kind::object: return item.object.dump();
                case value::kind::ordered:
                case value::kind::op: break;
            }
            return "<object>";
        }

        // `_to_str`: the VM's own stringification (native names, list joining, …).
        std::string to_str(value const& item) {
            static std::map<std::string, std::string> const native{
              {           "window.Math",                       "[object Math]"},
              {        "window.Reflect",                    "[object Reflect]"},
              {    "window.performance",                "[object Performance]"},
              {   "window.localStorage",                    "[object Storage]"},
              {         "window.Object", "function Object() { [native code] }"},
              {    "window.Reflect.set",    "function set() { [native code] }"},
              {"window.performance.now",       "function () { [native code] }"},
              {  "window.Object.create", "function create() { [native code] }"},
              {    "window.Object.keys",   "function keys() { [native code] }"},
              {    "window.Math.random", "function random() { [native code] }"},
            };
            switch (item.type) {
                case value::kind::none: return "undefined";
                case value::kind::boolean: return item.boolean ? "True" : "False";
                case value::kind::integer: return std::to_string(item.integer);
                case value::kind::real: return python_float(item.real);
                case value::kind::text: {
                    auto const found = native.find(item.text);
                    return found == native.end() ? item.text : found->second;
                }
                case value::kind::list: {
                    bool all_text = true;
                    for (auto const& entry : *item.list) {
                        if (entry.type != value::kind::text) {
                            all_text = false;
                            break;
                        }
                    }
                    if (all_text) {
                        std::string out;
                        bool        first = true;
                        for (auto const& entry : *item.list) {
                            if (!first) {
                                out += ",";
                            }
                            out   += entry.text;
                            first  = false;
                        }
                        return out;
                    }
                    return python_repr(item);
                }
                case value::kind::object: return item.object.dump();
                case value::kind::ordered:
                case value::kind::op: break;
            }
            return "<object>";
        }

        bool python_equal(value const& left, value const& right) {
            auto numeric = [](value const& item) {
                return item.type == value::kind::integer || item.type == value::kind::real ||
                       item.type == value::kind::boolean;
            };
            auto number = [](value const& item) -> double {
                if (item.type == value::kind::integer) {
                    return static_cast<double>(item.integer);
                }
                if (item.type == value::kind::boolean) {
                    return item.boolean ? 1.0 : 0.0;
                }
                return item.real;
            };
            if (numeric(left) && numeric(right)) {
                return number(left) == number(right);
            }
            if (left.type != right.type) {
                return false;
            }
            switch (left.type) {
                case value::kind::none: return true;
                case value::kind::text: return left.text == right.text;
                case value::kind::list: {
                    if (left.list->size() != right.list->size()) {
                        return false;
                    }
                    for (std::size_t index = 0; index < left.list->size(); ++index) {
                        if (!python_equal((*left.list)[index], (*right.list)[index])) {
                            return false;
                        }
                    }
                    return true;
                }
                case value::kind::object: return left.object == right.object;
                case value::kind::ordered: return left.ordered == right.ordered;
                case value::kind::op: return left.op == right.op;
                default: return false;
            }
        }

        // Python's json.dumps default separators (", " / ": ").
        std::string python_json_dumps(value const& item) {
            switch (item.type) {
                case value::kind::none: return "null";
                case value::kind::boolean: return item.boolean ? "true" : "false";
                case value::kind::integer: return std::to_string(item.integer);
                case value::kind::real: return python_float(item.real);
                case value::kind::text: return quote_json(item.text);
                case value::kind::list: {
                    std::string out   = "[";
                    bool        first = true;
                    for (auto const& entry : *item.list) {
                        if (!first) {
                            out += ", ";
                        }
                        out   += python_json_dumps(entry);
                        first  = false;
                    }
                    out += "]";
                    return out;
                }
                case value::kind::object: {
                    std::string out   = "{";
                    bool        first = true;
                    for (auto const& [key, entry] : item.object.items()) {
                        if (!first) {
                            out += ", ";
                        }
                        out   += quote_json(key) + ": " + python_json_dumps(from_json(entry));
                        first  = false;
                    }
                    out += "}";
                    return out;
                }
                case value::kind::ordered:
                case value::kind::op: break;
            }
            throw std::runtime_error("not JSON serialisable");
        }

        std::string xor_text(std::string_view text, std::string_view key) {
            if (key.empty()) {
                return std::string(text);
            }
            std::string out(text);
            for (std::size_t index = 0; index < out.size(); ++index) {
                auto const byte = static_cast<unsigned char>(out[index]);
                auto const mask = static_cast<unsigned char>(key[index % key.size()]);
                out[index]      = static_cast<char>(byte ^ mask);
            }
            return out;
        }

        class missing_key : public std::exception {
          public:
            [[nodiscard]] char const* what() const noexcept override {
                return "missing memory key";
            }
        };

        class turnstile_vm {
          public:
            turnstile_vm(json program, std::string key) : started_(std::chrono::steady_clock::now()) {
                memory_[1]  = make_op(1);
                memory_[2]  = make_op(2);
                memory_[3]  = make_op(3);
                memory_[5]  = make_op(5);
                memory_[6]  = make_op(6);
                memory_[7]  = make_op(7);
                memory_[8]  = make_op(8);
                memory_[9]  = from_json(program);
                memory_[10] = make_text("window");
                memory_[14] = make_op(14);
                memory_[15] = make_op(15);
                memory_[16] = make_text(std::move(key));
                memory_[17] = make_op(17);
                memory_[18] = make_op(18);
                memory_[19] = make_op(19);
                memory_[20] = make_op(20);
                memory_[21] = make_op(21);
                memory_[23] = make_op(23);
                memory_[24] = make_op(24);
                program_    = std::move(program);
            }

            std::optional<std::string> run() {
                for (auto const& token : program_) {
                    if (!token.is_array() || token.empty()) {
                        continue;
                    }
                    auto const& head = token[0];
                    if (!head.is_number_integer()) {
                        continue;
                    }
                    auto const found = memory_.find(head.get<std::int64_t>());
                    if (found == memory_.end() || found->second.type != value::kind::op) {
                        continue;
                    }
                    std::vector<value> args;
                    args.reserve(token.size() - 1);
                    for (std::size_t index = 1; index < token.size(); ++index) {
                        args.push_back(from_json(token[index]));
                    }
                    try {
                        call_op(found->second.op, args);
                    } catch (std::exception const&) {
                        // One bad op must not abort the run.
                        continue;
                    }
                }
                if (result_.empty()) {
                    note("turnstile: dx produced no result", std::string_view{"sentinel"});
                    return std::nullopt;
                }
                return result_;
            }

          private:
            static std::int64_t key_of(value const& key) {
                if (key.type != value::kind::integer) {
                    throw missing_key{};
                }
                return key.integer;
            }

            value const& at(value const& key) const {
                auto const found = memory_.find(key_of(key));
                if (found == memory_.end()) {
                    throw missing_key{};
                }
                return found->second;
            }

            static value const& arg(std::vector<value> const& args, std::size_t index) {
                if (index >= args.size()) {
                    throw missing_key{};
                }
                return args[index];
            }

            void call_op(int op, std::vector<value> const& args) {
                if (++depth_ > kMaxDepth) {
                    --depth_;
                    throw std::runtime_error("turnstile: too deep");
                }
                try {
                    dispatch(op, args);
                } catch (...) {
                    --depth_;
                    throw;
                }
                --depth_;
            }

            void dispatch(int op, std::vector<value> const& args) {
                switch (op) {
                    case 1: op_xor(args); break;
                    case 2: op_assign(args); break;
                    case 3: op_result(args); break;
                    case 5: op_concat(args); break;
                    case 6: op_join(args); break;
                    case 7: op_call(args); break;
                    case 8: op_copy(args); break;
                    case 14: op_parse(args); break;
                    case 15: op_stringify(args); break;
                    case 17: op_invoke(args); break;
                    case 18: op_atob(args); break;
                    case 19: op_btoa(args); break;
                    case 20: op_if_equal(args); break;
                    case 21: break;
                    case 23: op_call_if(args); break;
                    case 24: op_join_str(args); break;
                    default: break;
                }
            }

            void op_xor(std::vector<value> const& args) {
                value const&      current     = at(arg(args, 0));
                value const&      source      = at(arg(args, 1));
                std::string const result      = xor_text(to_str(current), to_str(source));
                memory_[key_of(arg(args, 0))] = make_text(result);
            }

            void op_assign(std::vector<value> const& args) {
                memory_[key_of(arg(args, 0))] = arg(args, 1);
            }

            void op_result(std::vector<value> const& args) {
                result_ = encode_base64(to_str(arg(args, 0)));
            }

            void op_concat(std::vector<value> const& args) {
                value const& current  = at(arg(args, 0));
                value const& incoming = at(arg(args, 1));
                if (current.type == value::kind::list) {
                    std::vector<value> merged = *current.list;
                    merged.push_back(incoming);
                    memory_[key_of(arg(args, 0))] = make_list(std::move(merged));
                } else if (current.type == value::kind::text || current.type == value::kind::real ||
                           incoming.type == value::kind::text || incoming.type == value::kind::real)
                {
                    memory_[key_of(arg(args, 0))] = make_text(to_str(current) + to_str(incoming));
                } else {
                    memory_[key_of(arg(args, 0))] = make_text("NaN");
                }
            }

            void op_join(std::vector<value> const& args) {
                value const& left  = at(arg(args, 1));
                value const& right = at(arg(args, 2));
                if (left.type != value::kind::text || right.type != value::kind::text) {
                    return;
                }
                std::string joined = left.text + "." + right.text;
                memory_[key_of(arg(args, 0))] =
                  make_text(joined == "window.document.location" ? "https://chatgpt.com/" : joined);
            }

            void op_call(std::vector<value> const& args) {
                value const&       callee = at(arg(args, 0));
                std::vector<value> values;
                for (std::size_t index = 1; index < args.size(); ++index) {
                    values.push_back(at(args[index]));
                }
                if (callee.type == value::kind::text && callee.text == "window.Reflect.set") {
                    if (values.size() >= 3 && values[0].type == value::kind::ordered) {
                        values[0].ordered->add(python_str(values[1]), values[2]);
                    }
                    return;
                }
                if (callee.type == value::kind::op) {
                    call_op(callee.op, values);
                }
            }

            void op_copy(std::vector<value> const& args) {
                memory_[key_of(arg(args, 0))] = at(arg(args, 1));
            }

            void op_parse(std::vector<value> const& args) {
                value const& source = at(arg(args, 1));
                if (source.type != value::kind::text) {
                    throw missing_key{};
                }
                json parsed                   = json::parse(source.text);
                memory_[key_of(arg(args, 0))] = from_json(parsed);
            }

            void op_stringify(std::vector<value> const& args) {
                memory_[key_of(arg(args, 0))] = make_text(python_json_dumps(at(arg(args, 1))));
            }

            void op_invoke(std::vector<value> const& args) {
                std::int64_t const    target = key_of(arg(args, 0));
                value const&       callee = at(arg(args, 1));
                std::vector<value> values;
                for (std::size_t index = 2; index < args.size(); ++index) {
                    values.push_back(at(args[index]));
                }
                if (callee.type == value::kind::text && callee.text == "window.performance.now") {
                    auto const elapsed =
                      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started_);
                    memory_[target] = make_real((elapsed.count() + random_double()) / 1e6);
                } else if (callee.type == value::kind::text && callee.text == "window.Object.create") {
                    memory_[target] = make_ordered();
                } else if (callee.type == value::kind::text && callee.text == "window.Object.keys") {
                    if (
                      !values.empty() && values[0].type == value::kind::text && values[0].text == "window.localStorage")
                    {
                        std::vector<value> keys;
                        for (std::string_view const key : kLocalStorageKeys) {
                            keys.push_back(make_text(std::string(key)));
                        }
                        memory_[target] = make_list(std::move(keys));
                    }
                } else if (callee.type == value::kind::text && callee.text == "window.Math.random") {
                    memory_[target] = make_real(random_double());
                } else if (callee.type == value::kind::op) {
                    call_op(callee.op, values);
                    memory_[target] = make_none();
                }
            }

            void op_atob(std::vector<value> const& args) {
                auto const decoded = decode_base64(to_str(at(arg(args, 0))));
                if (!decoded) {
                    throw std::runtime_error("turnstile: atob failed");
                }
                memory_[key_of(arg(args, 0))] = make_text(*decoded);
            }

            void op_btoa(std::vector<value> const& args) {
                memory_[key_of(arg(args, 0))] = make_text(encode_base64(to_str(at(arg(args, 0)))));
            }

            void op_if_equal(std::vector<value> const& args) {
                value const& left   = at(arg(args, 0));
                value const& right  = at(arg(args, 1));
                value const& action = at(arg(args, 2));
                if (!python_equal(left, right) || action.type != value::kind::op) {
                    return;
                }
                std::vector<value> values;
                for (std::size_t index = 3; index < args.size(); ++index) {
                    values.push_back(at(args[index]));
                }
                call_op(action.op, values);
            }

            void op_call_if(std::vector<value> const& args) {
                value const& target = at(arg(args, 0));
                value const& callee = at(arg(args, 1));
                if (target.type == value::kind::none || callee.type != value::kind::op) {
                    return;
                }
                std::vector<value> values(args.begin() + 2, args.end());
                call_op(callee.op, values);
            }

            void op_join_str(std::vector<value> const& args) {
                value const& left  = at(arg(args, 1));
                value const& right = at(arg(args, 2));
                if (left.type != value::kind::text || right.type != value::kind::text) {
                    return;
                }
                memory_[key_of(arg(args, 0))] = make_text(left.text + "." + right.text);
            }

            static constexpr int              kMaxDepth           = 100;
            static constexpr std::string_view kLocalStorageKeys[] = {
              "STATSIG_LOCAL_STORAGE_INTERNAL_STORE_V4",
              "STATSIG_LOCAL_STORAGE_STABLE_ID",
              "client-correlated-secret",
              "oai/apps/capExpiresAt",
              "oai-did",
              "STATSIG_LOCAL_STORAGE_LOGGING_REQUEST",
              "UiState.isNavigationCollapsed.1",
            };

            std::map<std::int64_t, value>            memory_;
            json                                  program_;
            std::string                           result_;
            int                                   depth_ = 0;
            std::chrono::steady_clock::time_point started_;
        };

    } // namespace

    nlohmann::json build_fingerprint(
      std::string_view           user_agent,
      std::optional<std::string> script_source,
      std::string_view           data_build) {
        double const now    = monotonic_millis();
        auto const&  screen = random_choice(kScreens);

        json fingerprint = json::array();
        fingerprint.push_back(screen.first + screen.second);
        fingerprint.push_back(parse_time());
        fingerprint.push_back(4'294'705'152LL);
        fingerprint.push_back(1);
        fingerprint.push_back(std::string(user_agent));
        fingerprint.push_back(script_source ? json(*script_source) : json(nullptr));
        fingerprint.push_back(std::string(data_build));
        fingerprint.push_back("en-US");
        fingerprint.push_back("en-US,en");
        fingerprint.push_back(random_double());
        fingerprint.push_back(std::string(random_choice(kNavigatorKeys)));
        fingerprint.push_back(std::string(random_choice(kDocumentKeys)));
        fingerprint.push_back(std::string(random_choice(kWindowKeys)));
        fingerprint.push_back(now);
        fingerprint.push_back(uuid4());
        fingerprint.push_back("");
        fingerprint.push_back(random_choice(kCores));
        fingerprint.push_back(epoch_millis() - now);
        for (int index = 0; index < 6; ++index) {
            fingerprint.push_back(0);
        }
        fingerprint.push_back(std::string(user_agent).find("Firefox") != std::string::npos ? 1 : 0);
        return fingerprint;
    }

    std::string requirements_token(nlohmann::json const& fingerprint) {
        return std::string(kRequestPrefix) + encode_base64(fingerprint.dump());
    }

    std::string zvt(std::string_view text) {
        std::uint32_t value = 2'166'136'261U;
        std::size_t   index = 0;
        while (index < text.size()) {
            auto const    byte      = static_cast<unsigned char>(text[index]);
            std::uint32_t codepoint = byte;
            std::size_t   length    = 1;
            if ((byte & 0x80U) != 0) {
                // Decode one UTF-8 code point so `ord(char)` matches Python.
                std::size_t const extra =
                  (byte & 0xE0U) == 0xC0U   ? 1
                  : (byte & 0xF0U) == 0xE0U ? 2
                  : (byte & 0xF8U) == 0xF0U ? 3
                                            : 0;
                if (extra != 0 && index + extra < text.size()) {
                    codepoint = byte & (0x3FU >> extra);
                    for (std::size_t offset = 1; offset <= extra; ++offset) {
                        auto const next = static_cast<unsigned char>(text[index + offset]);
                        codepoint       = (codepoint << 6U) | (next & 0x3FU);
                    }
                    length = extra + 1;
                }
            }
            value ^= codepoint;
            value *= 16'777'619U;
            index += length;
        }
        value ^= value >> 16;
        value *= 2'246'822'507U;
        value ^= value >> 13;
        value *= 3'266'489'909U;
        value ^= value >> 16;

        char buffer[9] = {};
        std::snprintf(buffer, sizeof(buffer), "%08x", value);
        return buffer;
    }

    std::string solve_proof(
      std::string const&    seed,
      std::string const&    difficulty,
      nlohmann::json const& fingerprint,
      std::string_view      algo,
      int                   limit) {
        if (seed.empty() || difficulty.empty()) {
            throw error("the proof-of-work challenge is malformed");
        }
        if (algo == "sha3") {
            throw error("the sha3 proof-of-work shape is not supported by the native solver");
        }

        std::size_t const length = difficulty.size();
        for (int attempt = 0; attempt < limit; ++attempt) {
            json config              = fingerprint;
            config[3]                = attempt;
            config[9]                = 0;
            std::string const answer = encode_base64(config.dump());
            if (zvt(seed + answer).substr(0, length) <= difficulty) {
                return std::string(kProofPrefix) + answer + "~S";
            }
        }
        throw error("proof-of-work unsolved after " + std::to_string(limit) + " attempts");
    }

    std::optional<std::string> solve_turnstile(std::string const& dx, std::string const& key) {
        auto const decoded = decode_base64(dx);
        if (!decoded) {
            note("turnstile: could not decode dx", std::string_view{"sentinel"});
            return std::nullopt;
        }
        std::string const plain   = xor_text(*decoded, key);
        json              program = json::parse(plain, nullptr, false);
        if (program.is_discarded() || !program.is_array()) {
            note("turnstile: could not decode dx", std::string_view{"sentinel"});
            return std::nullopt;
        }
        turnstile_vm vm(std::move(program), key);
        return vm.run();
    }

    std::string encode_base64(std::string_view data) {
        static constexpr std::string_view kAlphabet =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        std::size_t index = 0;
        while (index + 2 < data.size()) {
            auto const a = static_cast<unsigned>(static_cast<unsigned char>(data[index]));
            auto const b = static_cast<unsigned>(static_cast<unsigned char>(data[index + 1]));
            auto const c = static_cast<unsigned>(static_cast<unsigned char>(data[index + 2]));
            out.push_back(kAlphabet[a >> 2U]);
            out.push_back(kAlphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
            out.push_back(kAlphabet[((b & 0x0FU) << 2U) | (c >> 6U)]);
            out.push_back(kAlphabet[c & 0x3FU]);
            index += 3;
        }
        if (index < data.size()) {
            auto const a = static_cast<unsigned>(static_cast<unsigned char>(data[index]));
            auto const b =
              index + 1 < data.size() ? static_cast<unsigned>(static_cast<unsigned char>(data[index + 1])) : 0U;
            out.push_back(kAlphabet[a >> 2U]);
            out.push_back(kAlphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
            out.push_back(index + 1 < data.size() ? kAlphabet[(b & 0x0FU) << 2U] : '=');
            out.push_back('=');
        }
        return out;
    }

    std::optional<std::string> decode_base64(std::string_view data) {
        auto value_of = [](char character) -> int {
            if (character >= 'A' && character <= 'Z') {
                return character - 'A';
            }
            if (character >= 'a' && character <= 'z') {
                return character - 'a' + 26;
            }
            if (character >= '0' && character <= '9') {
                return character - '0' + 52;
            }
            if (character == '+' || character == '-') {
                return 62;
            }
            if (character == '/' || character == '_') {
                return 63;
            }
            return -1;
        };
        std::string out;
        out.reserve((data.size() / 4) * 3);
        std::uint32_t buffer = 0;
        int           bits   = 0;
        for (char const character : data) {
            if (character == '=' || character == '\n' || character == '\r' || character == ' ' || character == '\t') {
                continue;
            }
            int const value = value_of(character);
            if (value < 0) {
                return std::nullopt;
            }
            buffer  = (buffer << 6U) | static_cast<std::uint32_t>(value);
            bits   += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
            }
        }
        return out;
    }

} // namespace dana::ai::sentinel
