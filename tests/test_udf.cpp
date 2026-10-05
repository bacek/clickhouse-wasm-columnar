// columnar_call: argument decoding, result encoding, NULL handling, and the
// bytes_codec / column_reader customization points.
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <variant>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "helpers.hpp"
#include "wire_fixtures.gen.hpp"

// A user type that travels as a String: "x,y".
struct Point2 {
    double x = 0, y = 0;
    bool   empty = false;
};

template <>
struct ch::bytes_codec<Point2> {
    static Point2 decode(std::span<const uint8_t> s) {
        std::string str(s.begin(), s.end());
        auto comma = str.find(',');
        if (comma == std::string::npos) throw std::runtime_error("bad point: " + str);
        return {std::stod(str.substr(0, comma)), std::stod(str.substr(comma + 1))};
    }
    static std::string encode(const Point2& p) {
        return std::to_string(int(p.x)) + "," + std::to_string(int(p.y));
    }
    static bool is_null(const Point2& p) { return p.empty; }
};

// A type decoded straight from the column: the row's string length.
struct Length { uint64_t n; };

template <>
struct ch::column_reader<Length> {
    static Length read(const ColView& col, uint32_t row) {
        return {col_get_span_arg(col, row).size()};
    }
};

using namespace ch;
using namespace test;

namespace {

double add(double a, double b) { return a + b; }
int32_t twice(int32_t v) { return v * 2; }
bool longer_than(std::string_view s, uint32_t n) { return s.size() > n; }
std::string upper(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}
std::vector<int32_t> range(uint32_t n) {
    std::vector<int32_t> r;
    for (uint32_t i = 0; i < n; ++i) r.push_back(int32_t(i));
    return r;
}
uint64_t sum_array(std::vector<int32_t> v) {
    uint64_t s = 0;
    for (auto x : v) s += uint64_t(x);
    return s;
}
std::pair<std::string, double> split_pair(std::string_view s, double d) {
    return {std::string(s), d};
}
double point_x(Point2 p) { return p.x; }
Point2 swap_point(Point2 p) { return p.x < 0 ? Point2{0, 0, true} : Point2{p.y, p.x}; }
uint64_t length_of(Length l) { return l.n; }
double throws(double) { throw std::runtime_error("boom"); }

}  // namespace

TEST(ColumnarCall, DoubleFromTwoColumns) {
    auto* in = make_frame(3, {fixed_col<double>({1, 2, 3}), fixed_col<double>({10, 20, 30})});
    auto got = read_fixed_result<double>(columnar_call(in, add));
    destroy(in);
    EXPECT_EQ(got, (std::vector<double>{11, 22, 33}));
}

TEST(ColumnarCall, ConstArgumentBroadcasts) {
    auto* in = make_frame(3, {fixed_col<double>({1, 2, 3}), fixed_col<double>({100}, {}, true)});
    auto got = read_fixed_result<double>(columnar_call(in, add));
    destroy(in);
    EXPECT_EQ(got, (std::vector<double>{101, 102, 103}));
}

TEST(ColumnarCall, NullRowIsNaNForFloatAndNotInvoked) {
    auto* in = make_frame(2, {fixed_col<double>({1, 2}, {0, 1}), fixed_col<double>({1, 1})});
    auto got = read_fixed_result<double>(columnar_call(in, add));
    destroy(in);
    EXPECT_EQ(got[0], 2.0);
    EXPECT_TRUE(std::isnan(got[1]));
}

TEST(ColumnarCall, NullRowIsZeroForIntegers) {
    auto* in = make_frame(2, {fixed_col<int32_t>({5, 7}, {1, 0})});
    auto got = read_fixed_result<int32_t>(columnar_call(in, twice));
    destroy(in);
    EXPECT_EQ(got, (std::vector<int32_t>{0, 14}));
}

TEST(ColumnarCall, NarrowIntegerWidensToDeclaredType) {
    // ClickHouse passes the literal 3 as UInt8.
    auto* in = make_frame(2, {string_col({"ab", "abcd"}), fixed_col<uint8_t>({3}, {}, true)});
    auto got = read_fixed_result<uint8_t>(columnar_call(in, longer_than));
    destroy(in);
    EXPECT_EQ(got, (std::vector<uint8_t>{0, 1}));
}

TEST(ColumnarCall, StringInStringOut) {
    auto* in = make_frame(3, {string_col({"a", "", "xyz"})});
    auto got = read_string_result(columnar_call(in, upper));
    destroy(in);
    EXPECT_EQ(got, (std::vector<std::string>{"A", "", "XYZ"}));
}

