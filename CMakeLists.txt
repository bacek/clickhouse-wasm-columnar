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

option(CLICKHOUSE_WASM_COLUMNAR_TESTS "Build the native unit tests" ${PROJECT_IS_TOP_LEVEL})

if(CLICKHOUSE_WASM_COLUMNAR_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()
