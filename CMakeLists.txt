cmake_minimum_required(VERSION 3.21)
project(clickhouse_wasm_columnar LANGUAGES CXX)

# Header-only wire format and UDF helpers.
add_library(clickhouse_wasm_columnar INTERFACE)
add_library(clickhouse_wasm::columnar ALIAS clickhouse_wasm_columnar)
target_include_directories(clickhouse_wasm_columnar INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(clickhouse_wasm_columnar INTERFACE cxx_std_23)

# Buffer management exports (clickhouse_create_buffer & co.) and the
# Emscripten runtime stubs.  Link exactly one copy into each module.
add_library(clickhouse_wasm_abi STATIC src/abi.cpp)
add_library(clickhouse_wasm::abi ALIAS clickhouse_wasm_abi)
target_link_libraries(clickhouse_wasm_abi PUBLIC clickhouse_wasm_columnar)

if(EMSCRIPTEN)
  # Newer Emscripten (LLD >= 21) links libstandalonewasm's standalone.o, which
  # defines __throw_exception_with_stack_trace strongly; src/abi.cpp defines it
  # too, to route an escaped exception to clickhouse_throw instead of a JS
  # import ClickHouse does not provide.  Our definition comes first in link
  # order, so let the linker keep it.
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-Wl,--allow-multiple-definition" CWC_HAS_ALLOW_MULTIDEF)
  if(CWC_HAS_ALLOW_MULTIDEF)
    target_link_options(clickhouse_wasm_abi INTERFACE "-Wl,--allow-multiple-definition")
  endif()
endif()

option(CLICKHOUSE_WASM_COLUMNAR_TESTS "Build the native unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CLICKHOUSE_WASM_COLUMNAR_EXAMPLES "Build the example module (Emscripten only)" ${PROJECT_IS_TOP_LEVEL})

if(CLICKHOUSE_WASM_COLUMNAR_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CLICKHOUSE_WASM_COLUMNAR_EXAMPLES)
  add_subdirectory(examples)
endif()
