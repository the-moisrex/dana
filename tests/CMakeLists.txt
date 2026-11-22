# Unit tests located in unit/
file(GLOB UNIT_TEST_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp")

foreach(test_src IN LISTS UNIT_TEST_SRCS)
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE dana_lib)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Integration/shell tests
add_test(NAME ai_native
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ai_native.sh
        $<TARGET_FILE:prompt> $<TARGET_FILE:ai.cli>)

# Environment setup for all tests
get_property(_prompt_tests DIRECTORY PROPERTY TESTS)
if(_prompt_tests)
    set_tests_properties(${_prompt_tests} PROPERTIES
            ENVIRONMENT "PROMPT_HISTORY_DIR=${CMAKE_CURRENT_BINARY_DIR}/history;PROMPT_FUNCS=")
endif()
