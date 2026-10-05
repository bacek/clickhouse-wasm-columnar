if(NOT EMSCRIPTEN)
  message(STATUS "clickhouse-wasm-columnar: examples need Emscripten (emcmake cmake ...); skipped")
  return()
endif()

# ClickHouse's wasmtime runtime needs native WASM exceptions in exnref form.
# Emscripten 3.x spells the switch -sWASM_EXNREF=1.
set(CWC_EXCEPTIONS -fexceptions -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0)

add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE clickhouse_wasm_abi)
set_target_properties(demo PROPERTIES SUFFIX ".wasm")
target_compile_options(demo PRIVATE ${CWC_EXCEPTIONS})
target_link_options(demo PRIVATE ${CWC_EXCEPTIONS}
  -sNO_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1
  -Wl,--no-entry -Wl,--allow-undefined)
target_compile_options(clickhouse_wasm_abi PRIVATE ${CWC_EXCEPTIONS})
