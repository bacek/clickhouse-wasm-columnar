//! The COLUMNAR_V1 frame format: the Rust twin of `include/clickhouse_wasm/wire.h`.
//! A test checks every constant against that header.

pub const FRAME_MAGIC: u32 = 0x4E49_4243; // "CBIN" read as a little-endian u32
pub const FRAME_VERSION: u16 = 1;
pub const HEADER_BYTES: usize = 16;
pub const COL_DESC_BYTES: usize = 40;

pub const COL_BYTES: u64 = 0;
pub const COL_FIXED8: u64 = 1;
pub const COL_FIXED16: u64 = 2;
pub const COL_FIXED32: u64 = 3;
pub const COL_FIXED64: u64 = 4;
pub const COL_COMPLEX: u64 = 5;
pub const COL_VARIANT: u64 = 6;
pub const COL_FIXEDN: u64 = 7;
pub const COL_LOWCARD: u64 = 8;

pub const COL_IS_NULLABLE: u64 = 0x20;
pub const COL_IS_CONST: u64 = 0x80;

pub const VARIANT_NULL_DISCRIMINATOR: u8 = 0xFF;

/// One column descriptor, as laid out on the wire (all fields little-endian).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ColDescriptor {
    pub type_: u64,
    pub null_offset: u64,
    pub offsets_offset: u64,
    pub data_offset: u64,
    pub data_size: u64,
}

impl ColDescriptor {
    /// Decode a descriptor from exactly `COL_DESC_BYTES` bytes.
    pub fn read(b: &[u8]) -> Self {
        let f = |i: usize| u64::from_le_bytes(b[i * 8..i * 8 + 8].try_into().unwrap());
        Self { type_: f(0), null_offset: f(1), offsets_offset: f(2), data_offset: f(3), data_size: f(4) }
    }

    pub fn write(&self, out: &mut [u8]) {
        for (i, v) in [self.type_, self.null_offset, self.offsets_offset, self.data_offset, self.data_size]
            .into_iter().enumerate() {
            out[i * 8..i * 8 + 8].copy_from_slice(&v.to_le_bytes());
        }
    }
}
