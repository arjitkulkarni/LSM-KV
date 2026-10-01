cmake_minimum_required(VERSION 3.20)

project(lsmkv
  VERSION 1.0.0
  DESCRIPTION "A crash-tested, observable LSM-tree key-value storage engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LSMKV_BUILD_TESTS "Build the GoogleTest suite" ON)
option(LSMKV_BUILD_BENCH "Build the benchmark harness" ON)
option(LSMKV_BUILD_TOOLS "Build lsmkv-dump and lsmkv-stress" ON)
option(LSMKV_WITH_LEVELDB "Fetch LevelDB and build the comparison adapter" OFF)
option(LSMKV_WERROR "Treat warnings as errors" OFF)
set(LSMKV_SANITIZE "" CACHE STRING "Sanitizers: address;undefined | thread | (empty)")

find_package(Threads REQUIRED)

# MinGW: link the C++ runtime statically so tools and benchmarks run from a
# plain shell without the toolchain's DLLs on PATH.
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_link_options(-static)
endif()
# Clang targeting MinGW defaults to native TLS, but links against a GCC-built
# runtime that uses emulated TLS; thread_local then faults at first access.
# Match GCC's model.
if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-femulated-tls)
endif()

# ---- Warnings & sanitizers ---------------------------------------------------
add_library(lsmkv_compile_flags INTERFACE)
if(MSVC)
  target_compile_options(lsmkv_compile_flags INTERFACE /W4 /permissive-)
  if(LSMKV_WERROR)
    target_compile_options(lsmkv_compile_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(lsmkv_compile_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference
    -Wimplicit-fallthrough)
  if(LSMKV_WERROR)
    target_compile_options(lsmkv_compile_flags INTERFACE -Werror)
  endif()
  if(LSMKV_SANITIZE)
    string(REPLACE ";" "," _san "${LSMKV_SANITIZE}")
    target_compile_options(lsmkv_compile_flags INTERFACE
      -fsanitize=${_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(lsmkv_compile_flags INTERFACE -fsanitize=${_san})
  endif()
endif()

# Record the exact toolchain so benchmark output can print it.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt_upper)
set(LSMKV_BUILD_FLAGS "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | ${CMAKE_BUILD_TYPE} | ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt_upper}}")

# ---- The engine --------------------------------------------------------------
set(LSMKV_SOURCES
  src/db/builder.cc
  src/db/db_impl.cc
  src/db/db_iter.cc
  src/db/dbformat.cc
  src/db/filename.cc
  src/db/table_cache.cc
  src/db/version_edit.cc
  src/db/version_set.cc
  src/db/write_batch.cc
  src/memtable/memtable.cc
  src/server/metrics_http_server.cc
  src/table/block.cc
  src/table/block_builder.cc
  src/table/format.cc
  src/table/merger.cc
  src/table/table.cc
  src/table/table_builder.cc
  src/table/two_level_iterator.cc
  src/util/arena.cc
  src/util/bloom.cc
  src/util/cache.cc
  src/util/coding.cc
  src/util/comparator.cc
  src/util/crc32c.cc
  src/util/env.cc
  src/util/fault_injection_env.cc
  src/util/hash.cc
  src/util/histogram.cc
  src/util/iterator.cc
  src/util/logger.cc
  src/util/metrics.cc
  src/util/status.cc
  src/wal/log_reader.cc
  src/wal/log_writer.cc
)
if(WIN32)
  list(APPEND LSMKV_SOURCES src/util/env_windows.cc)
else()
  list(APPEND LSMKV_SOURCES src/util/env_posix.cc)
endif()

add_library(lsmkv STATIC ${LSMKV_SOURCES})
target_include_directories(lsmkv
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsmkv PUBLIC Threads::Threads PRIVATE lsmkv_compile_flags)
if(WIN32)
  target_link_libraries(lsmkv PUBLIC ws2_32)
endif()
target_compile_definitions(lsmkv PUBLIC LSMKV_BUILD_FLAGS="${LSMKV_BUILD_FLAGS}")

# Internal headers (src/) are visible to tests, tools and benchmarks: they
# exercise components (skiplist, WAL, blocks) directly, not just the DB API.
add_library(lsmkv_internal INTERFACE)
target_include_directories(lsmkv_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lsmkv_internal INTERFACE lsmkv lsmkv_compile_flags)

# ---- Tools -----------------------------------------------------------------------
if(LSMKV_BUILD_TOOLS)
  add_executable(lsmkv-dump tools/lsmkv_dump.cc)
  target_link_libraries(lsmkv-dump PRIVATE lsmkv_internal)
  add_executable(lsmkv-stress tools/lsmkv_stress.cc)
  target_link_libraries(lsmkv-stress PRIVATE lsmkv_internal)
endif()

# ---- Benchmarks -------------------------------------------------------------------
if(LSMKV_BUILD_BENCH)
  set(_bench_sources bench/lsmkv_bench.cc bench/ycsb.cc bench/sysinfo.cc)
  add_executable(lsmkv-bench ${_bench_sources})
  target_include_directories(lsmkv-bench PRIVATE bench)
  target_link_libraries(lsmkv-bench PRIVATE lsmkv_internal)

  add_executable(micro-bench bench/micro_bench.cc bench/sysinfo.cc)
  target_include_directories(micro-bench PRIVATE bench)
  target_link_libraries(micro-bench PRIVATE lsmkv_internal)

  add_executable(bloom-sweep bench/bloom_sweep.cc bench/sysinfo.cc)
  target_include_directories(bloom-sweep PRIVATE bench)
  target_link_libraries(bloom-sweep PRIVATE lsmkv_internal)

  if(LSMKV_WITH_LEVELDB)
    include(FetchContent)
    set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(leveldb
      URL https://github.com/google/leveldb/archive/refs/tags/1.23.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(leveldb)
    target_compile_definitions(lsmkv-bench PRIVATE LSMKV_WITH_LEVELDB=1)
    target_link_libraries(lsmkv-bench PRIVATE leveldb)
  endif()
endif()

# ---- Tests ------------------------------------------------------------------------
if(LSMKV_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  set(LSMKV_TESTS
    arena_test
    bloom_test
    cache_test
    coding_test
    crc32c_test
    db_test
    db_concurrency_test
    db_differential_test
    fault_injection_test
    format_test
    histogram_test
    log_test
    memtable_test
    merger_test
    metrics_test
    recovery_test
    skiplist_test
    table_test
    version_edit_test
    write_batch_test
  )
  foreach(t ${LSMKV_TESTS})
    add_executable(${t} tests/${t}.cc tests/test_util.cc)
    target_include_directories(${t} PRIVATE tests)
    target_link_libraries(${t} PRIVATE lsmkv_internal GTest::gtest_main)
    gtest_discover_tests(${t} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endforeach()
endif()
