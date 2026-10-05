#pragma once

// COLUMNAR_V1 wire format for ClickHouse WASM UDFs.
//
// Replaces RowBinary with a columnar layout.  Key benefit: ColumnConst data
// (e.g. a constant 169 KB polygon) is passed ONCE regardless of num_rows.
//
// ┌──────────────────────────────────────────────────────────────────────────┐
// │ FrameHeader (16 bytes)                                                   │
// │   magic    : u32  — 'C','B','I','N'                                      │
// │   version  : u16  — 1                                                    │
// │   reserved : u16                                                         │
// │   num_rows : u32                                                         │
// │   num_cols : u32                                                         │
// ├──────────────────────────────────────────────────────────────────────────┤
// │ ColDescriptor[num_cols] (40 bytes each)                                  │
// │   type           : u64  — ColType | COL_IS_CONST flag                   │
// │   null_offset    : u64  — offset to u8[row_count] null map; 0=no nulls  │
// │   offsets_offset : u64  — offset to u64[row_count+1] start offsets;     │
// │                           0 for fixed-width columns                      │
// │   data_offset    : u64  — offset to raw column data                     │
// │   data_size      : u64  — total bytes in the data block                 │
// ├──────────────────────────────────────────────────────────────────────────┤
// │ Data blocks at offsets described above                                   │
// └──────────────────────────────────────────────────────────────────────────┘
//
// Offsets (COL_BYTES, nullable or not):
//   offsets[0..row_count] are start-based (offsets[0]=0).
//   No null terminators on the wire. ColumnString has no null terminators internally
//   (see ColumnString.h); the wire matches exactly.
//   String i bytes: data[offsets[i] .. offsets[i+1]-1], len = offsets[i+1]-offsets[i].
//
// SQL: ABI COLUMNAR_V1  (no serialization_format needed)

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "abi.hpp"

namespace ch {

// ── Customization points ─────────────────────────────────────────────────────
//
// bytes_codec<T>: lets a user type travel as a String/FixedString value —
// as an argument, an Array element, or a result.  Specialize with any of:
//   static T    decode(std::span<const uint8_t>);   // argument / Array element
//   static auto encode(const T&);                   // result: has .data(), .size()
//   static bool is_null(const T&);                  // result: emit NULL / empty
//
// column_reader<T>: full control over decoding an argument of type T from a
// column, e.g. to accept more than one wire shape.  Specialize with
//   static T read(const struct ColView&, uint32_t row);
// It takes precedence over bytes_codec<T>::decode for arguments.
//
// Specializations must be visible before the first instantiation that uses T.

template <typename T> struct bytes_codec;
template <typename T> struct column_reader;

template <typename T>
concept BytesDecodable = requires(std::span<const uint8_t> s) {
    { bytes_codec<T>::decode(s) } -> std::same_as<T>;
};

template <typename T>
concept BytesEncodable = requires(const T& v) {
    bytes_codec<T>::encode(v).data();
    bytes_codec<T>::encode(v).size();
};

template <typename T>
bool bytes_value_is_null(const T& v) {
    if constexpr (requires { { bytes_codec<T>::is_null(v) } -> std::convertible_to<bool>; })
        return bytes_codec<T>::is_null(v);
    else
        return false;
}

// ── Type tags ────────────────────────────────────────────────────────────────

enum ColType : uint32_t {
    COL_BYTES       = 0,  // String:            offsets[row_count+1] + data
    COL_FIXED8      = 1,  // UInt8/Int8:        u8[row_count]
    COL_FIXED16     = 2,  // UInt16/Int16:      u16[row_count]
    COL_FIXED32     = 3,  // UInt32/Int32/Float32
    COL_FIXED64     = 4,  // UInt64/Int64/Float64
    // COL_COMPLEX: generic Array(T) / Tuple(T...) — type-guided recursive format.
    // offsets_offset → uint64[N+1] outer offsets (for Array rows; 0 for Tuple/scalar).
    // data_offset    → recursive data block (layout determined by C++/CH declared type).
    // Recursive layout per type:
    //   scalar T:           T[N]  (packed, fixed width)
    //   String:             uint64[N+1] offsets + bytes (no null terminator, same as COL_BYTES)
    //   vector<T> (Array):  uint64[N+1] outer_offsets → M total, then recursive(M, T)
    //   pair/tuple (Tuple): recursive(N, T0) ++ recursive(N, T1) ++ ...  (columnar)
    COL_COMPLEX     = 5,
    COL_VARIANT     = 6,  // Variant(...): disc[N](u8) + row_offs[N](u32) + header{K, records} + sub-data
    // Fixed-width payload whose width is not 1/2/4/8: UUID, IPv6, Int128/UInt128,
    // Decimal128/256, FixedString(N) for N ∉ {1,2,4,8}.  No offsets array; the
    // element width is recorded only as data_size / row_count.  Widths 1/2/4/8 —
    // including FixedString(8) — are classed into COL_FIXED8/16/32/64 by the host,
    // so signedness and logical type never reach the wire: interpretation comes
    // from the declared C++ side, the tag only validates the width.
    COL_FIXEDN      = 7,
    // Top-level LowCardinality(T): offsets_offset → index[num_rows] of
    // index_elem_width ∈ {1,2,4,8} bytes; data_offset → uint32 dict_row_count,
    // uint8 index_elem_width, pad[3], an embedded ColDescriptor for the
    // dictionary, then the dictionary sub-column.  null_offset is always 0:
    // for LowCardinality(Nullable(T)) NULL rides on dictionary slot 0 and the
    // wire is byte-identical to a plain LC whose dictionary starts with the
    // default value, so this guest only ever decodes the non-nullable shape —
    // any COL_IS_NULLABLE bit on a COL_LOWCARD descriptor is rejected, and the
    // nullable form must be refused host-side before it reaches the wire.
    COL_LOWCARD     = 8,

