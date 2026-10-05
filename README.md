# clickhouse-wasm-columnar

A header-only C++23 library for writing ClickHouse WebAssembly UDFs that use the
columnar call interface (`ABI COLUMNAR_V1`).

With `COLUMNAR_V1`, ClickHouse sends a whole block of rows in one call. Each
column is one typed buffer, laid out the same way as the ClickHouse column in
memory. A constant column is sent once, not once per row. This library decodes
that frame, calls your function for each row, and encodes the result.

Writing Rust instead? See [rust/](rust/README.md): same frames, same tests.

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

Arguments and results use the same C++ types:

| C++ type | ClickHouse type |
|---|---|
| `bool`, integers, `float`, `double` | `Bool`, `UInt*`, `Int*`, `Float*`, `Date*`, `Decimal32/64` |
| `std::array<uint8_t, N>` | any N-byte fixed type: `FixedString(N)`, `UUID`, `IPv6`, `Int128`, `Decimal128/256` |
| `std::string`, `std::string_view` (argument only), `std::span<const uint8_t>` (argument only) | `String`, `FixedString(N)` |
| `std::vector<T>` | `Array(T)`, nested to any depth |
| `std::pair<A, B>`, `std::tuple<...>` | `Tuple(...)` |
| `std::map<K, V>` | `Map(K, V)` |
| `std::optional<T>` | `Nullable(T)`, also inside `Array`, `Tuple` and `Map` |
| `std::variant<Ts...>` | `Variant(...)` |
| `bytes_codec` types | `String` (see below) |

`LowCardinality(T)` arguments arrive as their dictionary and are read as T.
Declare the SQL result as plain T; the library writes it without a dictionary.

The wire format does not say whether a fixed-width column is signed or a float.
Declare the SQL argument type to match the C++ type. Narrower integer columns are
widened to the declared C++ type, so `n UInt32` accepts the literal `3`, which
ClickHouse sends as `UInt8`. ClickHouse does not convert types inside `Array`,
`Tuple` and `Map` arguments: cast them to the declared type, for example
`f([1, 2]::Array(Int64))`.

### Variant

ClickHouse orders the alternatives of a `Variant` by type name, and the
discriminator is the position in that order. Write the `std::variant`
alternatives in the same order: `Variant(String, Int64)` is
`std::variant<int64_t, std::string>` (`Int64` sorts before `String`).

### NULL

A `std::optional` argument receives NULL as `std::nullopt`. If any other argument
of a row is NULL, your function is not called for that row. With a
`std::optional<T>` result, that row is NULL, and so is every row where your
function returns `std::nullopt`; declare the SQL result as `Nullable(T)`. Without
`std::optional`, the result for that row is NaN for floating-point results, `0`
for other numbers, an empty string for strings, `T{}` for arrays, tuples and
maps, and NULL for a `std::variant`.

How a NULL argument reaches the module depends on the ABI and on whether the
function declares a `Nullable` argument:

| ABI | Declared arguments | NULL rows |
|---|---|---|
| `COLUMNAR_V1` | at least one `Nullable(T)` | sent to the module, with the null map set; the module decides the result |
| `COLUMNAR_V1` | no `Nullable` | not sent; ClickHouse returns NULL for that row itself |
| `BUFFERED_V1` and older ABIs | any | not sent; ClickHouse returns NULL for that row itself |

Where ClickHouse returns NULL itself, a result type that cannot be `Nullable`
(`Array`, `Tuple`, `Map`) is the exception: the row is sent with the default
value in place of the NULL, and the module's result is returned.

ClickHouse has no `Nullable(Array)` or `Nullable(Map)`, so `std::optional` of a
vector or map is not a valid result type; `Nullable(Tuple)` is.

## Wire format

[SPEC.md](SPEC.md) describes the frame byte by byte.
[`clickhouse_wasm/wire.h`](include/clickhouse_wasm/wire.h) has its constants and
structs as plain C, for modules in other languages. Releases `v1.x` of this
library use frame version 1.

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

`examples/demo.cpp` has one function for each supported shape (strings,
`Nullable`, nested arrays, tuples, maps, `Variant`, `LowCardinality`, `UUID`,
errors) and `examples/demo.sql` registers them. Build it with Emscripten:

```sh
emcmake cmake -B build_wasm -G Ninja && ninja -C build_wasm   # build_wasm/examples/demo.wasm
```

`tests/e2e.py` loads the module into a running server, checks every function,
and removes them again:

```sh
tests/e2e.py --clickhouse /path/to/clickhouse --wasm build_wasm/examples/demo.wasm --port 9000
```

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
