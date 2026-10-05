//! Write ClickHouse WebAssembly UDFs with the `COLUMNAR_V1` ABI.
//!
//! ```ignore
//! use clickhouse_wasm_columnar::columnar_udf;
//!
//! columnar_udf! {
//!     fn repeat_string(s: &str, n: u32) -> String { s.repeat(n as usize) }
//! }
//! ```
//!
//! Argument and result types map to ClickHouse types as described in the
//! README; the frame format is in SPEC.md.
pub mod abi;
pub mod arg;
pub mod frame;
pub mod ret;
pub mod udf;
pub mod variant;
pub mod wire;

pub use arg::{read_arg, Arg, Elem};
pub use frame::{ColView, Frame};
pub use ret::{Bytes, ElemOut, NullableRet, Ret};
pub use udf::call_frame;