TEST(ColumnarCall, NullStringRowIsEmpty) {
    auto* in = make_frame(2, {string_col({"a", "b"}, {0, 1})});
    auto got = read_string_result(columnar_call(in, upper));
    destroy(in);
    EXPECT_EQ(got, (std::vector<std::string>{"A", ""}));
}

TEST(ColumnarCall, ArrayResultRoundTripsAsArgument) {
    auto* in = make_frame(3, {fixed_col<uint32_t>({0, 2, 3})});
    auto* arrays = columnar_call(in, range);
    destroy(in);
    auto got = read_fixed_result<uint64_t>(columnar_call(arrays, sum_array));
    destroy(arrays);
    EXPECT_EQ(got, (std::vector<uint64_t>{0, 1, 3}));
}

TEST(ColumnarCall, TupleResultIsColumnar) {
    auto* in = make_frame(2, {string_col({"p", "qq"}), fixed_col<double>({1.5, 2.5})});
    auto* out = columnar_call(in, split_pair);
    destroy(in);
    // Tuple(String, Float64): offsets[n+1] + bytes, then Float64[n].
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    EXPECT_EQ(col.base_type, COL_COMPLEX);
    const uint8_t* d = col.data;
    uint64_t offs[3];
    std::memcpy(offs, d, sizeof(offs));
    EXPECT_EQ(offs[2], 3u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(d + 24), 3), "pqq");
    double second[2];
    std::memcpy(second, d + 24 + 3, sizeof(second));
    EXPECT_EQ(second[0], 1.5);
    EXPECT_EQ(second[1], 2.5);
    destroy(out);
}

TEST(ColumnarCall, BytesCodecDecodesArgument) {
    auto* in = make_frame(2, {string_col({"1,2", "-3,4"})});
    auto got = read_fixed_result<double>(columnar_call(in, point_x));
    destroy(in);
    EXPECT_EQ(got, (std::vector<double>{1, -3}));
}

TEST(ColumnarCall, BytesCodecEncodesResultAndNull) {
    auto* in = make_frame(2, {string_col({"1,2", "-3,4"})});
    auto got = read_string_result(columnar_call(in, swap_point));
    destroy(in);
    EXPECT_EQ(got, (std::vector<std::string>{"2,1", ""}));
}

TEST(ColumnarCall, ColumnReaderTakesPrecedence) {
    auto* in = make_frame(2, {string_col({"abc", ""})});
    auto got = read_fixed_result<uint64_t>(columnar_call(in, length_of));
    destroy(in);
    EXPECT_EQ(got, (std::vector<uint64_t>{3, 0}));
}

TEST(ColumnarCall, ExceptionBecomesPanic) {
    auto* in = make_frame(1, {fixed_col<double>({1})});
    EXPECT_THROW(columnar_call(in, throws), WasmPanic);
    destroy(in);
}

TEST(ColumnarCall, TooFewColumnsPanics) {
    auto* in = make_frame(1, {fixed_col<double>({1})});
    EXPECT_THROW(columnar_call(in, add), WasmPanic);
    destroy(in);
}

TEST(ColumnarCall, LowCardinalityStringArgument) {
    // A host-written LowCardinality(String) frame decodes through the same
    // string_view argument as a plain String column.
    auto* in = frame_from_bytes(wire_fixture::LOWCARD_STRING_W1, wire_fixture::LOWCARD_STRING_W1_len);
    auto got = read_string_result(columnar_call(in, upper));
    destroy(in);
    EXPECT_EQ(got, (std::vector<std::string>{"ALPHA", "BETA", "ALPHA", "GAMMA", "BETA", "ALPHA"}));
}

// ── std::optional results → Nullable(T) ─────────────────────────────────────

namespace {

std::optional<int64_t> parse_int(std::string_view s) {
    int64_t v = 0;
    if (s.empty()) return std::nullopt;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}
std::optional<std::string> non_empty(std::string_view s) {
    if (s.empty()) return std::nullopt;
    return std::string(s);
}
std::optional<bool> is_even(int32_t v) { return v % 2 == 0; }

}  // namespace

TEST(ColumnarCall, OptionalFixedResultIsNullable) {
    auto* in = make_frame(4, {string_col({"12", "x", "", "7"}, {0, 0, 0, 1})});
    auto* out = columnar_call(in, parse_int);
    destroy(in);
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    EXPECT_EQ(col.base_type, COL_FIXED64);
    ASSERT_NE(col.null_map, nullptr);
    EXPECT_FALSE(col.is_null(0));
    EXPECT_EQ(col.get_fixed<int64_t>(0), 12);
    EXPECT_TRUE(col.is_null(1));   // nullopt
    EXPECT_TRUE(col.is_null(2));   // nullopt
    EXPECT_TRUE(col.is_null(3));   // NULL argument
    destroy(out);
}

