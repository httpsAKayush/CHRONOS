cmake_minimum_required(VERSION 3.20)
project(chronos LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Origin-relative install RPATH so installed binaries find the shared
# libraries next to them (bin/<exe> -> ../lib). MUST be set before any
# add_library/add_executable: CMake copies this into each target's
# INSTALL_RPATH property at target creation time.
set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

include(FetchContent)
FetchContent_Declare(
  libgit2
  GIT_REPOSITORY https://github.com/libgit2/libgit2.git
  GIT_TAG        v1.8.1
)
set(BUILD_TESTS OFF CACHE BOOL "Disable libgit2 tests" FORCE)
set(BUILD_CLI OFF CACHE BOOL "Disable libgit2 CLI" FORCE)
set(USE_SSH OFF CACHE BOOL "Disable libssh2" FORCE)
set(USE_HTTPS OFF CACHE BOOL "Disable HTTPS" FORCE)
FetchContent_MakeAvailable(libgit2)

# hnswlib for vector quantization and HNSW index
FetchContent_Declare(
  hnswlib
  GIT_REPOSITORY https://github.com/nmslib/hnswlib.git
  GIT_TAG        v0.8.0
)
FetchContent_MakeAvailable(hnswlib)

# nlohmann_json for LLM API parsing
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# cpp-httplib for communicating with local Ollama daemon
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.15.3
)
FetchContent_MakeAvailable(httplib)

# tree-sitter core + grammars are pulled in as a git submodule / system lib.
# We link against libtree-sitter and grammar libs if present; the code
# degrades to "parse_confidence = 0" nodes if a grammar lib is missing so
# the rest of the pipeline (Codex, PPR, IPC, Oracle) stays testable in isolation.
# User-local pip-installed grammars live in ~/.local/lib; search there too.
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")
find_library(TREE_SITTER_LIB tree-sitter HINTS "$ENV{HOME}/.local/lib")
find_library(TREE_SITTER_CPP_LIB tree-sitter-cpp HINTS "$ENV{HOME}/.local/lib")
find_library(TREE_SITTER_PYTHON_LIB tree-sitter-python HINTS "$ENV{HOME}/.local/lib")

find_package(Threads REQUIRED)

add_library(chronos_core
  src/infrastructure/storage/codex.cpp
  src/domain/simhash.cpp
  src/domain/ppr.cpp
  src/infrastructure/storage/vector_index.cpp
  src/domain/ast_indexer.cpp
  src/domain/ast_mutation_scorer.cpp
  src/daemon/ipc.cpp
  src/use_cases/context_builder.cpp
  src/use_cases/oracle.cpp
  src/domain/mmr.cpp
  src/domain/rrf.cpp
  src/infrastructure/vcs/git_indexer.cpp
  src/use_cases/diagnose.cpp
  src/infrastructure/llm/llm_config.cpp
  src/infrastructure/config.cpp
  src/infrastructure/llm/llm_client_factory.cpp
  src/infrastructure/llm/ollama_client.cpp
  src/infrastructure/llm/openai_client.cpp
  src/infrastructure/llm/ipc_llm_client.cpp
  src/use_cases/repo_profiler.cpp
  src/use_cases/hierarchical_summarizer.cpp
  src/use_cases/ask_engine.cpp
  src/cli/cli_util.cpp
)

target_include_directories(chronos_core PUBLIC include ${SQLITE3_INCLUDE_DIRS} ${libgit2_SOURCE_DIR}/include ${hnswlib_SOURCE_DIR})
target_link_libraries(chronos_core PUBLIC ${SQLITE3_LIBRARIES} Threads::Threads libgit2package hnswlib nlohmann_json::nlohmann_json httplib::httplib)

if(TREE_SITTER_LIB)
  # The tree-sitter core runtime is needed by every grammar (including the
  # always-built JavaScript/CSS parsers below), so link it whenever found.
  target_link_libraries(chronos_core PUBLIC ${TREE_SITTER_LIB})
  if(TREE_SITTER_CPP_LIB)
    target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_CPP=1)
    target_link_libraries(chronos_core PUBLIC ${TREE_SITTER_CPP_LIB})
  endif()
  if(TREE_SITTER_PYTHON_LIB)
    target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_PYTHON=1)
    target_link_libraries(chronos_core PUBLIC ${TREE_SITTER_PYTHON_LIB})
  endif()
endif()

# Add JavaScript Parser
add_library(tree-sitter-javascript STATIC external/tree-sitter-javascript/src/parser.c external/tree-sitter-javascript/src/scanner.c)
target_include_directories(tree-sitter-javascript PRIVATE external/tree-sitter-javascript/src)
target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_JAVASCRIPT=1)

# Add CSS Parser
add_library(tree-sitter-css STATIC external/tree-sitter-css/src/parser.c external/tree-sitter-css/src/scanner.c)
target_include_directories(tree-sitter-css PRIVATE external/tree-sitter-css/src)
target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_CSS=1)

target_link_libraries(chronos_core PUBLIC tree-sitter-javascript tree-sitter-css)

# --- Indexer entrypoint (invoked by the git hook, fail-open, async worker) ---
add_executable(chronos-indexer src/daemon/main_indexer.cpp)
target_link_libraries(chronos-indexer PRIVATE chronos_core)

# --- Daemon: owns the AF_UNIX socket + lazy LLM execution tier ---
add_executable(chronos-daemon src/daemon/main_daemon.cpp)
target_link_libraries(chronos-daemon PRIVATE chronos_core)

# --- CLI/TUI querier ---
add_executable(chronos
  src/cli/main_cli.cpp
  src/cli/commands/cmd_init.cpp
  src/cli/commands/cmd_sync.cpp
  src/cli/commands/cmd_ask.cpp
  src/cli/commands/cmd_explain.cpp
  src/cli/commands/cmd_map.cpp
  src/cli/commands/cmd_diagnose.cpp
  src/cli/commands/cmd_trace.cpp
  src/cli/commands/cmd_timeline.cpp
  src/cli/commands/cmd_check_staging.cpp
  src/cli/commands/cmd_status.cpp
  src/cli/commands/cmd_config.cpp
  src/cli/commands/cmd_export.cpp
  src/cli/commands/cmd_clean.cpp
)
target_link_libraries(chronos PRIVATE chronos_core)

enable_testing()
add_subdirectory(tests)

# --- Installation ---
# Relative DESTINATIONs resolve against CMAKE_INSTALL_PREFIX (default
# /usr/local) and also honor `cmake --install <dir> --prefix <p>` staging.
install(TARGETS chronos chronos-daemon chronos-indexer DESTINATION bin)
install(TARGETS chronos_core LIBRARY DESTINATION lib)
install(FILES ${libgit2_BINARY_DIR}/libgit2.so.1.8 DESTINATION lib)
install(PROGRAMS scripts/pre-commit DESTINATION bin/scripts)
