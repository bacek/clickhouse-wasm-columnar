/* ClickHouse COLUMNAR_V1 wire format: constants and structs only.
 *
 * Plain C99, no dependencies, so modules in any language can port it and C
 * test modules can include it. SPEC.md describes the format in full; the C++
 * library (columnar.hpp) checks its own constants against this file at
 * compile time.
 *
 * All integers are little-endian. Offsets are byte offsets from the start of
 * the frame.
 */
#ifndef CLICKHOUSE_WASM_WIRE_H
#define CLICKHOUSE_WASM_WIRE_H

#include <stdint.h>

#define CHW_FRAME_MAGIC    0x4E494243u /* "CBIN" read as a little-endian u32 */
#define CHW_FRAME_VERSION  1u
#define CHW_HEADER_BYTES   16u
#define CHW_COL_DESC_BYTES 40u

/* Base type tags: low bits of ColDescriptor.type. */
#define CHW_COL_BYTES    0u /* String: u64 offsets[rows+1] + chars           */
#define CHW_COL_FIXED8   1u /* 1-byte values                                 */
#define CHW_COL_FIXED16  2u /* 2-byte values                                 */
#define CHW_COL_FIXED32  3u /* 4-byte values                                 */
#define CHW_COL_FIXED64  4u /* 8-byte values                                 */
#define CHW_COL_COMPLEX  5u /* Array / Tuple / Map / nested Nullable          */
#define CHW_COL_VARIANT  6u /* Variant(...)                                  */
#define CHW_COL_FIXEDN   7u /* fixed width not in {1,2,4,8}: data_size / rows */
#define CHW_COL_LOWCARD  8u /* LowCardinality(T): index + embedded dictionary */

/* Flags OR'd onto the base tag. */
#define CHW_COL_IS_NULLABLE 0x20u /* null_offset -> u8 null map[rows]         */
#define CHW_COL_IS_CONST    0x80u /* one stored row, broadcast to all rows    */

/* Variant discriminator meaning NULL. */
#define CHW_VARIANT_NULL_DISCRIMINATOR 0xFFu

/* 16 bytes at offset 0. */
typedef struct chw_frame_header {
    uint32_t magic;    /* CHW_FRAME_MAGIC   */
    uint16_t version;  /* CHW_FRAME_VERSION */
    uint16_t reserved; /* must be 0         */
    uint32_t num_rows;
    uint32_t num_cols;
} chw_frame_header;

/* num_cols descriptors follow the header, 40 bytes each. */
typedef struct chw_col_descriptor {
    uint64_t type;           /* base tag | flags                        */
    uint64_t null_offset;    /* null map / Variant discriminators       */
    uint64_t offsets_offset; /* String/Array offsets, Variant row offs,
                                LowCardinality index                    */
    uint64_t data_offset;
    uint64_t data_size;
} chw_col_descriptor;

#if defined(__cplusplus)
static_assert(sizeof(chw_frame_header) == CHW_HEADER_BYTES, "frame header size");
static_assert(sizeof(chw_col_descriptor) == CHW_COL_DESC_BYTES, "descriptor size");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(chw_frame_header) == CHW_HEADER_BYTES, "frame header size");
_Static_assert(sizeof(chw_col_descriptor) == CHW_COL_DESC_BYTES, "descriptor size");
#endif

#endif /* CLICKHOUSE_WASM_WIRE_H */
