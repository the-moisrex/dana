# Core unit tests - these test components directly without spawning processes
add_executable(test_chaining test_chaining.cpp)
target_link_libraries(test_chaining PRIVATE dana_lib)
target_include_directories(test_chaining PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME chaining COMMAND test_chaining)

add_executable(test_errors test_errors.cpp)
target_link_libraries(test_errors PRIVATE dana_lib)
target_include_directories(test_errors PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME errors COMMAND test_errors)

add_executable(test_prompt_file test_prompt_file.cpp)
target_link_libraries(test_prompt_file PRIVATE dana_lib)
target_include_directories(test_prompt_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME prompt_file COMMAND test_prompt_file)

add_executable(test_function test_function.cpp)
target_link_libraries(test_function PRIVATE dana_lib)
target_include_directories(test_function PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME function COMMAND test_function)

add_executable(test_fs test_fs.cpp)
target_link_libraries(test_fs PRIVATE dana_lib)
target_include_directories(test_fs PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fs COMMAND test_fs)

add_executable(test_sanitize test_sanitize.cpp)
target_link_libraries(test_sanitize PRIVATE dana_lib)
target_include_directories(test_sanitize PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME sanitize COMMAND test_sanitize)

add_executable(test_sink test_sink.cpp)
target_link_libraries(test_sink PRIVATE dana_lib)
target_include_directories(test_sink PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME sink COMMAND test_sink)

add_executable(test_facts test_facts.cpp)
target_link_libraries(test_facts PRIVATE dana_lib)
target_include_directories(test_facts PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME facts COMMAND test_facts)

add_executable(test_alias test_alias.cpp)
target_link_libraries(test_alias PRIVATE dana_lib)
target_include_directories(test_alias PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME alias COMMAND test_alias)

add_executable(test_compress test_compress.cpp)
target_link_libraries(test_compress PRIVATE dana_lib)
target_include_directories(test_compress PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME compress COMMAND test_compress)

add_executable(test_schema test_schema.cpp)
target_link_libraries(test_schema PRIVATE dana_lib)
target_include_directories(test_schema PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME schema COMMAND test_schema)

add_executable(test_tool_call test_tool_call.cpp)
target_link_libraries(test_tool_call PRIVATE dana_lib)
target_include_directories(test_tool_call PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME tool_call COMMAND test_tool_call)

add_executable(test_file_scope test_file_scope.cpp)
target_link_libraries(test_file_scope PRIVATE dana_lib)
target_include_directories(test_file_scope PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME file_scope COMMAND test_file_scope)

add_executable(test_file_filter test_file_filter.cpp)
target_link_libraries(test_file_filter PRIVATE dana_lib)
target_include_directories(test_file_filter PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME file_filter COMMAND test_file_filter)

add_executable(test_embed test_embed.cpp)
target_link_libraries(test_embed PRIVATE dana_lib)
target_include_directories(test_embed PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME embed COMMAND test_embed)

add_executable(test_strings test_strings.cpp)
target_link_libraries(test_strings PRIVATE dana_lib)
target_include_directories(test_strings PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME strings COMMAND test_strings)

add_executable(test_completion test_completion.cpp)
target_link_libraries(test_completion PRIVATE dana_lib)
target_include_directories(test_completion PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME completion COMMAND test_completion)

add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE dana_lib)
target_include_directories(test_history PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME history COMMAND test_history)

add_executable(test_stash test_stash.cpp)
target_link_libraries(test_stash PRIVATE dana_lib)
target_include_directories(test_stash PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME stash COMMAND test_stash)

add_executable(test_clipboard test_clipboard.cpp)
target_link_libraries(test_clipboard PRIVATE dana_lib)
target_include_directories(test_clipboard PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME clipboard_identifier COMMAND test_clipboard)

# AI engine tests
add_executable(test_ai test_ai.cpp)
target_link_libraries(test_ai PRIVATE dana_lib)
target_include_directories(test_ai PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ai COMMAND test_ai)

add_executable(test_ai_debate test_ai_debate.cpp)
target_link_libraries(test_ai_debate PRIVATE dana_lib)
target_include_directories(test_ai_debate PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ai_debate COMMAND test_ai_debate)