TEST(ColumnarCall, OptionalStringResultIsNullable) {
    auto* in = make_frame(3, {string_col({"a", "", "c"})});
    auto* out = columnar_call(in, non_empty);
    destroy(in);
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    ASSERT_NE(col.null_map, nullptr);
    EXPECT_FALSE(col.is_null(0));
    EXPECT_TRUE(col.is_null(1));
    EXPECT_FALSE(col.is_null(2));
    auto s = col.get_bytes(2);
    EXPECT_EQ(std::string(s.begin(), s.end()), "c");
    destroy(out);
}

TEST(ColumnarCall, OptionalBoolResultIsNullableUInt8) {
    auto* in = make_frame(3, {fixed_col<int32_t>({2, 3, 4}, {0, 0, 1})});
    auto* out = columnar_call(in, is_even);
    destroy(in);
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    EXPECT_EQ(col.base_type, COL_FIXED8);
    EXPECT_EQ(col.get_fixed<uint8_t>(0), 1);
    EXPECT_EQ(col.get_fixed<uint8_t>(1), 0);
    EXPECT_TRUE(col.is_null(2));
    destroy(out);
}

// ── Complex arguments, Variant, Nullable arguments ──────────────────────────
//
// Round trips: a result frame written by write_result_column is a valid input
// frame, so frame(f(x)) fed to g checks the writer and the reader together.

namespace {

std::vector<std::vector<int64_t>> nested(uint32_t n) {
    std::vector<std::vector<int64_t>> out(n);
    for (uint32_t i = 0; i < n; ++i) out[i].assign(i, int64_t(i));
    return out;
}
uint64_t nested_total(std::vector<std::vector<int64_t>> xss) {
    uint64_t t = 0;
    for (auto& xs : xss) for (auto x : xs) t += uint64_t(x);
    return t;
}
std::tuple<std::string, std::optional<int32_t>, std::vector<std::string>> make_tuple3(uint32_t i) {
    return {std::string(i, 'a'), i % 2 ? std::optional<int32_t>(int32_t(i)) : std::nullopt,
            std::vector<std::string>(i, "x")};
}
std::string show_tuple3(std::tuple<std::string, std::optional<int32_t>, std::vector<std::string>> t) {
    auto& [s, o, v] = t;
    return s + "|" + (o ? std::to_string(*o) : "null") + "|" + std::to_string(v.size());
}
std::map<std::string, int64_t> make_map(uint32_t i) {
    std::map<std::string, int64_t> m;
    for (uint32_t k = 0; k < i; ++k) m["k" + std::to_string(k)] = k;
    return m;
}
int64_t map_sum(std::map<std::string, int64_t> m) {
    int64_t s = 0;
    for (auto& [k, v] : m) s += v;
    return s;
}
std::variant<int64_t, std::string> make_variant(uint32_t i) {
    if (i % 2) return std::string(i, 'z');
    return int64_t(i) * 10;
}
std::string show_variant(std::variant<int64_t, std::string> v) {
    return v.index() == 0 ? "i" + std::to_string(std::get<0>(v)) : "s" + std::get<1>(v);
}
std::string show_opt(std::optional<int32_t> v) { return v ? std::to_string(*v) : "null"; }
std::optional<std::pair<int32_t, int32_t>> maybe_pair(int32_t v) {
    if (v < 0) return std::nullopt;
    return std::pair<int32_t, int32_t>{v, -v};
}
int32_t pair_first(std::optional<std::pair<int32_t, int32_t>> p) { return p ? p->first : -1; }

}  // namespace

TEST(ColumnarComplex, NestedArrayRoundTrip) {
    auto* in = make_frame(4, {fixed_col<uint32_t>({0, 1, 2, 3})});
    auto* mid = columnar_call(in, nested);
    destroy(in);
    auto got = read_fixed_result<uint64_t>(columnar_call(mid, nested_total));
    destroy(mid);
    // Row n holds n inner arrays; inner array i holds i copies of i.
    EXPECT_EQ(got, (std::vector<uint64_t>{0, 0, 1, 5}));
}

