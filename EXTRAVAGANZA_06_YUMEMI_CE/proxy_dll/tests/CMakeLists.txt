include(GoogleTest)

function(add_proxy_ut name)
    add_executable(${name} ${name}.cpp $<TARGET_OBJECTS:translator_logic>)
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_proxy_ut(ut_translator_logic)
add_proxy_ut(ut_patcher_logic)
