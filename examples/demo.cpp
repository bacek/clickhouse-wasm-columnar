// Example COLUMNAR_V1 UDFs.  Build with Emscripten (see examples/CMakeLists.txt),
// then load with examples/demo.sql or run tests/e2e.py against a server.
#include <clickhouse_wasm/columnar.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <variant>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// String × UInt32 → String
std::string repeat(std::string_view s, uint32_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (uint32_t i = 0; i < n; ++i) out += s;
    return out;
}

// String → Nullable(Int64): NULL when the text is not a non-negative integer.
std::optional<int64_t> parse_uint(std::string_view s) {
    if (s.empty() || s.size() > 18) return std::nullopt;
    int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

// Array(Float64) → Float64
double array_mean(std::vector<double> xs) {
    if (xs.empty()) return 0.0;
    double sum = 0;
    for (double x : xs) sum += x;
    return sum / double(xs.size());
}

// String → Array(String), split on a single-character separator.
std::vector<std::string> split(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    if (sep.size() != 1) { out.emplace_back(s); return out; }
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep[0]) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// Int64 × Int64 → Tuple(Int64, Int64): quotient and remainder.
std::pair<int64_t, int64_t> divmod(int64_t a, int64_t b) {
    if (b == 0) ch::panic("divmod: division by zero");
    return {a / b, a % b};
}

// Tuple(Int64, Float64) → Float64
double tuple_sum(std::pair<int64_t, double> t) { return double(t.first) + t.second; }

// Array(Array(Int64)) → Array(Int64)
std::vector<int64_t> flatten(std::vector<std::vector<int64_t>> xss) {
    std::vector<int64_t> out;
    for (auto& xs : xss) out.insert(out.end(), xs.begin(), xs.end());
    return out;
}

// Array(Nullable(String)) → Array(Nullable(String)): upper-cases, keeps NULLs.
std::vector<std::optional<std::string>> upper_all(std::vector<std::optional<std::string>> xs) {
    for (auto& x : xs)
        if (x) for (auto& c : *x) if (c >= 'a' && c <= 'z') c = char(c - 32);
    return xs;
}

// Map(String, Int64) × String → Nullable(Int64)
std::optional<int64_t> map_get(std::map<std::string, int64_t> m, std::string_view key) {
    auto it = m.find(std::string(key));
    if (it == m.end()) return std::nullopt;
    return it->second;
}

// Map(String, Int64) → Map(Int64, String)
std::map<int64_t, std::string> map_invert(std::map<std::string, int64_t> m) {
    std::map<int64_t, std::string> out;
    for (auto& [k, v] : m) out[v] = k;
    return out;
}

// Variant(Int64, String) → String.  Alternatives in ClickHouse's order (by type name).
std::string describe(std::variant<int64_t, std::string> v) {
    if (v.index() == 0) return "int:" + std::to_string(std::get<0>(v));
    return "str:" + std::get<1>(v);
}

// String → Variant(Int64, String)
std::variant<int64_t, std::string> classify(std::string_view s) {
    if (auto n = parse_uint(s)) return *n;
    return std::string(s);
}

// Nullable(Int64) → String: the argument may be NULL.
std::string show_nullable(std::optional<int64_t> v) {
    return v ? std::to_string(*v) : std::string("null");
}

// Int64 → Nullable(Tuple(Int64, Int64)): NULL for negative input.
std::optional<std::pair<int64_t, int64_t>> halves(int64_t v) {
    if (v < 0) return std::nullopt;
    return std::pair<int64_t, int64_t>{v / 2, v - v / 2};
}

// UUID → String: the 16 raw bytes as hex, as stored.
std::string uuid_bytes(std::array<uint8_t, 16> u) {
    std::string out;
    char buf[3];
    for (uint8_t b : u) { std::snprintf(buf, sizeof(buf), "%02x", b); out += buf; }
    return out;
}

// LowCardinality(FixedString(3)) → String
std::string lc_fixed(std::string_view s) { return std::string(s) + "!"; }

// LowCardinality(UInt32) → UInt64
uint64_t lc_double(uint32_t v) { return uint64_t(v) * 2u; }

}  // namespace

CH_COLUMNAR_UDF(demo_repeat, repeat)
CH_COLUMNAR_UDF(demo_parse_uint, parse_uint)
CH_COLUMNAR_UDF(demo_array_mean, array_mean)
CH_COLUMNAR_UDF(demo_split, split)
CH_COLUMNAR_UDF(demo_divmod, divmod)
CH_COLUMNAR_UDF(demo_tuple_sum, tuple_sum)
CH_COLUMNAR_UDF(demo_flatten, flatten)
CH_COLUMNAR_UDF(demo_upper_all, upper_all)
CH_COLUMNAR_UDF(demo_map_get, map_get)
CH_COLUMNAR_UDF(demo_map_invert, map_invert)
CH_COLUMNAR_UDF(demo_describe, describe)
CH_COLUMNAR_UDF(demo_classify, classify)
CH_COLUMNAR_UDF(demo_show_nullable, show_nullable)
CH_COLUMNAR_UDF(demo_halves, halves)
CH_COLUMNAR_UDF(demo_uuid_bytes, uuid_bytes)
CH_COLUMNAR_UDF(demo_lc_fixed, lc_fixed)
CH_COLUMNAR_UDF(demo_lc_double, lc_double)