    // Flags — OR'd onto base type.  Bits 5 and 7 as defined by the host; bits
    // outside the two flags and the known base tags are rejected by
    // ColumnarBuf::col rather than passed through.
    COL_IS_NULLABLE = 0x20u, // Nullable(T): null_offset carries u8[row_count] null map
    COL_IS_CONST    = 0x80u, // 1 stored row, broadcast to num_rows
};

// ── Type traits for complex (Array/Tuple) C++ types ─────────────────────────

template <typename T> struct is_vector_t      : std::false_type {};
template <typename T> struct is_vector_t<std::vector<T>> : std::true_type {};
template <typename T> inline constexpr bool is_vector_v = is_vector_t<T>::value;

template <typename T> struct is_pair_t        : std::false_type {};
template <typename A, typename B> struct is_pair_t<std::pair<A,B>> : std::true_type {};
template <typename T> inline constexpr bool is_pair_v = is_pair_t<T>::value;

template <typename T> struct is_tuple_t       : std::false_type {};
template <typename... Ts> struct is_tuple_t<std::tuple<Ts...>> : std::true_type {};
template <typename T> inline constexpr bool is_tuple_v = is_tuple_t<T>::value;

template <typename T> struct is_optional_t    : std::false_type {};
template <typename T> struct is_optional_t<std::optional<T>> : std::true_type {};
template <typename T> inline constexpr bool is_optional_v = is_optional_t<T>::value;

template <typename T> inline constexpr bool is_complex_v =
    is_vector_v<T> || is_pair_v<T> || is_tuple_v<T>;

// ── Wire structs ──────────────────────────────────────────────────────────────

struct ColDescriptor {
    uint64_t type;
    uint64_t null_offset;
    uint64_t offsets_offset;
    uint64_t data_offset;
    uint64_t data_size;
};
static_assert(sizeof(ColDescriptor) == 40);

// Frame header, matching ClickHouse's ColumnBinaryWire.h:
// [4 B magic | 2 B version | 2 B reserved | 4 B num_rows | 4 B num_cols]
static constexpr uint32_t FRAME_MAGIC   = 0x4E494243u;  // 'C' | 'B'<<8 | 'I'<<16 | 'N'<<24
static constexpr uint16_t FRAME_VERSION = 1;
static constexpr uint32_t HEADER_BYTES  = 16;  // sizeof FrameHeader
static constexpr uint32_t COL_DESC_BYTES = 40;  // sizeof ColDescriptor

// Write the frame header at the start of an output buffer.
inline void write_frame_header(uint8_t* p, uint32_t num_rows, uint32_t num_cols) {
    const uint16_t reserved = 0;
    std::memcpy(p,      &FRAME_MAGIC,   4);
    std::memcpy(p + 4,  &FRAME_VERSION, 2);
    std::memcpy(p + 6,  &reserved,      2);
    std::memcpy(p + 8,  &num_rows,      4);
    std::memcpy(p + 12, &num_cols,      4);
}

// ── Input column accessor ─────────────────────────────────────────────────────

struct ColView {
    ColType         base_type;
    bool            is_const;
    uint32_t        row_count;    // stored rows (1 if const, N otherwise)
    const uint8_t*  null_map;     // nullable: null_map[i]!=0 → NULL;
                                  // COL_VARIANT: discriminators (0xFF = NULL)
    const uint64_t* offsets;      // start-based; nullptr for fixed-width
    const uint8_t*  data;
    uint64_t        data_size;    // descriptor's data blob size
    uint32_t        fixed_width;  // COL_FIXED8/16/32/64: tag width;
                                  // COL_FIXEDN: data_size / row_count (0 if row_count == 0)
    const uint8_t*  base;         // buffer base — needed for absolute offset navigation

    // COL_LOWCARD only: the shared index array and the dictionary sub-column's
    // descriptor (absolute offsets into the same frame).  Populated by
    // ColumnarBuf::col once the header and extents are validated.
    const uint8_t*  lc_index;      // index[row_count], lc_index_width bytes each
    uint8_t         lc_index_width;
    uint32_t        lc_dict_rows;
    ColDescriptor   lc_dict;

    // Map logical row to stored row index.
    uint32_t effective_row(uint32_t row) const noexcept {
        if (is_const) return 0u;
        return row;
    }

    bool is_null(uint32_t row) const noexcept {
        if (!null_map) return false;
        const uint8_t v = null_map[effective_row(row)];
        if (base_type == COL_VARIANT) return v == 0xFFu;
        return v != 0u;
    }

    // For COL_BYTES — exact byte span, no null terminator on wire.
    std::span<const uint8_t> get_bytes(uint32_t row) const noexcept {
        uint32_t idx   = effective_row(row);
        uint64_t start = offsets[idx];
        uint64_t end   = offsets[idx + 1];
        uint64_t len   = end - start;
        return {data + start, static_cast<size_t>(len)};
    }

    // Fixed-width row bytes (COL_FIXED8/16/32/64 via fixed_width, and
    // COL_FIXEDN).  The tag never says what the bytes mean — width only.
    std::span<const uint8_t> get_fixed_bytes(uint32_t row) const {
        if (fixed_width == 0u)
            panic("columnar: fixed-width read of a column with no width");
        uint32_t idx = effective_row(row);
        uint64_t off = uint64_t(idx) * fixed_width;
        if (off + fixed_width > data_size)
            panic("columnar: fixed-width row extends past the column data");
        return {data + off, fixed_width};
    }

    // COL_VARIANT only: the row's position within its sub-column. The array at
    // offsets_offset is uint32[row_count] — the host's writer stores uint32 there
    // (ColumnBinaryWire.h, Variant branch of writeColData) and its reader loads
    // uint32 back; it is NOT the uint64 offsets array COL_BYTES uses. Reading the
    // u64 `offsets` pointer over this array reads pairs of row offsets as one
    // (wrong) position per row.
    uint32_t variant_offset_at(uint32_t row) const {
        uint32_t idx = effective_row(row);
        uint32_t v;
        std::memcpy(&v, reinterpret_cast<const uint8_t*>(offsets) + uint64_t(idx) * sizeof(uint32_t), sizeof(uint32_t));
        return v;
    }

    // COL_LOWCARD: the shared index value for a row, read at its wire width.
    uint64_t lc_index_at(uint32_t row) const {
        uint32_t idx = effective_row(row);
        const uint8_t* p = lc_index + uint64_t(idx) * lc_index_width;
        uint64_t v = 0;
        std::memcpy(&v, p, lc_index_width);
        return v;
    }

