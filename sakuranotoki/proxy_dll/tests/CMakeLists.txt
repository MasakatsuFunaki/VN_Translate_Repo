# Test pyramid for sakuranotoki proxy_dll:
#   UT/      -- pure standalone unit tests
#   CT/      -- clean-checkout file-backed TSV component tests
#   SYSTEM/  -- black-box DLL load test and local translations.tsv check

add_subdirectory(UT)
add_subdirectory(CT)
add_subdirectory(SYSTEM)
