# GoogleTest suites for standalone vn::llm_client (compiled as C++20):
#   - UT:     hermetic unit tests + header-insulation compile check
#   - CT:     multi-request component tests via create_llm_client + fake raw HTTP seam
#   - SYSTEM: live HTTPS/TLS gemini-4-carbon model-degradation baseline gauntlet

add_library(vn_llm_test_gauntlet STATIC
    model_baseline_gauntlet.cpp
    model_baseline_gauntlet.h
)
set_target_properties(vn_llm_test_gauntlet PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(vn_llm_test_gauntlet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(vn_llm_test_gauntlet PRIVATE /W4 /utf-8 /bigobj)
endif()

function(vn_llm_add_test target_name source_file test_label)
    add_executable(${target_name} ${source_file})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /utf-8
            /bigobj
            /external:anglebrackets
            /external:W0
        )
    endif()
    target_link_libraries(${target_name} PRIVATE
        vn::llm_client
        vn_llm_test_gauntlet
        GTest::gtest
        GTest::gtest_main
    )
    gtest_discover_tests(${target_name} PROPERTIES LABELS "${test_label}")
endfunction()

vn_llm_add_test(ut_llm_client ut_llm_client.cpp "UT")
vn_llm_add_test(ct_llm_client ct_llm_client.cpp "CT")
vn_llm_add_test(st_llm_model_baseline st_llm_model_baseline.cpp "SYSTEM")