    // COL_LOWCARD with a COL_BYTES dictionary: the row's value bytes.
    std::span<const uint8_t> lc_get_bytes(uint32_t row) const {
        uint64_t e = lc_index_at(row);
        if (e >= lc_dict_rows)
            panic("columnar: LowCardinality index exceeds dictionary");
        const uint64_t* dict_offs =
            reinterpret_cast<const uint64_t*>(base + lc_dict.offsets_offset);
        uint64_t start = dict_offs[e];
        uint64_t end   = dict_offs[e + 1];
        if (end > lc_dict.data_size)
            panic("columnar: LowCardinality dictionary offsets exceed data");
        return {base + lc_dict.data_offset + start, static_cast<size_t>(end - start)};
    }

    template <typename T>
    T get_fixed(uint32_t row) const noexcept {
        uint32_t idx = effective_row(row);
        T v;
        std::memcpy(&v, data + idx * sizeof(T), sizeof(T));
        return v;
    }

    // True when every logical row carries the same bytes.
    // Covers: COL_IS_CONST, and the legacy cross-join pattern where CH
    // repeats the same WKB N times (uniform offsets + identical elements).
    //
    // Every row must be compared. Checking only the first and last is not
    // enough: callers use this to build one geometry from row 0 and reuse it
    // for the whole batch, so a differing middle row would be silently
    // evaluated against the wrong geometry. Uniform stride is common by
    // itself — every 2D WKB point is 21 bytes — so it proves nothing.
    // The loop exits at row 1 for a genuinely varying column, which is the
    // usual case.
    bool is_effectively_const_bytes() const noexcept {
        if (is_const) return true;
        if (!offsets || row_count < 2) return false;
        uint64_t elem_stride = offsets[1];
        if (elem_stride == 0) return false;
        if (offsets[row_count] != elem_stride * row_count) return false;
        for (uint32_t i = 1; i < row_count; ++i)
            if (std::memcmp(data, data + offsets[i], elem_stride) != 0)
                return false;
        return true;
    }
};

struct ColumnarBuf {
    uint32_t              num_rows;
    uint32_t              num_cols;
    uint64_t              total_bytes;
    const ColDescriptor*  descs;
    const uint8_t*        base;

    // Resolve one column descriptor into a ColView, refusing frames the guest
    // cannot interpret: an unknown base tag (the host's COL_FIXEDN/COL_LOWCARD
    // or plain garbage), stray bits outside the two flags, or a descriptor
    // whose null map / offsets array / data blob is not fully inside the frame.
    // Masking flags and hoping the C++ accessor picks a compatible layout is
    // exactly how a String column over a dictionary index array used to read
    // out-of-frame bytes without any error — see the ColumnBinaryWire.h
    // read-side branches, which validate every untrusted offset the same way.
    ColView col(uint32_t i) const {
        ColDescriptor d;
        std::memcpy(&d, descs + i, sizeof(d));

        // Stray bits above the flags (anything but base-tag low bits, 0x20, 0x80).
        if (d.type & ~(uint64_t)(COL_IS_CONST | COL_IS_NULLABLE | 0x0Fu))
            panic("columnar: descriptor type word has bits outside the known flags");
        const uint64_t base_tag = d.type & ~(uint64_t)(COL_IS_CONST | COL_IS_NULLABLE);
        const bool is_nullable = (d.type & COL_IS_NULLABLE) != 0;
        switch (base_tag) {
            case COL_BYTES:
            case COL_FIXED8:
            case COL_FIXED16:
            case COL_FIXED32:
            case COL_FIXED64:
            case COL_COMPLEX:
            case COL_VARIANT:
            case COL_FIXEDN:
                break;
            case COL_LOWCARD:
                // null_offset is always 0 for the writer's LC columns and the
                // dictionary's nullability never reaches the wire; a set bit is
                // a shape this guest would silently misread.
                if (is_nullable)
                    panic("columnar: LowCardinality(Nullable) is not supported");
                break;
            default:
                panic("columnar: unsupported column tag in descriptor");
        }

        ColView v{};
        v.is_const  = (d.type & COL_IS_CONST) != 0;
        v.base_type = static_cast<ColType>(base_tag);
        v.row_count = v.is_const ? 1u : num_rows;

        // Every extent below is checked against the frame size before any
        // pointer is formed from an untrusted offset.  0 keeps its "absent"
        // meaning for null/offset maps.
        // Extent checks mirror the host reader: an offset equal to the frame
        // size is only valid with a zero-length extent (a zero-row column's
        // empty blob legitimately ends the frame); the remaining-space form
        // keeps a huge untrusted offset from wrapping past the comparison.
        if (d.null_offset &&
            (d.null_offset > total_bytes || v.row_count > total_bytes - d.null_offset))
            panic("columnar: null map extends past end of frame");
        if (d.data_offset > total_bytes ||
            d.data_size > total_bytes - d.data_offset)
            panic("columnar: column data range extends past end of frame");

        // Fixed-width tags: record the element width and require the data blob
        // to be exactly row_count elements (the host reader enforces the same
        // equality; a short blob otherwise reads neighbouring columns as data).
        switch (base_tag) {
            case COL_FIXED8:  v.fixed_width = 1u; break;
            case COL_FIXED16: v.fixed_width = 2u; break;
            case COL_FIXED32: v.fixed_width = 4u; break;
            case COL_FIXED64: v.fixed_width = 8u; break;
            case COL_FIXEDN:
                // Width lives only in data_size/row_count.  A zero-row blob
                // has no width to recover; a non-divisible blob is corruption.
                if (v.row_count == 0u) {
                    if (d.data_size != 0u)
                        panic("columnar: COL_FIXEDN data_size must be 0 for an empty column");
                    v.fixed_width = 0u;
                } else {
                    if (d.data_size % v.row_count != 0u)
                        panic("columnar: COL_FIXEDN data_size is not a multiple of row count");
                    v.fixed_width = uint32_t(d.data_size / v.row_count);
                    if (v.fixed_width == 0u)
                        panic("columnar: COL_FIXEDN element width is zero");
                }
                break;
            default:
                break;
        }
        if (v.fixed_width != 0u &&
            d.data_size != uint64_t(v.row_count) * v.fixed_width)
            panic("columnar: fixed-width data_size does not match row count");

        if (d.offsets_offset) {
            // COL_BYTES reads offsets[idx + 1], so it needs row_count + 1
            // uint64 entries; COL_LOWCARD's array holds one entry per row at
            // the dictionary's index width (parsed below); COL_VARIANT's holds
            // uint32 row positions (see variant_offset_at); COL_COMPLEX keeps
            // its offsets inside the data blob.
            uint64_t bytes;
            if (base_tag == COL_BYTES)
                bytes = (v.row_count + 1u) * sizeof(uint64_t);
            else if (base_tag == COL_LOWCARD)
                bytes = uint64_t(v.row_count) * lc_header_index_width(d);
            else if (base_tag == COL_VARIANT)
                bytes = uint64_t(v.row_count) * sizeof(uint32_t);
            else
                bytes = v.row_count * sizeof(uint64_t);
            if (d.offsets_offset > total_bytes ||
                bytes > total_bytes - d.offsets_offset)
                panic("columnar: offsets array extends past end of frame");
        }

        v.null_map  = d.null_offset ? base + d.null_offset : nullptr;
        v.offsets   = d.offsets_offset ? reinterpret_cast<const uint64_t*>(base + d.offsets_offset) : nullptr;
        v.data      = base + d.data_offset;
        v.data_size = d.data_size;
        v.base      = base;

        if (base_tag == COL_LOWCARD)
            load_lowcard(v, d);
        return v;
    }

private:
    // index_elem_width from the COL_LOWCARD header at data_offset, validated to
    // be 1/2/4/8.  The header prefix is bounds-checked here because the generic
    // extent checks of col() consult the width before anything else reads it.
    uint8_t lc_header_index_width(const ColDescriptor& d) const {
        constexpr uint64_t prefix = 4u + 1u;
        if (d.data_size < prefix ||
            d.data_offset > total_bytes ||
            prefix > total_bytes - d.data_offset)
            panic("columnar: LowCardinality header truncated");
        uint8_t w;
        std::memcpy(&w, base + d.data_offset + 4u, 1);
        if (w != 1u && w != 2u && w != 4u && w != 8u)
            panic("columnar: LowCardinality index element width is not 1/2/4/8");
        return w;
    }

