// Frame parsing and validation.  Every host frame used here is a verbatim dump
// of the ClickHouse writer (tests/wire_fixtures/, see regenerate_wire_fixtures.sh).
#include <algorithm>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "wire_fixtures.gen.hpp"

using namespace ch;
using namespace test;

namespace {

struct FixedStrideCol {
    std::vector<uint8_t>  data;
    std::vector<uint64_t> offsets;
    ColView               view{};

    // rows: equal-length elements, laid out back to back with start-based offsets.
    explicit FixedStrideCol(const std::vector<std::string>& rows) {
        offsets.push_back(0);
        for (const auto& r : rows) {
            data.insert(data.end(), r.begin(), r.end());
            offsets.push_back(data.size());
        }
        view.base_type = COL_BYTES;
        view.is_const  = false;
        view.row_count = static_cast<uint32_t>(rows.size());
        view.null_map  = nullptr;
        view.offsets   = offsets.data();
        view.data      = data.data();
        view.base      = data.data();
    }
};

}  // namespace

TEST(ColumnarConstBytes, ConstFlagShortCircuits) {
    ColView v{};
    v.is_const  = true;
    v.row_count = 1;
    EXPECT_TRUE(v.is_effectively_const_bytes());
}

TEST(ColumnarConstBytes, SingleRowIsNotConst) {
    FixedStrideCol c({"aaa"});
    EXPECT_FALSE(c.view.is_effectively_const_bytes());
}

TEST(ColumnarConstBytes, AllRowsIdentical) {
    FixedStrideCol c({"abc", "abc", "abc", "abc"});
    EXPECT_TRUE(c.view.is_effectively_const_bytes());
}

// The regression: uniform stride and first == last, but a middle row differs.
// Every 2D WKB point is 21 bytes, so stride alone proves nothing.
TEST(ColumnarConstBytes, DifferingMiddleRowIsNotConst) {
    FixedStrideCol c({"abc", "xyz", "abc"});
    EXPECT_FALSE(c.view.is_effectively_const_bytes());
}

TEST(ColumnarConstBytes, DifferingLastRowIsNotConst) {
    FixedStrideCol c({"abc", "abc", "xyz"});
    EXPECT_FALSE(c.view.is_effectively_const_bytes());
}

TEST(ColumnarConstBytes, VaryingWidthIsNotConst) {
    FixedStrideCol c({"ab", "cde", "fg"});
    EXPECT_FALSE(c.view.is_effectively_const_bytes());
}

// ── G1: strict frame and tag validation ─────────────────────────────────────
//
// Before this work the guest masked only COL_IS_CONST/COL_IS_NULLABLE off the
// descriptor type word, so any tag survived verbatim and whichever accessor the
// C++ argument type picked silently misread the column (e.g. a String accessor
// over a dictionary index array). parse_columnar never checked the frame magic
// or version, so a host-side layout change was diagnosed as arbitrary wrong
// answers. Every case below must be a loud failure, not garbage.

namespace {

// Patch bytes into an otherwise-valid frame built by make_columnar.
static void patch(uint8_t* p, size_t off, const void* src, size_t n) {
    std::memcpy(p + off, src, n);
}

static void patch_magic(raw_buffer* buf, uint32_t magic)   { patch(buf->data(), 0, &magic, 4); }
static void patch_version(raw_buffer* buf, uint16_t ver)   { patch(buf->data(), 4, &ver, 2); }
static void patch_reserved(raw_buffer* buf, uint16_t res)  { patch(buf->data(), 6, &res, 2); }

static void patch_desc_type(raw_buffer* buf, uint32_t col, uint64_t type) {
    patch(buf->data(), HEADER_BYTES + col * COL_DESC_BYTES, &type, 8);
}
static void patch_desc_data_offset(raw_buffer* buf, uint32_t col, uint64_t off) {
    patch(buf->data(), HEADER_BYTES + col * COL_DESC_BYTES + 24u, &off, 8);
}

// A minimal well-formed single-column fixed8 frame used as the base for patching.
static raw_buffer* make_fixed8_frame(uint32_t n) {
    ColData c;
    c.col_type = static_cast<uint32_t>(COL_FIXED8);
    c.data.assign(n, 7u);
    return make_frame(n, {c});
}

}  // namespace

