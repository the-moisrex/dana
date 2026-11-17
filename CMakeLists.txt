cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROMPT_VERSION)
string(STRIP "${PROMPT_VERSION}" PROMPT_VERSION)

project(dana VERSION ${PROMPT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(IS_GCC TRUE)
    set(IS_CLANG FALSE)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(IS_GCC FALSE)
    set(IS_CLANG TRUE)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (IS_GCC)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -Wall -Wpedantic -Wextra -Wconversion -Wshadow -fsanitize=address")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
    elseif (IS_CLANG)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -Wall -Wpedantic -Wextra -Wconversion -Wshadow -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
    endif()
endif()

# Always: clangd and the code-spanning tools read the build directory.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/import.cmake)

file(GLOB PROMPT_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/prompts/*.cpp")
# One file per extractor (`src/extract/<name>.cpp`), each self-registering like
# a prompt; the grammar, plan and runner live in `src/sdk/extract.{hpp,cpp}`.
file(GLOB_RECURSE EXTRACT_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/extract/*.cpp")
# One file per compressor (`src/compress/<name>.cpp`), each self-registering via
# `compress::register_compressor`; the registry, the fenced-region scanner and
# the planner live in `src/compress/compress.{hpp,cpp}`. Globbing keeps adding a
# compressor to a single new file, like a prompt or an extractor.
file(GLOB_RECURSE COMPRESS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/compress/*.cpp")
# The native AI engine (`src/ai/`): transport, providers, state and the shared
# CLI.  Globbing keeps a new provider a single new file, like a prompt.
file(GLOB_RECURSE AI_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/*.cpp")

# The `new` prompt embeds the SDK headers and an example prompt so an AI can
# write a consistent new one. They are baked into the binary as raw strings at
# build time rather than read from the source tree at run time, so the binary
# stays self-contained; the header is regenerated whenever one of them changes.
set(PROMPT_ASSETS
    prompt_registry_hpp|src/sdk/prompt_registry.hpp
    stage_hpp|src/sdk/stage.hpp
    facts_hpp|src/sdk/facts.hpp
    file_scope_hpp|src/sdk/file_scope.hpp
    chain_state_hpp|src/sdk/chain_state.hpp
    errors_hpp|src/sdk/errors.hpp
    embed_hpp|src/sdk/embed.hpp
    args_hpp|src/sdk/args.hpp
    completion_hpp|src/sdk/completion.hpp
    sink_types_hpp|src/sdk/sink_types.hpp
    sink_hpp|src/sdk/sink.hpp
    process_hpp|src/core/process.hpp
    fs_hpp|src/core/fs.hpp
    example_fix_cpp|src/prompts/fix.cpp
)
set(PROMPT_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PROMPT_ASSET_HPP ${PROMPT_ASSET_DIR}/prompt_assets.hpp)
set(PROMPT_ASSET_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-assets.cmake)
foreach(asset IN LISTS PROMPT_ASSETS)
    string(FIND "${asset}" "|" bar)
    math(EXPR asset_start "${bar} + 1")
    string(SUBSTRING "${asset}" ${asset_start} -1 asset_relpath)
    list(APPEND PROMPT_ASSET_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/${asset_relpath}")
endforeach()
add_custom_command(
    OUTPUT ${PROMPT_ASSET_HPP}
    COMMAND ${CMAKE_COMMAND}
        "-DASSET_OUT=${PROMPT_ASSET_HPP}"
        "-DASSET_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DASSET_FILES=${PROMPT_ASSETS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-assets.cmake"
    DEPENDS ${PROMPT_ASSET_DEPS}
    VERBATIM
)

# OBJECT, not a static library: every prompt registers itself from its own
# translation unit (see sdk/prompt_registry.hpp), and a static archive only
# pulls in members that resolve some undefined symbol — the registrar of a
# prompt nothing references would be dropped, silently unregistering it.
add_library(dana_lib OBJECT
    src/environment.cpp
    src/core/fs.cpp
    src/core/process.cpp
    src/core/clipboard.cpp
    src/core/sanitize.cpp
    src/core/strings.cpp
    src/core/git.cpp
    src/sdk/facts.cpp
    src/sdk/stage.cpp
    src/sdk/wire.cpp
    src/sdk/prompt_registry.cpp
    src/sdk/chaining.cpp
    src/sdk/prompt_file.cpp
    src/sdk/completion.cpp
    src/sdk/line_completion.cpp
    src/sdk/function.cpp
    src/sdk/embed.cpp
    src/sdk/file_scope.cpp
    src/sdk/file_filter.cpp
    src/sdk/args.cpp
    src/sdk/sink.cpp
    src/sdk/agent.cpp
    src/sdk/extract.cpp
    src/sdk/policy.cpp
    src/sdk/history.cpp
    src/sdk/stash.cpp
    src/sdk/terminal.cpp
    src/sdk/project.cpp
    src/sdk/signals.cpp
    src/sdk/schema.cpp
    src/sdk/tool_call.cpp
    src/sdk/tool_exec.cpp
    src/sdk/alias.cpp
    ${EXTRACT_SRCS}
    ${COMPRESS_SRCS}
    ${AI_SRCS}
    ${PROMPT_SRCS}
)
target_sources(dana_lib PRIVATE ${PROMPT_ASSET_HPP})

target_compile_features(dana_lib PUBLIC cxx_std_26)
target_include_directories(dana_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROMPT_ASSET_DIR}
)
target_link_libraries(dana_lib PUBLIC
    nlohmann_json::nlohmann_json
)

# The AI engine's transport.  Prefer libcurl-impersonate (the browser TLS
# fingerprint the signed-in web sessions need); fall back to the system libcurl
# with a warning at run time.
find_path(PROMPT_CURL_INCLUDE_DIR NAMES curl/curl.h)
find_library(PROMPT_CURL_IMPERSONATE_LIBRARY NAMES curl-impersonate)
find_library(PROMPT_CURL_LIBRARY NAMES curl)
if (PROMPT_CURL_IMPERSONATE_LIBRARY AND PROMPT_CURL_INCLUDE_DIR)
    target_include_directories(dana_lib PUBLIC ${PROMPT_CURL_INCLUDE_DIR})
    target_link_libraries(dana_lib PUBLIC ${PROMPT_CURL_IMPERSONATE_LIBRARY})
    target_compile_definitions(dana_lib PUBLIC PROMPT_HAVE_CURL_IMPERSONATE=1)
elseif (PROMPT_CURL_LIBRARY AND PROMPT_CURL_INCLUDE_DIR)
    target_include_directories(dana_lib PUBLIC ${PROMPT_CURL_INCLUDE_DIR})
    target_link_libraries(dana_lib PUBLIC ${PROMPT_CURL_LIBRARY})
else()
    message(WARNING "libcurl not found: the native ai.* providers will not build")
endif()

add_executable(prompt src/main.cpp)
target_link_libraries(prompt PRIVATE dana_lib)
target_compile_definitions(prompt PRIVATE PROMPT_VERSION="${PROMPT_VERSION}")

# The extraction engine's standalone face. It shares `dana_lib` (the parser,
# the registry and the renderers), so `prompt` and `md.extract` can never drift.
add_executable(md.extract src/md_extract_main.cpp)
target_link_libraries(md.extract PRIVATE dana_lib)

# The native `ai.*` dispatcher: one binary, invoked through the `ai.chatgpt`,
# `ai.debate`, … symlinks (argv[0] selects the provider/command).  Installed as
# `ai.cli` because `bin/ai/` is still the Python ai.save package.
add_executable(ai.cli src/ai_main.cpp)
target_link_libraries(ai.cli PRIVATE dana_lib)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    enable_testing()
    add_subdirectory(tests)
endif()

# Component "prompt" keeps `cmake --install --component prompt` from also
# installing the FetchContent'd dependencies (httplib/date) into the prefix.
install(TARGETS prompt md.extract ai.cli RUNTIME DESTINATION bin COMPONENT prompt)

# `ai.chatgpt`, `ai.debate`, `ai.keys`, … are the same native binary; argv[0]
# selects the command.  Created at install time so the repo's `bin/ai/` Python
# package (ai.save) keeps its directory.
set(PROMPT_AI_COMMANDS
    ai.lms ai.openai ai.openrouter ai.claude ai.kimi ai.perplexity ai.gemini ai.dseek ai.chatgpt ai.grok
    ai.gap ai.debate ai.keys)
install(CODE "
    foreach(command IN ITEMS ${PROMPT_AI_COMMANDS})
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink ai.cli
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/\${command}\")
    endforeach()
" COMPONENT prompt)