    // Validate and attach the dictionary of a COL_LOWCARD view.  The embedded
    // dict_desc is untrusted bytes, so every pointer formed from it is bounded
    // against the frame the same way the top-level descriptor's pointers are.
    void load_lowcard(ColView& v, const ColDescriptor& d) const {
        constexpr uint64_t header_bytes = 4u + 4u + COL_DESC_BYTES;
        if (d.data_size < header_bytes ||
            d.data_offset > total_bytes - header_bytes)
            panic("columnar: LowCardinality header truncated");
        const uint8_t w = lc_header_index_width(d);
        // The index array is mandatory for COL_LOWCARD — 0 is never a real
        // offset (same rejection as the host's readColumnFromDesc).
        if (d.offsets_offset == 0)
            panic("columnar: LowCardinality descriptor has no index array");

        uint32_t dict_rows;
        std::memcpy(&dict_rows, base + d.data_offset, 4);
        ColDescriptor dict{};
        std::memcpy(&dict, base + d.data_offset + 8u, COL_DESC_BYTES);

        // The writer never sets those bits on the dictionary descriptor, and a
        // const or nullable dictionary would break navigation (the host reader
        // rejects the const shape for the same reason).
        if (dict.type & COL_IS_CONST)
            panic("columnar: LowCardinality dictionary must not set COL_IS_CONST");
        if (dict.type & COL_IS_NULLABLE)
            panic("columnar: LowCardinality dictionary must not set COL_IS_NULLABLE");
        // Only string-shaped dictionaries are decoded: that is the sole shape
        // any consumer of a LowCardinality argument asks for.
        if ((dict.type & ~uint64_t(COL_IS_CONST | COL_IS_NULLABLE)) != COL_BYTES)
            panic("columnar: LowCardinality dictionary is not COL_BYTES");
        // ColumnUnique reserves the leading default slot, so a real dictionary
        // is never empty.
        if (dict_rows < 1u)
            panic("columnar: LowCardinality dictionary is empty");
        if (dict.offsets_offset > total_bytes ||
            (uint64_t(dict_rows) + 1u) * sizeof(uint64_t) > total_bytes - dict.offsets_offset)
            panic("columnar: LowCardinality dictionary offsets exceed frame");
        if (dict.data_offset > total_bytes ||
            dict.data_size > total_bytes - dict.data_offset)
            panic("columnar: LowCardinality dictionary data exceeds frame");

        // offsets_offset is the shared index array.
        v.lc_index        = base + d.offsets_offset;
        v.lc_index_width  = w;
        v.lc_dict_rows    = dict_rows;
        v.lc_dict         = dict;
    }
};

inline ColumnarBuf parse_columnar(const raw_buffer* buf) {
    const uint8_t* p = buf->data();
    const uint64_t total = buf->size();
    if (total < HEADER_BYTES)
        panic("columnar: frame shorter than header");

    // The host's readFrameHeader validates magic, version, and the reserved
    // field for the same reason: without it a layout change or a foreign byte
    // range is parsed as an arbitrary frame and produces arbitrary answers.
    uint32_t magic;
    uint16_t version, reserved;
    std::memcpy(&magic, p, 4);
    std::memcpy(&version, p + 4, 2);
    std::memcpy(&reserved, p + 6, 2);
    if (magic != FRAME_MAGIC)
        panic("columnar: bad frame magic");
    if (version != FRAME_VERSION)
        panic("columnar: unsupported frame version");
    if (reserved != 0)
        panic("columnar: reserved frame header field is non-zero");

    ColumnarBuf cb;
    cb.base = p;
    cb.total_bytes = total;
    std::memcpy(&cb.num_rows, p + 8,  4);
    std::memcpy(&cb.num_cols, p + 12, 4);
    if (cb.num_cols > (total - HEADER_BYTES) / COL_DESC_BYTES)
        panic("columnar: descriptor table extends past end of frame");
    cb.descs = reinterpret_cast<const ColDescriptor*>(p + HEADER_BYTES);
    return cb;
}

// ── Output writers ────────────────────────────────────────────────────────────

// Write a fixed-width single-column output (e.g. UInt8 predicates, Float64 scalars).
// Caller fills out->data() + HEADER_BYTES + COL_DESC_BYTES with num_rows * sizeof(T).
template <typename T>
inline void col_write_fixed_header(raw_buffer* out, uint32_t num_rows, uint32_t col_type) {
    out->resize(HEADER_BYTES + COL_DESC_BYTES + num_rows * static_cast<uint32_t>(sizeof(T)));
    uint8_t* p = out->data();

    write_frame_header(p, num_rows, 1);

    ColDescriptor d{};
    d.type         = col_type;
    d.data_offset  = HEADER_BYTES + COL_DESC_BYTES;
    d.data_size    = num_rows * static_cast<uint32_t>(sizeof(T));
    std::memcpy(p + HEADER_BYTES, &d, sizeof(d));
}

// Streaming writer for a single variable-length (bytes) column output.
// Layout: [BufHeader][ColDescriptor][null_map: u8[N]][offsets: u64[N+1]][data...]
struct ColBytesWriter {
    raw_buffer* out;
    uint32_t    num_rows;
    uint32_t    rows_written = 0;
    uint32_t    null_base;
    uint32_t    offs_base;
    uint32_t    data_base;
    bool        nullable;

