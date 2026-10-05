#pragma once

#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "clickhouse_wasm/columnar.hpp"

// Raised by the native clickhouse_throw stub (host_stubs.cpp).
struct WasmPanic : std::exception {
    explicit WasmPanic(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
    std::string msg;
};

namespace test {

using namespace ch;

// One input column for make_frame().
struct ColData {
    uint32_t              col_type;   // ColType | flags
    std::vector<uint8_t>  null_map;   // non-empty → null map present
    std::vector<uint64_t> offsets;    // non-empty → offsets array present
    std::vector<uint8_t>  data;
};

// Assemble a COLUMNAR_V1 frame from per-column parts.
inline raw_buffer* make_frame(uint32_t num_rows, const std::vector<ColData>& cols) {
    uint32_t pos = HEADER_BYTES + static_cast<uint32_t>(cols.size()) * COL_DESC_BYTES;

    struct Placement { uint32_t null_off, offsets_off, data_off, data_sz; };
    std::vector<Placement> at;
    for (const auto& col : cols) {
        Placement b{};
        if (!col.null_map.empty()) {
            b.null_off = pos;
            pos += static_cast<uint32_t>(col.null_map.size());
        }
        if (!col.offsets.empty()) {
            pos = (pos + 7u) & ~7u;
            b.offsets_off = pos;
            pos += static_cast<uint32_t>(col.offsets.size()) * 8u;
        }
        b.data_off = pos;
        b.data_sz  = static_cast<uint32_t>(col.data.size());
        pos += b.data_sz;
        at.push_back(b);
    }

    auto* buf = clickhouse_create_buffer(pos);
    uint8_t* p = buf->data();
    std::memset(p, 0, pos);
    write_frame_header(p, num_rows, static_cast<uint32_t>(cols.size()));

    for (size_t i = 0; i < cols.size(); ++i) {
        ColDescriptor d{};
        d.type           = cols[i].col_type;
        d.null_offset    = at[i].null_off;
        d.offsets_offset = at[i].offsets_off;
        d.data_offset    = at[i].data_off;
        d.data_size      = at[i].data_sz;
        std::memcpy(p + HEADER_BYTES + i * COL_DESC_BYTES, &d, sizeof(d));
        if (!cols[i].null_map.empty())
            std::memcpy(p + at[i].null_off, cols[i].null_map.data(), cols[i].null_map.size());
        if (!cols[i].offsets.empty())
            std::memcpy(p + at[i].offsets_off, cols[i].offsets.data(), cols[i].offsets.size() * 8);
        if (!cols[i].data.empty())
            std::memcpy(p + at[i].data_off, cols[i].data.data(), cols[i].data.size());
    }
    return buf;
}

// String column; a row listed in `nulls` (non-zero) is NULL.
inline ColData string_col(const std::vector<std::string>& rows,
                          const std::vector<uint8_t>& nulls = {},
                          bool is_const = false) {
    ColData c;
    c.col_type = COL_BYTES | (nulls.empty() ? 0u : uint32_t(COL_IS_NULLABLE))
                           | (is_const ? uint32_t(COL_IS_CONST) : 0u);
    c.null_map = nulls;
    c.offsets.push_back(0);
    for (const auto& r : rows) {
        c.data.insert(c.data.end(), r.begin(), r.end());
        c.offsets.push_back(c.data.size());
    }
    return c;
}

template <typename T>
ColData fixed_col(const std::vector<T>& vals, const std::vector<uint8_t>& nulls = {},
                  bool is_const = false) {
    ColData c;
    c.col_type = fixed_col_tag<T>() | (nulls.empty() ? 0u : uint32_t(COL_IS_NULLABLE))
                                    | (is_const ? uint32_t(COL_IS_CONST) : 0u);
    c.null_map = nulls;
    c.data.resize(vals.size() * sizeof(T));
    std::memcpy(c.data.data(), vals.data(), c.data.size());
    return c;
}

// Copy raw frame bytes (e.g. a host-written fixture) into a guest buffer.
inline raw_buffer* frame_from_bytes(const uint8_t* bytes, size_t n) {
    raw_buffer* buf = clickhouse_create_buffer(static_cast<uint32_t>(n));
    std::memcpy(buf->data(), bytes, n);
    return buf;
}

inline ColDescriptor desc_at(const uint8_t* frame, uint32_t col) {
    ColDescriptor d;
    std::memcpy(&d, frame + HEADER_BYTES + col * COL_DESC_BYTES, sizeof(d));
    return d;
}

inline void destroy(raw_buffer* buf) {
    clickhouse_destroy_buffer(reinterpret_cast<uint8_t*>(buf));
}

// Fixed-width result column → values; frees the buffer.
template <typename T>
std::vector<T> read_fixed_result(raw_buffer* out) {
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    std::vector<T> res(cb.num_rows);
    for (uint32_t i = 0; i < cb.num_rows; ++i) res[i] = col.get_fixed<T>(i);
    destroy(out);
    return res;
}

// String result column → values; frees the buffer.
inline std::vector<std::string> read_string_result(raw_buffer* out) {
    auto cb = parse_columnar(out);
    auto col = cb.col(0);
    std::vector<std::string> res;
    for (uint32_t i = 0; i < cb.num_rows; ++i) {
        auto s = col.get_bytes(i);
        res.emplace_back(s.begin(), s.end());
    }
    destroy(out);
    return res;
}

inline void expect_span_eq(std::span<const uint8_t> s, std::string_view want) {
    EXPECT_EQ(std::string(s.begin(), s.end()), std::string(want));
}

}  // namespace test
