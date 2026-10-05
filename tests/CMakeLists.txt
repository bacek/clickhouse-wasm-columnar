find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(columnar_tests
  host_stubs.cpp
  test_frame.cpp
  test_udf.cpp
)
target_link_libraries(columnar_tests PRIVATE clickhouse_wasm_abi GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(columnar_tests)
