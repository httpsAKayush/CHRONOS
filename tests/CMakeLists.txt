add_executable(chronos_tests
  test_main.cpp
  test_simhash.cpp
  test_codex_alias.cpp
  test_mmr.cpp
  test_oracle_harness.cpp
  test_recency.cpp
  test_ast_mutation_scorer.cpp
  test_staging_check.cpp
  test_vector_quantization_tiering.cpp
  test_core_interfaces.cpp
)
target_link_libraries(chronos_tests PRIVATE chronos_core)
add_test(NAME chronos_tests COMMAND chronos_tests)

add_executable(test_mock_storage test_mock_storage.cpp)
target_include_directories(test_mock_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_mock_storage PRIVATE chronos_core)
add_test(NAME test_mock_storage COMMAND test_mock_storage)