TEST(ColumnarFrameValidation, BadMagicThrows) {
    auto* buf = make_fixed8_frame(3);
    patch_magic(buf, 0xDEADBEEFu);
    EXPECT_THROW(parse_columnar(buf), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

TEST(ColumnarFrameValidation, Version2Throws) {
    auto* buf = make_fixed8_frame(3);
    patch_version(buf, 2);
    EXPECT_THROW(parse_columnar(buf), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// The host's readFrameHeader requires reserved == 0; a frame that uses a
// frame-wide flag must be refused, not silently parsed.
TEST(ColumnarFrameValidation, NonZeroReservedThrows) {
    auto* buf = make_fixed8_frame(3);
    patch_reserved(buf, 1);
    EXPECT_THROW(parse_columnar(buf), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// The known-tag bound is the switch in ColumnarBuf::col: tags 0–8 (COL_BYTES
// … COL_LOWCARD) parse, anything above throws. It was 0–6 before COL_FIXEDN
// and COL_LOWCARD landed, which is exactly why the bound is expressed as a
// switch over the enum and not as `base_tag <= 6`.
TEST(ColumnarFrameValidation, UnknownDescriptorType10Throws) {
    auto* buf = make_fixed8_frame(3);
    patch_desc_type(buf, 0, 10);
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

TEST(ColumnarFrameValidation, UnknownDescriptorType9Throws) {
    auto* buf = make_fixed8_frame(3);
    patch_desc_type(buf, 0, 9);
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// Tags above the known range must also be refused, including garbage in the
// bits above the flags.
TEST(ColumnarFrameValidation, GarbageDescriptorTypeThrows) {
    auto* buf = make_fixed8_frame(3);
    patch_desc_type(buf, 0, 0x1234u);
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// All tags the guest supports must still parse cleanly with flags set.
TEST(ColumnarFrameValidation, KnownTagsWithFlagsAccepted) {
    const uint32_t tags[] = {COL_BYTES, COL_FIXED8, COL_FIXED16, COL_FIXED32, COL_FIXED64};
    for (uint32_t tag : tags) {
        ColData c;
        c.col_type = tag | COL_IS_NULLABLE;
        c.null_map = {0u, 0u};
        if (tag == COL_BYTES) {
            c.offsets = {0u, 2u, 4u};
            c.data = {'a', 'b', 'c', 'd'};
        } else {
            c.data.assign(2u * (tag == COL_FIXED8 ? 1u : tag == COL_FIXED16 ? 2u : tag == COL_FIXED32 ? 4u : 8u), 0u);
        }
        auto* buf = make_frame(2, {c});
        auto cb = parse_columnar(buf);
        ColView v;
        try {
            v = cb.col(0);
        } catch (const WasmPanic& e) {
            clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
            FAIL() << "tag " << tag << " rejected: " << e.what();
            continue;
        }
        EXPECT_EQ(static_cast<uint32_t>(v.base_type), tag);
        EXPECT_TRUE(v.null_map != nullptr);
        clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
    }
}

// A descriptor claiming data outside the buffer must not build a view over
// foreign bytes.
TEST(ColumnarFrameValidation, DescriptorDataBeyondBufferThrows) {
    auto* buf = make_fixed8_frame(3);
    patch_desc_data_offset(buf, 0, 1u << 20);
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// A wrapper-level bad frame must trap (panic), not propagate through WASM.
TEST(ColumnarFrameValidation, WrapperPanicsOnBadMagic) {
    auto* buf = make_fixed8_frame(3);
    patch_magic(buf, 0u);
    EXPECT_THROW(columnar_call(buf, +[](uint8_t) { return true; }),
                 WasmPanic);
}

// col_get_fixed_widened must refuse a column whose tag is not fixed-width:
// today its default: branch memcpy's sizeof(T) bytes from wherever `data`
// happens to point.
TEST(ColumnarFrameValidation, FixedWidenedRejectsBytesColumn) {
    ColData c;
    c.col_type = static_cast<uint32_t>(COL_BYTES);
    c.offsets = {0u, 4u};
    c.data = {'1', '2', '3', '4'};
    auto* buf = make_frame(1, {c});
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    EXPECT_THROW((void)col_get_fixed_widened<double>(col, 0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// ── G2: COL_FIXEDN — fixed width ∉ {1,2,4,8} ────────────────────────────────
//
// Every frame in this section is a verbatim dump of the ClickHouse host's own
// buildColDescriptor/writeColData output (tests/wire_fixtures/, regenerated by
// tests/regenerate_wire_fixtures.sh). The interesting property, pinned by the
// host's FixedStringWidthClassesByLength test: the descriptor encodes a coarse
// width class only. FixedString(8) arrives as COL_FIXED64; UUID/IPv6/
// Decimal128/FixedString(16) arrive as COL_FIXEDN with the width recorded only
// as data_size/num_rows. Interpretation must come from the C++ side; the tag
// may only ever validate the width.


TEST(ColumnarFixedN, FixedString16RoundTripFromHostFrame) {
    auto* buf = frame_from_bytes(wire_fixture::FIXEDN_FS16, wire_fixture::FIXEDN_FS16_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_EQ(col.base_type, static_cast<ColType>(COL_FIXEDN));
    ASSERT_EQ(col.row_count, 2u);
    ASSERT_EQ(col.fixed_width, 16u);
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 0), "0123456789abcdef");
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 1), "fedcba9876543210");
}

TEST(ColumnarFixedN, UuidIPv6Decimal128ShareOneTag) {
    struct { const uint8_t* bytes; size_t len; const char* name; } fixtures[] = {
        {wire_fixture::FIXEDN_UUID, wire_fixture::FIXEDN_UUID_len, "uuid"},
        {wire_fixture::FIXEDN_IPV6, wire_fixture::FIXEDN_IPV6_len, "ipv6"},
        {wire_fixture::FIXEDN_DECIMAL128, wire_fixture::FIXEDN_DECIMAL128_len, "decimal128"},
    };
    for (auto& fx : fixtures) {
        raw_buffer* b = frame_from_bytes(fx.bytes, fx.len);
        auto cb = parse_columnar(b);
        auto col = cb.col(0);
        // The raw row spans must be exactly the descriptor's data blob at stride 16.
        ColDescriptor d = desc_at(fx.bytes, 0);
        ASSERT_EQ(col.base_type, static_cast<ColType>(COL_FIXEDN)) << fx.name;
        ASSERT_EQ(col.fixed_width, 16u) << fx.name;
        for (uint32_t row = 0; row < col.row_count; ++row) {
            auto s = col_get_arg<std::span<const uint8_t>>(col, row);
            ASSERT_EQ(s.size(), 16u) << fx.name;
            EXPECT_TRUE(std::equal(s.begin(), s.end(), fx.bytes + d.data_offset + row * 16u))
                << fx.name << " row " << row;
        }
        clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(b));
    }
}

TEST(ColumnarFixedN, NullableFixedString16Nulls) {
    auto* buf = frame_from_bytes(wire_fixture::FIXEDN_FS16_NULLABLE, wire_fixture::FIXEDN_FS16_NULLABLE_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_EQ(col.base_type, static_cast<ColType>(COL_FIXEDN));
    EXPECT_FALSE(col.is_null(0));
    EXPECT_TRUE(col.is_null(1));   // the middle row is NULL; its value slot stays present
    EXPECT_FALSE(col.is_null(2));
    // FixedString(16) values are zero-padded on the wire.
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 0),
                 std::string_view("abcdefgh\0\0\0\0\0\0\0\0", 16));
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 2),
                 std::string_view("ijklmnop\0\0\0\0\0\0\0\0", 16));
}

TEST(ColumnarFixedN, ZeroRowsParsesWithoutDivideByZero) {
    auto* buf = frame_from_bytes(wire_fixture::FIXEDN_ZERO_ROWS, wire_fixture::FIXEDN_ZERO_ROWS_len);
    ColView v;
    {
        auto cb = parse_columnar(buf);
        v = cb.col(0);  // must not throw: width cannot be divided out of 0 rows
    }
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
    EXPECT_EQ(v.base_type, static_cast<ColType>(COL_FIXEDN));
    EXPECT_EQ(v.row_count, 0u);
}

TEST(ColumnarFixedN, WidthMismatchRejectsTypedRead) {
    // A Double argument against a 16-byte fixed column is a declared-type lie;
    // the tag is only allowed to validate width, so this must be a hard error.
    auto* buf = frame_from_bytes(wire_fixture::FIXEDN_FS16, wire_fixture::FIXEDN_FS16_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
    EXPECT_THROW((void)col_get_arg<double>(col, 0), WasmPanic);
}

TEST(ColumnarFixedN, NotDivisibleDataSizeThrows) {
    ColData c;
    c.col_type = static_cast<uint32_t>(COL_FIXEDN);
    c.data = {1, 2, 3};                      // 3 bytes, 2 rows — not a multiple
    auto* buf = make_frame(2, {c});
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

TEST(ColumnarFixedN, FixedString8ArrivesAsFixed64WidthClass) {
    // The host classes by width: FixedString(8) is COL_FIXED64, never COL_FIXEDN.
    // A span-shaped argument must still get the right 8 bytes.
    auto* buf = frame_from_bytes(wire_fixture::FIXEDWIDTH_FS8, wire_fixture::FIXEDWIDTH_FS8_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_EQ(col.base_type, static_cast<ColType>(COL_FIXED64));
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 0), "abcdefgh");
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 1), "ijklmnop");
}

TEST(ColumnarFixedN, ConstFixedString8Broadcasts) {
    auto* buf = frame_from_bytes(wire_fixture::FIXEDWIDTH_FS8_CONST, wire_fixture::FIXEDWIDTH_FS8_CONST_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_TRUE(col.is_const);
    ASSERT_EQ(col.row_count, 1u);
    EXPECT_EQ(col.data_size, 8u);            // one stored row, not five
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 4), "const888");
}

// ── G3: COL_LOWCARD — dictionary + shared index ──────────────────────────────
//
// Wire shape (host ColumnBinaryWire.h, COL_LOWCARD branch):
//   offsets_offset → index[num_rows], index_elem_width bytes each (1/2/4/8)
//   data_offset    → uint32 dict_row_count | uint8 index_elem_width | pad[3]
//                    | ColDescriptor dict_desc | dictionary sub-column data
// null_offset is 0 for both the outer column and the dictionary; for
// LowCardinality(Nullable(T)) NULL rides on dictionary slot 0 and the wire is
// byte-identical to a plain LowCardinality whose dictionary happens to start
// with the empty string. The guest therefore supports the non-nullable shape
// only; any COL_IS_NULLABLE bit on a COL_LOWCARD descriptor is rejected, and
// the nullable form must be refused before it reaches the wire (see the H1
// host-side gate).

namespace {

constexpr const char* kLowCardValues[] = {"alpha", "beta", "alpha", "gamma", "beta", "alpha"};

}  // namespace

TEST(ColumnarLowCard, IndexWidthsAllDecodeFromHostFrames) {
    struct { const uint8_t* bytes; size_t len; uint8_t width; } fixtures[] = {
        {wire_fixture::LOWCARD_STRING_W1, wire_fixture::LOWCARD_STRING_W1_len, 1},
        {wire_fixture::LOWCARD_STRING_W2, wire_fixture::LOWCARD_STRING_W2_len, 2},
        {wire_fixture::LOWCARD_STRING_W4, wire_fixture::LOWCARD_STRING_W4_len, 4},
        {wire_fixture::LOWCARD_STRING_W8, wire_fixture::LOWCARD_STRING_W8_len, 8},
    };
    for (auto& fx : fixtures) {
        raw_buffer* b = frame_from_bytes(fx.bytes, fx.len);
        auto cb = parse_columnar(b);
        auto col = cb.col(0);
        ASSERT_EQ(col.base_type, static_cast<ColType>(COL_LOWCARD)) << "width " << uint32_t(fx.width);
        ASSERT_EQ(col.row_count, 6u) << "width " << uint32_t(fx.width);
        for (uint32_t row = 0; row < col.row_count; ++row) {
            expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, row),
                         kLowCardValues[row]);
        }
        clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(b));
    }
}

TEST(ColumnarLowCard, DictionaryMayExceedFrameRows) {
    auto* buf = frame_from_bytes(wire_fixture::LOWCARD_DICT_GT_ROWS, wire_fixture::LOWCARD_DICT_GT_ROWS_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_EQ(col.base_type, static_cast<ColType>(COL_LOWCARD));
    EXPECT_GT(col.lc_dict_rows, col.row_count);
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 0), "only-one-value-here");
}

