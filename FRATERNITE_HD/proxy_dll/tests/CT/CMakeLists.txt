# CT — Component Tests.

include(GoogleTest)

add_executable(translation_pipeline_ct
    translation_pipeline_ct.cpp
    $<TARGET_OBJECTS:translator_logic>
)
target_link_libraries(translation_pipeline_ct PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(translation_pipeline_ct PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(translation_pipeline_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
    GAME_TSV_PATH="C:/Games/FRATERNITE_HD/translations.tsv"
)
gtest_discover_tests(translation_pipeline_ct PROPERTIES LABELS "CT")

# Optional CT: validates RVAs in the dumped EXE. SKIPs when the dump is absent.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hook_sites_ct.cpp")
    add_executable(hook_sites_ct hook_sites_ct.cpp)
    target_link_libraries(hook_sites_ct PRIVATE GTest::gtest GTest::gtest_main)
    target_compile_definitions(hook_sites_ct PRIVATE
        _CRT_SECURE_NO_WARNINGS
        CC_DUMPED_EXE_PATH="${CMAKE_SOURCE_DIR}/../analysys/cc_dumped.exe"
    )
    gtest_discover_tests(hook_sites_ct PROPERTIES LABELS "CT")
endif()
