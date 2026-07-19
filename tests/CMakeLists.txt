add_executable(chronos_tests
  test_main.cpp
  test_simhash.cpp
  test_codex_alias.cpp
  test_mmr.cpp
  test_oracle_harness.cpp
)
target_link_libraries(chronos_tests PRIVATE chronos_core)
add_test(NAME chronos_tests COMMAND chronos_tests)
