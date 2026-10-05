# clickhouse-wasm-columnar

A header-only C++23 library for writing ClickHouse WebAssembly UDFs that use the
columnar call interface (`ABI COLUMNAR_V1`).

With `COLUMNAR_V1`, ClickHouse sends a whole block of rows in one call. Each
column is one typed buffer, laid out the same way as the ClickHouse column in
memory. A constant column is sent once, not once per row. This library decodes
that frame, calls your function for each row, and encodes the result.

## Requirements

- ClickHouse with WASM UDFs and the `COLUMNAR_V1` ABI. Upstream ClickHouse does
  not have it yet. It is available in the
  [`bacek/wasm`](https://github.com/bacek/ClickHouse/tree/bacek/wasm) branch.
  The frame format is the one in ClickHouse's `src/Formats/ColumnBinaryWire.h`
  (see [PR #104424](https://github.com/ClickHouse/ClickHouse/pull/104424)).
- Emscripten, to build the module. Native builds are for unit tests only.

## Example

```cpp
#include <clickhouse_wasm/columnar.hpp>

static std::string repeat(std::string_view s, uint32_t n) {
    std::string out;
    for (uint32_t i = 0; i < n; ++i) out += s;
    return out;
}

CH_COLUMNAR_UDF(repeat_string, repeat)
```

```sql
INSERT INTO system.webassembly_modules (name, code)
VALUES ('mymodule', file('mymodule.wasm'));

CREATE FUNCTION repeat_string
LANGUAGE WASM FROM 'mymodule'
ARGUMENTS (s String, n UInt32) RETURNS String
ABI COLUMNAR_V1
DETERMINISTIC;

SELECT repeat_string('ab', 3);  -- ababab
```

`CH_COLUMNAR_UDF(name, fn)` exports `fn` as the WASM function `name`. The
argument and result types come from the C++ signature of `fn`.

## Types

| C++ argument type | ClickHouse column |
|---|---|
| `bool`, integers, `float`, `double` | `UInt*`, `Int*`, `Float*`, `Bool`, `Date*`, `Decimal32/64` |
| `std::string_view`, `std::string`, `std::span<const uint8_t>` | `String`, `FixedString(N)`, `LowCardinality(String)` |
| `std::span<const uint8_t>` | also `UUID`, `IPv4/6`, `Int128`, `Decimal128/256` (raw bytes) |
| `std::vector<T>` | `Array(T)` |

| C++ result type | ClickHouse column |
|---|---|
| `bool`, integers, `float`, `double` | the fixed-width type of the same size |
| `std::string` | `String` |
| `std::vector<T>`, `std::pair<A,B>`, `std::tuple<...>` | `Array(T)`, `Tuple(...)`, nested |

The wire format does not say whether a fixed-width column is signed or a float.
Declare the SQL argument type to match the C++ type. Narrower integer columns are
widened to the declared C++ type, so `n UInt32` accepts the literal `3`, which
ClickHouse sends as `UInt8`.

### NULL

If any argument of a row is NULL, your function is not called for that row. The
result for that row is NaN for floating-point results, `0` for other numbers, an
empty string for strings, and `T{}` for arrays and tuples.

## Your own types

Specialize `ch::bytes_codec<T>` to pass a type as a `String` value:

```cpp
template <> struct ch::bytes_codec<Point> {
    static Point decode(std::span<const uint8_t> bytes);  // argument, Array element
    static std::string encode(const Point& p);             // result: anything with data() and size()
    static bool is_null(const Point& p);                   // optional: write NULL / empty
};
```

Specialize `ch::column_reader<T>` to decode an argument from the column
yourself, for example to accept more than one wire layout:

```cpp
template <> struct ch::column_reader<Point> {
    static Point read(const ch::ColView& col, uint32_t row);
};
```

`column_reader` takes precedence over `bytes_codec` for arguments.

## Lower-level API

`columnar_call` is a thin layer over the frame API, and you can use that API
directly:

- `parse_columnar(buf)` checks the frame header and returns a `ColumnarBuf`.
  `ColumnarBuf::col(i)` checks one column descriptor against the frame and
  returns a `ColView`.
- `ColView` reads a row: `is_null`, `get_bytes`, `get_fixed<T>`,
  `get_fixed_bytes`, `lc_get_bytes`, `variant_offset_at`. It also has
  `is_const` and `is_effectively_const_bytes()`. Use these to do expensive work,
  such as parsing or indexing, once per call instead of once per row.
- `col_get_arg<T>(col, row)` is the typed reader that `columnar_call` uses.
- `write_result_column<Ret>(n, any_null, invoke)`, `ColBytesWriter`,
  `write_complex_col` and `col_write_fixed_header` build result frames.

A malformed frame calls `ch::panic`, which aborts the call with a ClickHouse
exception. It never reads outside the frame.

## Building

The library is header-only. It also has one source file, `src/abi.cpp`, with the
buffer exports that ClickHouse calls (`clickhouse_create_buffer`,
`clickhouse_destroy_buffer`, `clickhouse_reallocate_buffer`) and stubs for the
Emscripten runtime imports. Link it into the module exactly once.

```cmake
add_subdirectory(clickhouse-wasm-columnar)
add_executable(mymodule mymodule.cpp)
target_link_libraries(mymodule PRIVATE clickhouse_wasm::abi)
set_target_properties(mymodule PROPERTIES SUFFIX ".wasm")
target_compile_options(mymodule PRIVATE -fwasm-exceptions)
target_link_options(mymodule PRIVATE -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0
  -sNO_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -Wl,--no-entry -Wl,--allow-undefined)
```

ClickHouse's wasmtime runtime needs native WASM exceptions in the new `exnref`
form. Emscripten 3.x uses `-sWASM_EXNREF=1` for this.

Unit tests (native):

```sh
cmake -B build -G Ninja && ninja -C build && ./build/tests/columnar_tests
```

The frames in `tests/wire_fixtures/` were written by the ClickHouse host
serializer itself. To regenerate them, see `tests/regenerate_wire_fixtures.sh`.

## Users

- [chgeos](https://github.com/bacek/chgeos): PostGIS-style spatial functions on
  GEOS.

## License

Apache License 2.0. See [LICENSE](LICENSE).