    explicit ColBytesWriter(raw_buffer* buf, uint32_t n, bool is_nullable = true)
        : out(buf), num_rows(n), nullable(is_nullable)
    {
        null_base = HEADER_BYTES + COL_DESC_BYTES;
        uint32_t after_null = null_base + (nullable ? n : 0u);
        offs_base = (after_null + 7u) & ~7u;   // align to 8
        data_base = offs_base + (n + 1u) * 8u;

        out->resize(data_base);
        uint8_t* p = out->data();

        write_frame_header(p, n, 1);

        ColDescriptor d{};
        d.type           = COL_BYTES | (is_nullable ? COL_IS_NULLABLE : 0u);
        d.null_offset    = is_nullable ? null_base : 0u;
        d.offsets_offset = offs_base;
        d.data_offset    = data_base;
        d.data_size      = 0;
        std::memcpy(p + HEADER_BYTES, &d, sizeof(d));

        const uint64_t zero = 0;
        std::memcpy(p + offs_base, &zero, 8);  // offsets[0] = 0
    }

    void push_null() {
        uint32_t i = rows_written++;
        uint64_t prev;
        std::memcpy(&prev, out->data() + offs_base + i * 8u, 8);
        uint8_t* p = out->data();
        if (nullable) p[null_base + i] = 1;
        std::memcpy(p + offs_base + (i + 1u) * 8u, &prev, 8);
    }

    void push_bytes(std::span<const uint8_t> bytes) {
        uint32_t i = rows_written++;
        uint64_t len = static_cast<uint64_t>(bytes.size());
        uint64_t prev;
        std::memcpy(&prev, out->data() + offs_base + i * 8u, 8);
        uint64_t next = prev + len;
        out->append(bytes.data(), static_cast<uint32_t>(len));
        uint8_t* p = out->data();
        if (nullable) p[null_base + i] = 0;
        std::memcpy(p + offs_base + (i + 1u) * 8u, &next, 8);
    }