TEST(ColumnarLowCard, ConstColumnBroadcastsOneIndex) {
    auto* buf = frame_from_bytes(wire_fixture::LOWCARD_STRING_CONST, wire_fixture::LOWCARD_STRING_CONST_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_TRUE(col.is_const);
    ASSERT_EQ(col.row_count, 1u);
    expect_span_eq(col_get_arg<std::span<const uint8_t>>(col, 5), "const-value");
}

TEST(ColumnarLowCard, NullableBitRejected) {
    // The real writer never sets COL_IS_NULLABLE on a COL_LOWCARD descriptor
    // (see lowcard_nullable_string host test); seeing the bit means the frame
    // is lying about a shape this guest would silently misread, so reject it.
    std::vector<uint8_t> bytes(wire_fixture::LOWCARD_STRING_W1,
                               wire_fixture::LOWCARD_STRING_W1 + wire_fixture::LOWCARD_STRING_W1_len);
    uint64_t t;
    std::memcpy(&t, bytes.data() + HEADER_BYTES, 8);
    t |= COL_IS_NULLABLE;
    std::memcpy(bytes.data() + HEADER_BYTES, &t, 8);

    auto* buf = frame_from_bytes(bytes.data(), bytes.size());
    auto cb = parse_columnar(buf);
    EXPECT_THROW(cb.col(0), WasmPanic);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

TEST(ColumnarLowCard, IndexPastDictionaryThrows) {
    std::vector<uint8_t> bytes(wire_fixture::LOWCARD_STRING_W1,
                               wire_fixture::LOWCARD_STRING_W1 + wire_fixture::LOWCARD_STRING_W1_len);
    ColDescriptor d = desc_at(bytes.data(), 0);
    bytes[d.offsets_offset] = 0xFFu;   // index beyond the dictionary

    auto* buf = frame_from_bytes(bytes.data(), bytes.size());
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
    EXPECT_THROW((void)col_get_arg<std::span<const uint8_t>>(col, 0), WasmPanic);
}

// ── COL_VARIANT row offsets are uint32 on the wire ─────────────────────────
//
// The host's Variant branch of writeColData places the per-row positions as
// uint32[num_rows] at offsets_offset, and the host reader loads them back as
// uint32. This frame is a verbatim dump of that writer (rows:
// UInt64(10), String("hi"), UInt64(20), NULL — positions {0,0,1,0}). A decoder
// walking the array as the uint64 COL_BYTES offsets uses reads two adjacent
// positions per step and mislocates rows; validation sized for uint64 would
// also reject legitimate short frames. See ColView::variant_offset_at.

TEST(ColumnarVariant, HostFrameRowOffsetsAreUint32) {
    auto* buf = frame_from_bytes(wire_fixture::VARIANT_U64_STRING, wire_fixture::VARIANT_U64_STRING_len);
    auto cb = parse_columnar(buf);
    auto col = cb.col(0);
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));

    ASSERT_EQ(col.base_type, static_cast<ColType>(COL_VARIANT));
    ASSERT_EQ(col.row_count, 4u);

    // Discriminators live at null_offset (0xFF = NULL row).
    ASSERT_FALSE(col.is_null(0));
    ASSERT_FALSE(col.is_null(1));
    ASSERT_FALSE(col.is_null(2));
    ASSERT_TRUE(col.is_null(3));

    // Positions within each sub-column, read at their wire width.
    EXPECT_EQ(col.variant_offset_at(0), 0u);
    EXPECT_EQ(col.variant_offset_at(1), 0u);
    EXPECT_EQ(col.variant_offset_at(2), 1u);
    EXPECT_EQ(col.variant_offset_at(3), 0u);
}

TEST(ColumnarVariant, Uint32SizedOffsetArrayAccepted) {
    // Park the offsets array at the very end of the frame: row_count × 4 bytes
    // fit exactly, row_count × 8 would not. A host-written frame this tight
    // must validate — the array is uint32, sized like the host sizes it.
    std::vector<uint8_t> bytes(wire_fixture::VARIANT_U64_STRING,
                               wire_fixture::VARIANT_U64_STRING + wire_fixture::VARIANT_U64_STRING_len);
    ColDescriptor d = desc_at(bytes.data(), 0);
    ASSERT_GT(d.offsets_offset, 0u);
    uint64_t new_offs = bytes.size() - 4u * 4u;   // 16 bytes, exactly row_count × 4
    std::memcpy(bytes.data() + HEADER_BYTES + 16, &new_offs, 8);   // offsets_offset field

    auto* buf = frame_from_bytes(bytes.data(), bytes.size());
    auto cb = parse_columnar(buf);
    EXPECT_NO_THROW((void)cb.col(0));
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// A null handle (what the host passes when there is no input frame) must
// panic with a message, not dereference null.
TEST(ColumnarFrame, NullHandlePanics) {
    EXPECT_THROW(parse_columnar(nullptr), WasmPanic);
}