add_executable(test_ai_pow test_ai_pow.cpp)
target_link_libraries(test_ai_pow PRIVATE dana_lib)
target_include_directories(test_ai_pow PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ai_pow COMMAND test_ai_pow)

add_executable(test_agent_loop test_agent_loop.cpp)
target_link_libraries(test_agent_loop PRIVATE dana_lib)
target_include_directories(test_agent_loop PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME agent_loop COMMAND test_agent_loop)

add_executable(test_tse_tool test_tse_tool.cpp)
target_link_libraries(test_tse_tool PRIVATE dana_lib)
target_include_directories(test_tse_tool PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME tse_tool COMMAND test_tse_tool)

# === NEW: Integrated prompt execution tests ===
# These replace the many CMake command-line tests with fast in-process tests

add_executable(test_prompt_exec test_prompt_exec.cpp)
target_link_libraries(test_prompt_exec PRIVATE dana_lib)
target_include_directories(test_prompt_exec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME prompt_exec COMMAND test_prompt_exec)

add_executable(test_tools_exec test_tools_exec.cpp)
target_link_libraries(test_tools_exec PRIVATE dana_lib)
target_include_directories(test_tools_exec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME tools_exec COMMAND test_tools_exec)

add_executable(test_agent_exec test_agent_exec.cpp)
target_link_libraries(test_agent_exec PRIVATE dana_lib)
target_include_directories(test_agent_exec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME agent_exec COMMAND test_agent_exec)

add_executable(test_ai_sinks test_ai_sinks.cpp)
target_link_libraries(test_ai_sinks PRIVATE dana_lib)
target_include_directories(test_ai_sinks PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ai_sinks COMMAND test_ai_sinks)

# === End-to-end shell tests (kept for integration coverage) ===

# These test the actual binary through its CLI, verifying the full stack
# including argument parsing, process forking, and shell integration.

add_test(NAME ai_sink_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_sink.sh $<TARGET_FILE:prompt>)

add_test(NAME ai_native
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_native.sh
        $<TARGET_FILE:prompt> $<TARGET_FILE:ai.cli>)

add_test(NAME ai_debate_sink
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_debate_sink.sh $<TARGET_FILE:prompt>)

add_test(NAME ai_reply_sink
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_reply.sh $<TARGET_FILE:prompt>)

add_test(NAME save_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_save.sh
        $<TARGET_FILE:prompt> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/numbered.txt)

add_test(NAME clear_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_clear.sh $<TARGET_FILE:prompt>)

add_test(NAME stash_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_stash.sh $<TARGET_FILE:prompt>)

add_test(NAME history_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_history.sh $<TARGET_FILE:prompt>)

add_test(NAME file_dedup
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_file_dedup.sh $<TARGET_FILE:prompt>)

add_test(NAME file_filter_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_file_filter.sh $<TARGET_FILE:prompt>)

add_test(NAME clipboard_prompt
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_prompt.sh $<TARGET_FILE:prompt>)

add_test(NAME output_fold
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_output.sh $<TARGET_FILE:prompt>)

add_test(NAME yt_roundtrip
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_yt.sh $<TARGET_FILE:prompt>)

add_test(NAME tools_run_reads_embedded_file
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_tools_read_embedded.sh $<TARGET_FILE:prompt>)

add_test(NAME agent_extract_bind
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_extract_bind.sh $<TARGET_FILE:prompt>)

# === Python unit tests (kept for complex protocol testing) ===

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ai_debate_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_debate.py)
    add_test(NAME ai_lms_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_lms.py)
    add_test(NAME ai_openrouter_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_openrouter.py)
    add_test(NAME ai_claude_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_claude.py)
    add_test(NAME ai_gap_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_gap.py)
    add_test(NAME ai_perplexity_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_perplexity.py)
    add_test(NAME ai_gemini_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_gemini.py)
    add_test(NAME ai_reply_state
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ai_state.py)
    add_test(NAME md_extract_unit
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_md_extract.py
            $<TARGET_FILE:md.extract>)
endif()

# === Environment setup for all tests ===

get_property(_prompt_tests DIRECTORY PROPERTY TESTS)
if(_prompt_tests)
    set_tests_properties(${_prompt_tests} PROPERTIES
            ENVIRONMENT "PROMPT_HISTORY_DIR=${CMAKE_CURRENT_BINARY_DIR}/history;PROMPT_FUNCS=")
endif()