TEST(ColumnarComplex, TupleWithNullableAndArrayFieldsRoundTrip) {
    auto* in = make_frame(3, {fixed_col<uint32_t>({0, 1, 2})});
    auto* mid = columnar_call(in, make_tuple3);
    destroy(in);
    auto got = read_string_result(columnar_call(mid, show_tuple3));
    destroy(mid);
    EXPECT_EQ(got, (std::vector<std::string>{"|null|0", "a|1|1", "aa|null|2"}));
}

TEST(ColumnarComplex, MapRoundTrip) {
    auto* in = make_frame(3, {fixed_col<uint32_t>({0, 2, 4})});
    auto* mid = columnar_call(in, make_map);
    destroy(in);
    auto got = read_fixed_result<int64_t>(columnar_call(mid, map_sum));
    destroy(mid);
    EXPECT_EQ(got, (std::vector<int64_t>{0, 1, 6}));
}

TEST(ColumnarComplex, TruncatedArrayOffsetsPanic) {
    // Array(Int64) block whose last offset points past the elements.
    ColData c;
    c.col_type = COL_COMPLEX;
    uint64_t offs[2] = {0, 1000};
    c.data.resize(sizeof(offs) + 8);
    std::memcpy(c.data.data(), offs, sizeof(offs));
    auto* in = make_frame(1, {c});
    EXPECT_THROW(columnar_call(in, nested_total), WasmPanic);
    destroy(in);
}

TEST(ColumnarVariant, WrittenVariantReadsBack) {
    auto* in = make_frame(4, {fixed_col<uint32_t>({0, 1, 2, 3}, {0, 0, 0, 1})});
    auto* mid = columnar_call(in, make_variant);
    destroy(in);
    auto cb = parse_columnar(mid);
    auto col = cb.col(0);
    EXPECT_EQ(col.base_type, COL_VARIANT);
    EXPECT_TRUE(col.is_null(3));   // NULL argument → NULL Variant row
    auto got = read_string_result(columnar_call(mid, show_variant));
    destroy(mid);
    EXPECT_EQ(got, (std::vector<std::string>{"i0", "sz", "i20", ""}));
}

TEST(ColumnarVariant, HostFrameDecodesTyped) {
    // Host fixture: rows UInt64(10), String("hi"), UInt64(20), NULL, from a
    // ColumnVariant built with alternatives (UInt64, String) in that order, so
    // discriminator 0 is UInt64.  (A SQL Variant type sorts its alternatives by
    // name; tests/e2e.py covers that order against a server.)
    auto* in = frame_from_bytes(wire_fixture::VARIANT_U64_STRING, wire_fixture::VARIANT_U64_STRING_len);
    auto out = read_string_result(columnar_call(in, +[](std::variant<uint64_t, std::string> v) {
        return v.index() == 1 ? "s:" + std::get<1>(v) : "u:" + std::to_string(std::get<0>(v));
    }));
    destroy(in);
    EXPECT_EQ(out, (std::vector<std::string>{"u:10", "s:hi", "u:20", ""}));
}

TEST(ColumnarNullableArg, OptionalArgumentSeesNull) {
    auto* in = make_frame(3, {fixed_col<int32_t>({4, 0, -2}, {0, 1, 0})});
    auto got = read_string_result(columnar_call(in, show_opt));
    destroy(in);
    EXPECT_EQ(got, (std::vector<std::string>{"4", "null", "-2"}));
}

TEST(ColumnarNullableArg, NullableTupleRoundTrip) {
    auto* in = make_frame(3, {fixed_col<int32_t>({5, -1, 7})});
    auto* mid = columnar_call(in, maybe_pair);
    destroy(in);
    auto cb = parse_columnar(mid);
    auto col = cb.col(0);
    EXPECT_TRUE(col.is_null(1));
    auto got = read_fixed_result<int32_t>(columnar_call(mid, pair_first));
    destroy(mid);
    EXPECT_EQ(got, (std::vector<int32_t>{5, -1, 7}));
}

TEST(ColumnarLowCard, FixedStringDictionaryAsSpan) {
    // A LowCardinality(String) host frame read as std::string through the
    // dictionary path, and the same frame through the generic dictionary view.
    auto* in = frame_from_bytes(wire_fixture::LOWCARD_STRING_W2, wire_fixture::LOWCARD_STRING_W2_len);
    auto cb = parse_columnar(in);
    auto col = cb.col(0);
    auto dict = lowcard_dictionary(col);
    EXPECT_EQ(dict.base_type, COL_BYTES);
    EXPECT_EQ(col_get_arg<std::string>(col, 3), "gamma");
    destroy(in);
}
