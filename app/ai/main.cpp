// The native `ai.*` dispatcher.  One binary, invoked through the
// `ai.chatgpt`/`ai.lms`/`ai.keys`/`ai.debate` symlinks: argv[0] selects the
// command, so the installed layout has no per-provider entry scripts.

#include "ai/cli.hpp"
#include "ai/providers/registry.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <vector>

namespace {

    std::string_view base_name(std::string_view path) {
        auto const slash = path.find_last_of('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_DFL);

    std::string              command = argc > 0 ? std::string(base_name(argv[0])) : "ai.cli";
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (command.starts_with("ai.")) {
        command.erase(0, 3);
    } else {
        command.clear();
    }

    if (command == "keys") {
        return dana::ai::run_keys_cli(args);
    }
    if (command == "debate") {
        return dana::ai::run_debate_cli(args);
    }
    if (!command.empty() && dana::ai::canonical_provider(command)) {
        return dana::ai::run_provider_cli(command, args);
    }

    if (!args.empty()) {
        std::string const first = args.front();
        if (first == "keys") {
            return dana::ai::run_keys_cli(std::span{args}.subspan(1));
        }
        if (first == "debate") {
            return dana::ai::run_debate_cli(std::span{args}.subspan(1));
        }
        if (dana::ai::canonical_provider(first)) {
            return dana::ai::run_provider_cli(first, std::span{args.begin() + 1, args.end()});
        }
    }

    std::cerr << "Usage: ai.<provider> [OPTIONS] [PROMPT...]\n"
                 "       ai.keys [PROVIDER...]\n"
                 "       ai.debate PROVIDER... [OPTIONS] [PROMPT...]\n"
                 "Providers:";
    for (auto* instance : dana::ai::all_providers()) {
        std::cerr << ' ' << instance->info().name;
    }
    std::cerr << '\n';
    return 2;
}
