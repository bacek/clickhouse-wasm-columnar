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
pub mod variant;
pub mod wire;

pub use arg::{read_arg, Arg, Elem};
pub use frame::{ColView, Frame};
pub use ret::{Bytes, ElemOut, NullableRet, Ret};

/// Run `f` over every row of an input frame and return the result frame.
/// `columnar_udf!` calls this; it is public for tests and custom exports.
pub fn call_frame<'a, R: Ret>(
    input: &'a [u8],
    nargs: u32,
    mut row_fn: impl FnMut(&[ColView<'a>], u32) -> frame::Result<Option<R>>,
) -> Result<Vec<u8>, String> {
    let frame = Frame::parse(input).map_err(|e| e.to_string())?;
    if frame.num_cols() < nargs {
        return Err("columnar: frame has fewer columns than the function has arguments".into());
    }
    let cols = (0..nargs).map(|i| frame.col(i)).collect::<frame::Result<Vec<_>>>().map_err(|e| e.to_string())?;
    let mut rows = Vec::with_capacity(frame.num_rows() as usize);
    for row in 0..frame.num_rows() {
        rows.push(row_fn(&cols, row).map_err(|e| e.to_string())?);
    }
    R::write_column(rows)
}

/// Export a function as a `COLUMNAR_V1` UDF with the same name.
///
/// ```ignore
/// columnar_udf! {
///     fn demo_repeat(s: &str, n: u32) -> String { s.repeat(n as usize) }
/// }
/// ```
///
/// This defines a module `demo_repeat` with the function (`demo_repeat::imp`)
/// and `demo_repeat::call(&[u8])`, which runs it over a frame, and on wasm32
/// the exported entry point `demo_repeat`.
#[macro_export]
macro_rules! columnar_udf {
    ($(#[$m:meta])* fn $name:ident ( $($arg:ident : $ty:ty),* $(,)? ) -> $ret:ty $body:block) => {
        #[allow(non_snake_case, dead_code)]
        pub mod $name {
            #[allow(unused_imports)]
            use super::*;

            $(#[$m])*
            pub fn imp($($arg: $ty),*) -> $ret $body

            /// Run the function over every row of `input`.
            pub fn call(input: &[u8]) -> ::core::result::Result<::std::vec::Vec<u8>, ::std::string::String> {
                const NARGS: u32 = [$(stringify!($arg)),*].len() as u32;
                $crate::call_frame::<$ret>(input, NARGS, |_cols, _row| {
                    let mut _it = _cols.iter();
                    $( let $arg = _it.next().unwrap(); )*
                    if false $(|| (!<$ty as $crate::Arg<'_>>::TAKES_NULL && $arg.is_null(_row)))* {
                        return Ok(None);
                    }
                    $( let $arg: $ty = $crate::read_arg::<$ty>($arg, _row)?; )*
                    Ok(Some(imp($($arg),*)))
                })
            }
        }

        #[cfg(target_arch = "wasm32")]
        #[no_mangle]
        pub unsafe extern "C" fn $name(input: *mut $crate::abi::RawBuffer, _rows: u32) -> *mut $crate::abi::RawBuffer {
            $crate::abi::run(input, $name::call)
        }
    };
}
