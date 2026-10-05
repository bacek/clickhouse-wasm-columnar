# COLUMNAR_V1 frame format

This is the byte layout that ClickHouse and a WebAssembly UDF exchange under
`ABI COLUMNAR_V1`. The same format is used in both directions: ClickHouse sends
one frame with the argument columns, and the function returns one frame with a
single result column.

[`include/clickhouse_wasm/wire.h`](include/clickhouse_wasm/wire.h) has every
constant and struct below as plain C. The frames in
[`tests/wire_fixtures/`](tests/wire_fixtures) were written by the ClickHouse
serializer and are the reference examples. If this document, `wire.h` and the
fixtures disagree, the fixtures win, and the disagreement is a bug.

## Conventions

- All integers are little-endian.
- Every offset is a byte offset from the start of the frame, including offsets
  inside embedded descriptors (Variant, LowCardinality).
- An offset of 0 means "absent". Offset 0 is always the frame header, so it is
  never a valid data position.
- `rows` is the frame's `num_rows` unless a section says otherwise.

## Frame

```
offset 0   frame header             16 bytes
offset 16  column descriptors       num_cols × 40 bytes
...        column data              anywhere after the descriptors
```

### Header

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `0x4E494243` (the bytes `CBIN`) |
| 4 | 2 | version | `1` |
| 6 | 2 | reserved | `0` |
| 8 | 4 | num_rows | rows in every column |
| 12 | 4 | num_cols | number of descriptors |

### Column descriptor

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | type: base tag in the low bits, OR'd with flags |
| 8 | 8 | null_offset |
| 16 | 8 | offsets_offset |
| 24 | 8 | data_offset |
| 32 | 8 | data_size |

### Flags

| Flag | Value | Meaning |
|---|---|---|
| `COL_IS_NULLABLE` | `0x20` | `null_offset` points to `u8[rows]`; 1 means NULL. |
| `COL_IS_CONST` | `0x80` | The column stores one row; it applies to every row. Read every section below with `rows = 1`. |

A reader must reject any other bit, and any base tag it does not know.

## Base tags

| Tag | Value | ClickHouse types | Layout |
|---|---|---|---|
| `COL_BYTES` | 0 | `String`, `FixedString(N)` | `offsets_offset`: `u64[rows+1]`, starting at 0. `data_offset`: the bytes. Row `i` is `data[offs[i] .. offs[i+1])`. |
| `COL_FIXED8` | 1 | 1-byte values | `data_offset`: `rows` values |
| `COL_FIXED16` | 2 | 2-byte values | as above |
| `COL_FIXED32` | 3 | 4-byte values, `Float32` | as above |
| `COL_FIXED64` | 4 | 8-byte values, `Float64` | as above |
| `COL_COMPLEX` | 5 | `Array`, `Tuple`, `Map`, nested `Nullable` | See [Complex](#complex). |
| `COL_VARIANT` | 6 | `Variant(...)` | See [Variant](#variant). |
| `COL_FIXEDN` | 7 | other fixed widths: `UUID`, `IPv6`, `(U)Int128/256`, `Decimal128/256`, `FixedString(N)` | `data_offset`: `rows` values. Width = `data_size / rows`. |
| `COL_LOWCARD` | 8 | `LowCardinality(T)` | See [LowCardinality](#lowcardinality). |

The fixed tags carry only a width. They do not say whether a value is signed,
a float, a date or a decimal; the declared SQL type of the function says that.
A width of 1, 2, 4 or 8 always uses `COL_FIXED8..64`, never `COL_FIXEDN`.

## Complex

`data_offset` points to a recursive block of `rows` elements. Its layout follows
the declared type:

| Type | Block of `n` elements |
|---|---|
| fixed width `T` | `T[n]` |
| `String` | `u64 offs[n+1]`, then the bytes |
| `Array(T)` | `u64 offs[n+1]` (`offs[0] = 0`, total `m = offs[n]`), then the block of `m` elements of `T` |
| `Tuple(T1, …, Tk)` | the blocks of `n` elements of `T1`, …, `Tk`, one after another |
| `Nullable(T)` | `u8 null_map[n]`, then the block of `n` elements of `T` |
| `Map(K, V)` | the same as `Array(Tuple(K, V))` |

Blocks follow each other with no padding.

A top-level `Nullable(Tuple(...))` sets `COL_IS_NULLABLE` on the descriptor and
puts the null map at `null_offset`; the block itself is the plain tuple.

## Variant

ClickHouse orders a Variant's alternatives by type name. The discriminator of a
value is its alternative's position in that order.

| Field | Contents |
|---|---|
| `null_offset` | `u8 discriminator[rows]`; `0xFF` means NULL |
| `offsets_offset` | `u32 row_offset[rows]`: the row's index inside its alternative |
| `data_offset` | `u32 K`, then `K` records |

Each record is 44 bytes: a `u8` discriminator, 3 bytes of padding, then a
40-byte descriptor of the alternative's column. In that embedded descriptor,
`null_offset` holds the alternative's row count, not an offset. The other
fields are frame offsets as usual, and the alternative's column is read with
that row count.

## LowCardinality

| Field | Contents |
|---|---|
| `null_offset` | always 0 |
| `offsets_offset` | `index[rows]`, each `width` bytes |
| `data_offset` | `u32 dict_rows`, `u8 width` (1, 2, 4 or 8), 3 bytes of padding, a 40-byte descriptor of the dictionary column |

Row `i` is dictionary row `index[i]`, read with `rows = dict_rows`. The
dictionary is any column except `COL_LOWCARD` or `COL_VARIANT`, and it never
sets `COL_IS_NULLABLE` or `COL_IS_CONST`. Slot 0 is ClickHouse's default value,
so `dict_rows >= 1`.

ClickHouse refuses `LowCardinality(Nullable(T))` arguments and
`LowCardinality` results when the function is created, so neither appears on
the wire.

## The result frame

The function returns a frame with `num_cols = 1` and `num_rows` equal to the
input's. The result column uses the same layout as an argument of the declared
return type, with these rules:

- `COL_IS_NULLABLE` must be set exactly when the declared type is
  `Nullable(T)`.
- `COL_IS_CONST` must not be set.
- A `Variant` result lists its alternatives in ClickHouse's order.

## Trust

ClickHouse validates every frame it reads from the module. A module should
treat the input frame as coming from a trusted host but still bounds-check
every read: the C++ library does, and calls `panic` (a ClickHouse exception) on
any offset, length or tag it does not accept. It never reads outside the frame.

## Versioning

`version` changes when the layout changes incompatibly. A reader must reject
any version it does not know. Library releases `v1.x` read and write frame
version 1.
