include(GoogleTest)

add_executable(ut_tone_glossary ut_tone_glossary.cpp)
target_link_libraries(ut_tone_glossary PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_tone_glossary
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_tone_glossary>"
    PROPERTIES LABELS "UT"
)
