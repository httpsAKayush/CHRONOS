cmake_minimum_required(VERSION 3.20)
project(chronos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# tree-sitter core + C++ grammar are pulled in as a git submodule / system lib.
# We link against libtree-sitter and libtree-sitter-cpp if present; the code
# degrades to "parse_confidence = 0" nodes if the grammar lib is missing so
# the rest of the pipeline (Codex, PPR, IPC, Oracle) stays testable in isolation.
find_library(TREE_SITTER_LIB tree-sitter)
find_library(TREE_SITTER_CPP_LIB tree-sitter-cpp)
find_library(TREE_SITTER_PYTHON_LIB tree-sitter-python)

find_package(Threads REQUIRED)

add_library(chronos_core
  src/codex.cpp
  src/simhash.cpp
  src/ppr.cpp
  src/vector_index.cpp
  src/ast_indexer.cpp
  src/ipc.cpp
  src/context_builder.cpp
  src/oracle.cpp
  src/mmr.cpp
  src/rrf.cpp
  src/git_indexer.cpp
)

target_include_directories(chronos_core PUBLIC include ${SQLITE3_INCLUDE_DIRS} ${libgit2_SOURCE_DIR}/include)
target_link_libraries(chronos_core PUBLIC ${SQLITE3_LIBRARIES} Threads::Threads libgit2package)

if(TREE_SITTER_LIB)
  if(TREE_SITTER_CPP_LIB)
    target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_CPP=1)
    target_link_libraries(chronos_core PUBLIC ${TREE_SITTER_LIB} ${TREE_SITTER_CPP_LIB})
  endif()
  if(TREE_SITTER_PYTHON_LIB)
    target_compile_definitions(chronos_core PUBLIC CHRONOS_HAVE_TREE_SITTER_PYTHON=1)
    target_link_libraries(chronos_core PUBLIC ${TREE_SITTER_LIB} ${TREE_SITTER_PYTHON_LIB})
  endif()
endif()

# --- Indexer entrypoint (invoked by the git hook, fail-open, async worker) ---
add_executable(chronos-indexer src/main_indexer.cpp)
target_link_libraries(chronos-indexer PRIVATE chronos_core)

# --- Daemon: owns the AF_UNIX socket + lazy LLM execution tier ---
add_executable(chronos-daemon src/main_daemon.cpp)
target_link_libraries(chronos-daemon PRIVATE chronos_core)

# --- CLI/TUI querier ---
add_executable(chronos src/main_cli.cpp)
target_link_libraries(chronos PRIVATE chronos_core)

enable_testing()
add_subdirectory(tests)
