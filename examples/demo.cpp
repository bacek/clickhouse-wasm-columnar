// Example COLUMNAR_V1 UDFs.  Build with Emscripten (see examples/CMakeLists.txt),
// then load with examples/demo.sql or run tests/e2e.py against a server.
#include <clickhouse_wasm/columnar.hpp>

#include <cstdint>
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

}  // namespace

CH_COLUMNAR_UDF(demo_repeat, repeat)
CH_COLUMNAR_UDF(demo_parse_uint, parse_uint)
CH_COLUMNAR_UDF(demo_array_mean, array_mean)
CH_COLUMNAR_UDF(demo_split, split)
CH_COLUMNAR_UDF(demo_divmod, divmod)
