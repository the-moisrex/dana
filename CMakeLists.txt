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

file(GLOB PROMPT_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prompts/*.cpp")
# One file per extractor (`extract/extractors/<name>.cpp`), each self-registering like
# a prompt; the grammar, plan and runner live in `runtime/extract.{hpp,cpp}`.
file(GLOB_RECURSE EXTRACT_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/extract/*.cpp")
# One file per compressor (`compress/<name>.cpp`), each self-registering via
# `compress::register_compressor`; the registry, the fenced-region scanner and
# the planner live in `compress/compress.{hpp,cpp}`.
file(GLOB_RECURSE COMPRESS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/compress/*.cpp")
# The native AI engine (`ai/`): transport, providers, state and the shared CLI.
file(GLOB_RECURSE AI_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ai/*.cpp")

# The `new` prompt embeds the runtime/SDK headers and an example prompt.
set(PROMPT_ASSETS
        prompt_registry_hpp|runtime/prompt_registry.hpp
        stage_hpp|runtime/stage.hpp
        facts_hpp|runtime/facts.hpp
        file_scope_hpp|runtime/file_scope.hpp
        chain_state_hpp|runtime/chain_state.hpp
        errors_hpp|runtime/errors.hpp
        embed_hpp|runtime/embed.hpp
        args_hpp|runtime/args.hpp
        completion_hpp|runtime/completion.hpp
        sink_types_hpp|runtime/sink_types.hpp
        sink_hpp|runtime/sink.hpp
        process_hpp|core/process.hpp
        fs_hpp|core/fs.hpp
        example_fix_cpp|prompts/fix.cpp
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

# OBJECT library for all internal components
add_library(dana_lib OBJECT
        core/clipboard.cpp
        core/fs.cpp
        core/git.cpp
        core/glob.cpp
        core/process.cpp
        core/sanitize.cpp
        core/strings.cpp
        core/environment.cpp
        runtime/agent.cpp
        runtime/alias.cpp
        runtime/args.cpp
        runtime/chain_state.cpp
        runtime/chaining.cpp
        runtime/completion.cpp
        runtime/embed.cpp
        runtime/extract.cpp
        runtime/facts.cpp
        runtime/file_filter.cpp
        runtime/file_scope.cpp
        runtime/function.cpp
        runtime/history.cpp
        runtime/line_completion.cpp
        runtime/policy.cpp
        runtime/project.cpp
        runtime/prompt_file.cpp
        runtime/prompt_registry.cpp
        runtime/schema.cpp
        runtime/signals.cpp
        runtime/sink.cpp
        runtime/stage.cpp
        runtime/stash.cpp
        runtime/terminal.cpp
        runtime/tool_call.cpp
        runtime/tool_exec.cpp
        runtime/wire.cpp
        ${EXTRACT_SRCS}
        ${COMPRESS_SRCS}
        ${AI_SRCS}
        ${PROMPT_SRCS}
)
target_sources(dana_lib PRIVATE ${PROMPT_ASSET_HPP})

target_compile_features(dana_lib PUBLIC cxx_std_26)
target_include_directories(dana_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROMPT_ASSET_DIR}
)
target_link_libraries(dana_lib PUBLIC
        nlohmann_json::nlohmann_json
)

# Curl integration for AI engine
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

add_executable(prompt app/prompt/main.cpp)
target_link_libraries(prompt PRIVATE dana_lib)
target_compile_definitions(prompt PRIVATE PROMPT_VERSION="${PROMPT_VERSION}")

add_executable(md.extract app/md-extract/main.cpp)
target_link_libraries(md.extract PRIVATE dana_lib)

add_executable(ai.cli app/ai/main.cpp)
target_link_libraries(ai.cli PRIVATE dana_lib)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS prompt md.extract ai.cli RUNTIME DESTINATION bin COMPONENT prompt)

set(PROMPT_AI_COMMANDS
        ai.lms ai.openai ai.openrouter ai.claude ai.kimi ai.perplexity ai.gemini ai.dseek ai.chatgpt ai.grok
        ai.gap ai.debate ai.keys)
install(CODE "
    foreach(command IN ITEMS ${PROMPT_AI_COMMANDS})
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink ai.cli
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/\${command}\")
    endforeach()
" COMPONENT prompt)