    // Append a value through its bytes_codec; a null value becomes push_null().
    template <BytesEncodable T>
    void push_value(const T& v) {
        if (bytes_value_is_null(v)) { push_null(); return; }
        auto bytes = bytes_codec<T>::encode(v);
        push_bytes({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    }

    void finish() {
        uint8_t* p = out->data();
        ColDescriptor d;
        std::memcpy(&d, p + HEADER_BYTES, sizeof(d));
        d.data_size = out->size() - data_base;
        std::memcpy(p + HEADER_BYTES, &d, sizeof(d));
    }
};

// ── COL_COMPLEX output writer ─────────────────────────────────────────────────
// write_complex_data<T>(out, n, get_val): appends N rows of type T to `out`.
// get_val(i) → T; may be called twice per element for pair/tuple fields.
// For vector<T>, pre-collects all rows before writing.

template <typename T, typename GetVal>
void write_complex_data(raw_buffer* out, uint32_t n, GetVal get_val) {
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        for (uint32_t i = 0; i < n; ++i) {
            T v = get_val(i);
            out->append(reinterpret_cast<const uint8_t*>(&v), sizeof(T));
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        // Two-pass: offsets[n+1] + bytes (no null terminators)
        std::vector<std::string> strs(n);
        for (uint32_t i = 0; i < n; ++i) strs[i] = get_val(i);
        std::vector<uint64_t> offs(n + 1u);
        offs[0] = 0u;
        for (uint32_t i = 0; i < n; ++i)
            offs[i + 1u] = offs[i] + static_cast<uint64_t>(strs[i].size());
        out->append(reinterpret_cast<const uint8_t*>(offs.data()), (n + 1u) * 8u);
        for (uint32_t i = 0; i < n; ++i)
            out->append(reinterpret_cast<const uint8_t*>(strs[i].data()),
                        static_cast<uint32_t>(strs[i].size()));
    } else if constexpr (BytesEncodable<T>) {
        // Two-pass: encode every value, then offsets + bytes (no null
        // terminator).  A null value is written as an empty string.
        using Encoded = decltype(bytes_codec<T>::encode(std::declval<const T&>()));
        std::vector<std::optional<Encoded>> encoded(n);
        for (uint32_t i = 0; i < n; ++i) {
            const T& v = get_val(i);
            if (!bytes_value_is_null(v)) encoded[i].emplace(bytes_codec<T>::encode(v));
        }
        std::vector<uint64_t> offs(n + 1u);
        offs[0] = 0u;
        for (uint32_t i = 0; i < n; ++i)
            offs[i + 1u] = offs[i] + (encoded[i] ? static_cast<uint64_t>(encoded[i]->size()) : 0u);
        out->append(reinterpret_cast<const uint8_t*>(offs.data()), (n + 1u) * 8u);
        for (uint32_t i = 0; i < n; ++i)
            if (encoded[i])
                out->append(reinterpret_cast<const uint8_t*>(encoded[i]->data()),
                            static_cast<uint32_t>(encoded[i]->size()));
    } else if constexpr (is_vector_v<T>) {
        using ElemT = typename T::value_type;
        // Collect all rows, write outer offsets, flatten elements, recurse.
        std::vector<T> rows(n);
        for (uint32_t i = 0; i < n; ++i) rows[i] = get_val(i);
        std::vector<uint64_t> outer_offs(n + 1u);
        outer_offs[0] = 0u;
        for (uint32_t i = 0; i < n; ++i)
            outer_offs[i + 1u] = outer_offs[i] + static_cast<uint64_t>(rows[i].size());
        uint32_t M = static_cast<uint32_t>(outer_offs[n]);
        out->append(reinterpret_cast<const uint8_t*>(outer_offs.data()), (n + 1u) * 8u);
        std::vector<ElemT> flat;
        flat.reserve(M);
        for (uint32_t i = 0; i < n; ++i)
            for (auto& elem : rows[i]) flat.push_back(std::move(elem));
        write_complex_data<ElemT>(out, M,
            [&](uint32_t j) -> const ElemT& { return flat[j]; });
    } else if constexpr (is_pair_v<T>) {
        using T1 = typename T::first_type;
        using T2 = typename T::second_type;
        write_complex_data<T1>(out, n, [&](uint32_t i) -> T1 { return get_val(i).first;  });
        write_complex_data<T2>(out, n, [&](uint32_t i) -> T2 { return get_val(i).second; });
    } else if constexpr (is_tuple_v<T>) {
        [&]<size_t... I>(std::index_sequence<I...>) {
            (write_complex_data<std::tuple_element_t<I, T>>(out, n,
                [&](uint32_t i) -> std::tuple_element_t<I, T> {
                    return std::get<I>(get_val(i));
                }), ...);
        }(std::make_index_sequence<std::tuple_size_v<T>>{});
    }
}

// Write a single-column COL_COMPLEX output buffer from n invocations of get_val.
template <typename Ret, typename GetVal>
raw_buffer* write_complex_col(uint32_t n, GetVal get_val) {
    raw_buffer* out = clickhouse_create_buffer(0);
    out->resize(HEADER_BYTES + COL_DESC_BYTES);
    uint8_t* p = out->data();
    write_frame_header(p, n, 1);
    ColDescriptor d{};
    d.type = static_cast<uint32_t>(COL_COMPLEX);
    if constexpr (is_vector_v<Ret>) {
        // Array: outer uint64[n+1] offsets at data_offset, nested data follows immediately.
        d.offsets_offset = 0;
        d.data_offset    = HEADER_BYTES + COL_DESC_BYTES;
    } else {
        // Tuple/pair/scalar: no outer offsets, data starts immediately.
        d.offsets_offset = 0u;
        d.data_offset    = HEADER_BYTES + COL_DESC_BYTES;
    }
    std::memcpy(p + HEADER_BYTES, &d, sizeof(d));

    std::vector<Ret> vals(n);
    for (uint32_t i = 0; i < n; ++i) vals[i] = get_val(i);
    write_complex_data<Ret>(out, n,
        [&](uint32_t i) -> const Ret& { return vals[i]; });

    // Patch data_size (at byte offset 32 within ColDescriptor = HEADER_BYTES+32 in buf)
    uint64_t data_size = static_cast<uint64_t>(out->size() - (HEADER_BYTES + COL_DESC_BYTES));
    std::memcpy(out->data() + HEADER_BYTES + 32u, &data_size, 8u);
    return out;
}

// ── COL_COMPLEX array reader ──────────────────────────────────────────────────
// Reads one row of an Array(T) COL_COMPLEX column.
// Wire layout (see COL_COMPLEX comment in ColType):
//   col.offsets → uint64[row_count+1] outer offsets (cumulative element counts)
//   col.data    → element data:
//     Array(String): uint64[M_total+1] inner_offsets + bytes (no terminator)
//     Array(arithmetic): ElemT[M_total] packed

template <typename ElemT>
std::vector<ElemT> col_get_complex_array(const ColView& col, uint32_t row) {
    uint32_t idx                = col.effective_row(row);
    const uint64_t* outer_offs  = reinterpret_cast<const uint64_t*>(col.data);
    uint64_t outer_start        = outer_offs[idx];
    uint64_t outer_end          = outer_offs[idx + 1];
    uint64_t M_total            = outer_offs[col.row_count];
    uint64_t count              = outer_end - outer_start;
    const uint8_t* inner_data   = col.data + (col.row_count + 1u) * sizeof(uint64_t);

    std::vector<ElemT> result;
    result.reserve(count);

    if constexpr (std::is_same_v<ElemT, std::span<const uint8_t>> || BytesDecodable<ElemT>) {
        // Array(String): inner_data = [uint64[M_total+1] inner_offs][bytes]
        const uint64_t* inner_offs = reinterpret_cast<const uint64_t*>(inner_data);
        const uint8_t*  chars      = inner_data + (M_total + 1u) * sizeof(uint64_t);
        for (uint64_t j = outer_start; j < outer_end; ++j) {
            uint64_t s   = inner_offs[j];
            uint64_t e   = inner_offs[j + 1];
            uint64_t len = e - s;
            std::span<const uint8_t> sp{chars + s, static_cast<size_t>(len)};
            if constexpr (std::is_same_v<ElemT, std::span<const uint8_t>>)
                result.push_back(sp);
            else
                result.push_back(bytes_codec<ElemT>::decode(sp));
        }
    } else if constexpr (std::is_arithmetic_v<ElemT>) {
        // Array(numeric): inner_data = ElemT[M_total] packed
        const ElemT* data_ptr = reinterpret_cast<const ElemT*>(inner_data);
        for (uint32_t j = outer_start; j < outer_end; ++j)
            result.push_back(data_ptr[j]);
    } else if constexpr (std::is_same_v<ElemT, std::string>) {
        const uint64_t* inner_offs = reinterpret_cast<const uint64_t*>(inner_data);
        const uint8_t*  chars      = inner_data + (M_total + 1u) * sizeof(uint64_t);
        for (uint64_t j = outer_start; j < outer_end; ++j) {
            uint64_t s   = inner_offs[j];
            uint64_t e   = inner_offs[j + 1];
            uint64_t len = e - s;
            result.push_back(std::string(reinterpret_cast<const char*>(chars + s), len));
        }
    }
    return result;
}

// ── Input column accessor by type ─────────────────────────────────────────────

// Read a fixed-width column value as type T, widening from narrower stored types.
// CH passes integer literals as the smallest fitting type (e.g. UInt8 for `2`),
// but the _impl function may declare a wider type (e.g. int32_t).  We check the
// actual stored ColType and widen via static_cast.  For floating-point targets,
// bytes are bit-cast directly (no numeric cast across float/int boundary).
template <typename T>
T col_get_fixed_widened(const ColView& col, uint32_t row) {
    uint32_t idx = col.effective_row(row);
    switch (col.base_type) {
        case COL_FIXED8: {
            uint8_t v; std::memcpy(&v, col.data + idx, 1);
            return static_cast<T>(v);
        }
        case COL_FIXED16: {
            int16_t v; std::memcpy(&v, col.data + idx * 2u, 2u);
            return static_cast<T>(v);
        }
        case COL_FIXED32: {
            uint32_t v; std::memcpy(&v, col.data + idx * 4u, 4u);
            return static_cast<T>(v);
        }
        case COL_FIXED64: {
            // Read at the stored width first: memcpy'ing sizeof(T) from a
            // stride-8 array mis-reads every row once T is narrower than 8.
            uint64_t raw;
            std::memcpy(&raw, col.data + uint64_t(idx) * 8u, 8u);
            if constexpr (std::is_floating_point_v<T>) {
                T v; std::memcpy(&v, &raw, 8u);   // Float64: bit-cast
                return v;
            } else {
                return static_cast<T>(raw);        // numeric widening/truncation
            }
        }
        // COL_FIXEDN: any width; take the interpretation from T and let the
        // wire width veto a mismatch (it never carries signedness or type).
        case COL_FIXEDN: {
            if (col.fixed_width != uint32_t(sizeof(T)))
                panic("columnar: COL_FIXEDN width does not match the requested type");
            T v; std::memcpy(&v, col.data + uint64_t(idx) * sizeof(T), sizeof(T));
            return v;
        }
        default: {
            // Any non fixed-width tag (bytes/complex/variant/lowcard) is a
            // hard error — the old default: memcpy'd sizeof(T) bytes from
            // whatever `data` pointed at.
            panic("columnar: col_get_fixed_widened called on non fixed-width column");
        }
    }
}

// Span-shaped argument (String, FixedString, LowCardinality(String)):
// dispatch by tag, validating the width rather than trusting it.  Interpretation
// comes from the C++ side; the tag never carries logical type.
inline std::span<const uint8_t> col_get_span_arg(const ColView& col, uint32_t row) {
    switch (col.base_type) {
        case COL_BYTES:
            return col.get_bytes(row);
        case COL_FIXED8:
        case COL_FIXED16:
        case COL_FIXED32:
        case COL_FIXED64:
        case COL_FIXEDN:
            return col.get_fixed_bytes(row);
        case COL_LOWCARD:
            return col.lc_get_bytes(row);
        default:
            panic("columnar: span argument against an unsupported column tag");
    }
}

template <typename T>
T col_get_arg(const ColView& col, uint32_t row) {
    if constexpr (requires { { column_reader<T>::read(col, row) } -> std::same_as<T>; }) {
        return column_reader<T>::read(col, row);
    } else if constexpr (is_vector_v<T>) {
        return col_get_complex_array<typename T::value_type>(col, row);
    } else if constexpr (std::is_same_v<T, std::span<const uint8_t>>) {
        return col_get_span_arg(col, row);
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        auto s = col_get_span_arg(col, row);
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto s = col_get_span_arg(col, row);
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    } else if constexpr (std::is_arithmetic_v<T>) {
        return col_get_fixed_widened<T>(col, row);
    } else if constexpr (BytesDecodable<T>) {
        return bytes_codec<T>::decode(col_get_span_arg(col, row));
    } else {
        static_assert(sizeof(T) == 0,
            "col_get_arg: unsupported argument type; specialize ch::column_reader or ch::bytes_codec");
    }
}

// ── Result column writer ──────────────────────────────────────────────────────
//
// Builds the single-column result frame for n rows.  invoke(i) computes row i;
// it is not called for a row where any_null(i) is true.  Result shapes:
//   arithmetic (incl. bool) → fixed-width column; NULL rows are NaN for
//                             floating point, 0 otherwise
//   std::string, bytes_codec types → String column; NULL rows are empty
//   std::vector / std::pair / std::tuple → COL_COMPLEX; NULL rows are T{}
//   std::optional<T> (T arithmetic, std::string or bytes_codec) → Nullable(T);
//                             NULL rows and std::nullopt are NULL.  Declare
//                             the SQL result as Nullable(T).
// The buffer is released if invoke throws, then the exception propagates.

template <typename T>
inline constexpr uint32_t fixed_col_tag() {
    if constexpr (sizeof(T) == 1) return COL_FIXED8;
    else if constexpr (sizeof(T) == 2) return COL_FIXED16;
    else if constexpr (sizeof(T) == 4) return COL_FIXED32;
    else if constexpr (sizeof(T) == 8) return COL_FIXED64;
    else static_assert(sizeof(T) == 0, "fixed_col_tag: unsupported width");
}

struct buffer_guard {
    raw_buffer* buf;
    ~buffer_guard() { if (buf) clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf)); }
    raw_buffer* release() { raw_buffer* b = buf; buf = nullptr; return b; }
};

// Nullable fixed-width result: [header][desc][null map u8[n]][pad to 8][T[n]].
template <typename T, typename Get>
raw_buffer* write_nullable_fixed_column(uint32_t n, Get get) {
    const uint32_t null_base = HEADER_BYTES + COL_DESC_BYTES;
    const uint32_t data_base = (null_base + n + 7u) & ~7u;
    buffer_guard g{clickhouse_create_buffer(data_base + n * uint32_t(sizeof(T)))};
    uint8_t* p = g.buf->data();
    std::memset(p, 0, data_base + n * sizeof(T));
    write_frame_header(p, n, 1);
    ColDescriptor d{};
    d.type        = fixed_col_tag<T>() | COL_IS_NULLABLE;
    d.null_offset = null_base;
    d.data_offset = data_base;
    d.data_size   = uint64_t(n) * sizeof(T);
    std::memcpy(p + HEADER_BYTES, &d, sizeof(d));
    for (uint32_t i = 0; i < n; ++i) {
        std::optional<T> v = get(i);
        uint8_t* row = g.buf->data();
        if (v) std::memcpy(row + data_base + uint64_t(i) * sizeof(T), &*v, sizeof(T));
        else   row[null_base + i] = 1;
    }
    return g.release();
}

template <typename Ret, typename AnyNull, typename Invoke>
raw_buffer* write_result_column(uint32_t n, AnyNull any_null, Invoke invoke) {
    if constexpr (is_optional_v<Ret>) {
        using T = typename Ret::value_type;
        auto get = [&](uint32_t i) -> Ret { return any_null(i) ? Ret{} : Ret(invoke(i)); };
        if constexpr (std::is_arithmetic_v<T>) {
            if constexpr (std::is_same_v<T, bool>)
                return write_nullable_fixed_column<uint8_t>(n, [&](uint32_t i) -> std::optional<uint8_t> {
                    auto v = get(i);
                    return v ? std::optional<uint8_t>(*v ? 1u : 0u) : std::nullopt;
                });
            else
                return write_nullable_fixed_column<T>(n, get);
        } else if constexpr (std::is_same_v<T, std::string> || BytesEncodable<T>) {
            buffer_guard g{clickhouse_create_buffer(0)};
            ColBytesWriter w(g.buf, n, /*nullable=*/true);
            for (uint32_t i = 0; i < n; ++i) {
                Ret v = get(i);
                if (!v) { w.push_null(); continue; }
                if constexpr (std::is_same_v<T, std::string>)
                    w.push_bytes({reinterpret_cast<const uint8_t*>(v->data()), v->size()});
                else
                    w.push_value(*v);
            }
            w.finish();
            return g.release();
        } else {
            static_assert(sizeof(T) == 0,
                "write_result_column: std::optional supports arithmetic, std::string and bytes_codec types");
        }
    } else if constexpr (std::is_same_v<Ret, bool>) {
        buffer_guard g{clickhouse_create_buffer(HEADER_BYTES + COL_DESC_BYTES + n)};
        col_write_fixed_header<uint8_t>(g.buf, n, COL_FIXED8);
        uint8_t* res = g.buf->data() + HEADER_BYTES + COL_DESC_BYTES;
        for (uint32_t i = 0; i < n; ++i)
            res[i] = (!any_null(i) && invoke(i)) ? 1u : 0u;
        return g.release();
    } else if constexpr (std::is_arithmetic_v<Ret>) {
        buffer_guard g{clickhouse_create_buffer(HEADER_BYTES + COL_DESC_BYTES + n * uint32_t(sizeof(Ret)))};
        col_write_fixed_header<Ret>(g.buf, n, fixed_col_tag<Ret>());
        uint8_t* res = g.buf->data() + HEADER_BYTES + COL_DESC_BYTES;
        for (uint32_t i = 0; i < n; ++i) {
            Ret v;
            if (!any_null(i))
                v = invoke(i);
            else if constexpr (std::is_floating_point_v<Ret>)
                v = std::numeric_limits<Ret>::quiet_NaN();
            else
                v = Ret{};
            std::memcpy(res + uint64_t(i) * sizeof(Ret), &v, sizeof(Ret));
        }
        return g.release();
    } else if constexpr (std::is_same_v<Ret, std::string> || BytesEncodable<Ret>) {
        buffer_guard g{clickhouse_create_buffer(0)};
        ColBytesWriter w(g.buf, n, /*nullable=*/false);
        for (uint32_t i = 0; i < n; ++i) {
            if (any_null(i)) { w.push_null(); continue; }
            if constexpr (std::is_same_v<Ret, std::string>) {
                std::string s = invoke(i);
                w.push_bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
            } else {
                w.push_value(invoke(i));
            }
        }
        w.finish();
        return g.release();
    } else if constexpr (is_complex_v<Ret>) {
        return write_complex_col<Ret>(n, [&](uint32_t i) -> Ret {
            return any_null(i) ? Ret{} : invoke(i);
        });
    } else {
        static_assert(sizeof(Ret) == 0,
            "write_result_column: unsupported result type; specialize ch::bytes_codec");
    }
}

// ── Generic columnar UDF entry point ─────────────────────────────────────────
//
// Decodes the input frame, calls impl once per row with arguments read by
// col_get_arg, and returns the result frame.  A row where any argument is
// NULL is not passed to impl (see write_result_column for its value).
// Every failure — malformed frame, exception from impl — becomes panic(),
// so nothing unwinds through the WASM boundary.

template <typename Ret, typename... Args>
raw_buffer* columnar_call(raw_buffer* input, Ret (*impl)(Args...)) {
    try {
        auto cb = parse_columnar(input);
        constexpr size_t nargs = sizeof...(Args);
        if (cb.num_cols < nargs)
            panic("columnar: frame has fewer columns than the function has arguments");

        std::array<ColView, nargs> cols;
        for (size_t j = 0; j < nargs; ++j) cols[j] = cb.col(static_cast<uint32_t>(j));

        auto any_null = [&](uint32_t row) {
            bool null = false;
            for (size_t j = 0; j < nargs; ++j) null |= cols[j].is_null(row);
            return null;
        };
        auto invoke = [&](uint32_t row) -> Ret {
            return [&]<size_t... I>(std::index_sequence<I...>) {
                return impl(col_get_arg<std::decay_t<Args>>(cols[I], row)...);
            }(std::make_index_sequence<nargs>{});
        };
        return write_result_column<Ret>(cb.num_rows, any_null, invoke);
    } catch (const std::exception& e) {
        panic(e.what());
    }
}

} // namespace ch

// Export `fn` as a COLUMNAR_V1 UDF named `name`:
//   CREATE FUNCTION name LANGUAGE WASM ABI COLUMNAR_V1 FROM 'module' ...
#define CH_COLUMNAR_UDF(name, fn)                                               \
    __attribute__((export_name(#name)))                                         \
    ch::raw_buffer * name(ch::raw_buffer * input, uint32_t) {                   \
        return ch::columnar_call(input, fn);                                    \
    }
