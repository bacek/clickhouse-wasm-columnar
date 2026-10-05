# clickhouse-wasm-columnar (Rust)

Write ClickHouse WebAssembly UDFs in Rust with the columnar call interface
(`ABI COLUMNAR_V1`). This is the Rust twin of the C++ library in this repo. It
reads and writes the same frames ([SPEC.md](../SPEC.md)) and is checked against
the same fixtures and end-to-end tests.

## Example

```toml
[lib]
crate-type = ["cdylib"]

[dependencies]
clickhouse-wasm-columnar = { git = "https://github.com/bacek/clickhouse-wasm-columnar" }
```

```rust
use clickhouse_wasm_columnar::columnar_udf;

columnar_udf! {
    fn repeat_string(s: &str, n: u32) -> String { s.repeat(n as usize) }
}
```

```sh
cargo build --release --target wasm32-unknown-unknown
```

```sql
INSERT INTO system.webassembly_modules (name, code)
VALUES ('mymodule', file('mymodule.wasm'));

CREATE FUNCTION repeat_string LANGUAGE WASM FROM 'mymodule'
ARGUMENTS (s String, n UInt32) RETURNS String ABI COLUMNAR_V1 DETERMINISTIC;
```

`columnar_udf!` exports the function under its own name. It also defines a
module with the same name: `repeat_string::call(&[u8])` runs the function over
a frame, so you can test it natively without WebAssembly.

## Types

| Rust | ClickHouse |
|---|---|
| `bool`, `i8`…`i128`, `u8`…`u128`, `f32`, `f64` | `Bool`, `Int*`, `UInt*`, `Float*`, `Date*`, `Decimal32/64` |
| `[u8; N]` | any N-byte fixed type: `FixedString(N)`, `UUID`, `IPv6`, `Decimal128/256` |
| `&str`, `String` | `String`, `FixedString(N)` (must be UTF-8) |
| `&[u8]` (argument), `Bytes` (result) | `String`, `FixedString(N)` as raw bytes |
| `Vec<T>` | `Array(T)`, nested to any depth |
| tuples, up to 8 fields | `Tuple(...)` |
| `BTreeMap<K, V>`, `HashMap<K, V>` | `Map(K, V)` |
| `Option<T>` | `Nullable(T)`, also inside `Array`, `Tuple` and `Map` |
| enum from `column_variant!` | `Variant(...)` |
| `Result<T, E: Display>` (result) | `T`; an `Err` aborts the query with its message |

`LowCardinality(T)` arguments are read as T. Declare the result as plain T.

The wire gives only a column's width, not its type, so declare SQL types that
match the Rust types. Narrower integer arguments are widened, so `n UInt32`
accepts the literal `3`, which ClickHouse sends as `UInt8`. ClickHouse does not
convert types inside `Array`, `Tuple` and `Map` arguments: cast them, for
example `f([1, 2]::Array(Int64))`.

**NULL.** An `Option` argument receives NULL as `None`. If any other argument is
NULL, the function is not called for that row, and the result is NULL for an
`Option` result. Otherwise it is `0`, NaN, an empty string, or an empty
array/tuple/map, and NULL for a Variant. ClickHouse sends NULLs to the module
only if the function declares a `Nullable` argument.

**Variant.** List the alternatives in ClickHouse's order, which is by type name:

```rust
use clickhouse_wasm_columnar::column_variant;

column_variant! {
    pub enum IntOrStr { Int(i64), Str(String) }   // Variant(Int64, String)
}
```

## Panics and errors

A panic, an `Err` result, or a malformed frame aborts the query with a
ClickHouse exception carrying the message. The reader checks every offset
against the frame; it never reads outside it.

## Demo and tests

`demo/` exports the same functions as the C++ `examples/demo.cpp`. The same
`tests/e2e.py` checks both against a running server:

```sh
cargo test                                    # unit tests, host fixtures
cargo build --release --target wasm32-unknown-unknown --manifest-path demo/Cargo.toml
../tests/e2e.py --clickhouse /path/to/clickhouse --port 9000 \
  --wasm demo/target/wasm32-unknown-unknown/release/demo.wasm
```
