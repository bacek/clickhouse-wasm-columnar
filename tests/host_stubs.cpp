// Native stand-ins for the ClickHouse host imports.  clickhouse_throw raises
// WasmPanic so tests can assert that a failure went through ch::panic().
#include <cstdint>
#include <string>

#include "helpers.hpp"

extern "C" {
  void clickhouse_log(uint32_t, const uint8_t *, uint32_t) {}
  void clickhouse_throw(const uint8_t *msg, uint32_t len) {
    throw WasmPanic(std::string(reinterpret_cast<const char *>(msg), len));
  }
  void clickhouse_random(uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(i ^ 0xa5u);
  }
}
