// columnar_call: argument decoding, result encoding, NULL handling, and the
// bytes_codec / column_reader customization points.
#include <cmath>
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
