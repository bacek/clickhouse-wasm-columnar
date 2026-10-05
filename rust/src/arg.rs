//! Reading function arguments from input columns.
use crate::frame::{ColTag, ColView, Error, Result};
use std::collections::{BTreeMap, HashMap};
use std::hash::Hash;

fn err<T>(msg: &'static str) -> Result<T> {
    Err(Error(msg))
}

/// A type that can be read from one row of an argument column.
pub trait Arg<'a>: Sized {
    /// True if the type receives NULL itself (`Option`); otherwise a NULL in
    /// this argument skips the row.
    const TAKES_NULL: bool = false;
    fn read(col: &ColView<'a>, row: u32) -> Result<Self>;
}

/// Read an argument, looking LowCardinality values up in their dictionary.
pub fn read_arg<'a, T: Arg<'a>>(col: &ColView<'a>, row: u32) -> Result<T> {
    if !T::TAKES_NULL && col.tag() == ColTag::LowCard {
        let i = col.dict_index(row)?;
        return T::read(col.dictionary().unwrap(), i);
    }
    T::read(col, row)
}

impl<'a, T: Arg<'a>> Arg<'a> for Option<T> {
    const TAKES_NULL: bool = true;
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        if col.is_null(row) {
            return Ok(None);
        }
        read_arg::<T>(col, row).map(Some)
    }
}

// Integers: the wire carries only a width, so a narrower column is widened to
// the declared type (the same rules as the C++ library).
macro_rules! int_arg {
    ($($t:ty),*) => {$(
        impl<'a> Arg<'a> for $t {
            fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
                let b = col.fixed_bytes(row)?;
                Ok(match col.tag() {
                    ColTag::Fixed8 => b[0] as $t,
                    ColTag::Fixed16 => i16::from_le_bytes([b[0], b[1]]) as $t,
                    ColTag::Fixed32 => u32::from_le_bytes(b.try_into().unwrap()) as $t,
                    ColTag::Fixed64 => u64::from_le_bytes(b.try_into().unwrap()) as $t,
                    _ => match b.try_into() {
                        Ok(a) => <$t>::from_le_bytes(a),
                        Err(_) => return err("fixed-width column does not match the argument's size"),
                    },
                })
            }
        }
    )*};
}
int_arg!(i8, i16, i32, i64, u8, u16, u32, u64, i128, u128);

impl<'a> Arg<'a> for f64 {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        let b = col.fixed_bytes(row)?;
        Ok(match col.tag() {
            ColTag::Fixed64 => f64::from_le_bytes(b.try_into().unwrap()),
            ColTag::Fixed8 => b[0] as f64,
            ColTag::Fixed16 => i16::from_le_bytes([b[0], b[1]]) as f64,
            ColTag::Fixed32 => u32::from_le_bytes(b.try_into().unwrap()) as f64,
            _ => return err("Float64 argument against a column that is not 8 bytes wide"),
        })
    }
}

impl<'a> Arg<'a> for f32 {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        let b = col.fixed_bytes(row)?;
        Ok(match col.tag() {
            ColTag::Fixed32 => f32::from_le_bytes(b.try_into().unwrap()),
            ColTag::Fixed8 => b[0] as f32,
            ColTag::Fixed16 => i16::from_le_bytes([b[0], b[1]]) as f32,
            _ => return err("Float32 argument against a column that is not 4 bytes wide"),
        })
    }
}

impl<'a> Arg<'a> for bool {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        Ok(col.fixed_bytes(row)?.iter().any(|&b| b != 0))
    }
}

impl<'a, const N: usize> Arg<'a> for [u8; N] {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        col.fixed_bytes(row)?
            .try_into()
            .map_err(|_| Error("fixed-width column does not match the argument's size"))
    }
}

impl<'a> Arg<'a> for &'a [u8] {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        match col.tag() {
            ColTag::Bytes => col.bytes(row),
            _ => col.fixed_bytes(row),
        }
    }
}

impl<'a> Arg<'a> for &'a str {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        std::str::from_utf8(<&[u8]>::read(col, row)?)
            .map_err(|_| Error("String argument is not valid UTF-8; take &[u8] instead"))
    }
}

impl<'a> Arg<'a> for String {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        <&str>::read(col, row).map(str::to_owned)
    }
}

fn complex_arg<'a, T: Elem<'a>>(col: &ColView<'a>, row: u32) -> Result<T> {
    if col.tag() != ColTag::Complex {
        return err("Array/Tuple/Map argument against a column that is not COL_COMPLEX");
    }
    let r = col.stored_row(row)?;
    T::get(col.data(), col.stored_rows() as u64, r as u64)
}

impl<'a, T: Elem<'a>> Arg<'a> for Vec<T> {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        complex_arg(col, row)
    }
}

impl<'a, K: Elem<'a> + Ord, V: Elem<'a>> Arg<'a> for BTreeMap<K, V> {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        complex_arg(col, row)
    }
}

impl<'a, K: Elem<'a> + Eq + Hash, V: Elem<'a>> Arg<'a> for HashMap<K, V> {
    fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
        complex_arg(col, row)
    }
}

// ── Elements of a COL_COMPLEX block (see SPEC.md, "Complex") ────────────────

/// A value stored inside a complex block: an Array element or Tuple field.
pub trait Elem<'a>: Sized {
    /// Bytes taken by a block of `n` values at the start of `b`.
    fn size(b: &'a [u8], n: u64) -> Result<u64>;
    /// Value `i` of a block of `n` values at the start of `b`.
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self>;
}

fn from(b: &[u8], off: u64) -> Result<&[u8]> {
    if off > b.len() as u64 {
        return err("complex block extends past its column data");
    }
    Ok(&b[off as usize..])
}

fn u64_at(b: &[u8], i: u64) -> Result<u64> {
    if i >= b.len() as u64 / 8 {
        return err("complex offsets extend past the column data");
    }
    let i = i as usize * 8;
    Ok(u64::from_le_bytes(b[i..i + 8].try_into().unwrap()))
}

fn check_index(n: u64, i: u64) -> Result<()> {
    if i >= n {
        return err("complex row index out of range");
    }
    Ok(())
}

macro_rules! fixed_elem {
    ($($t:ty),*) => {$(
        impl<'a> Elem<'a> for $t {
            fn size(b: &'a [u8], n: u64) -> Result<u64> {
                const W: u64 = std::mem::size_of::<$t>() as u64;
                if n > b.len() as u64 / W {
                    return err("fixed-width block extends past the column data");
                }
                Ok(n * W)
            }
            fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
                check_index(n, i)?;
                Self::size(b, n)?;
                const W: usize = std::mem::size_of::<$t>();
                let o = i as usize * W;
                Ok(<$t>::from_le_bytes(b[o..o + W].try_into().unwrap()))
            }
        }
    )*};
}
fixed_elem!(i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, f32, f64);

impl<'a> Elem<'a> for bool {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        u8::size(b, n)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        u8::get(b, n, i).map(|v| v != 0)
    }
}

impl<'a, const N: usize> Elem<'a> for [u8; N] {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        if N == 0 || n > b.len() as u64 / N as u64 {
            return err("fixed-width block extends past the column data");
        }
        Ok(n * N as u64)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        check_index(n, i)?;
        Self::size(b, n)?;
        let o = i as usize * N;
        Ok(b[o..o + N].try_into().unwrap())
    }
}

impl<'a> Elem<'a> for &'a [u8] {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        let chars = u64_at(b, n)?;
        let head = (n + 1) * 8;
        if chars > b.len() as u64 - head {
            return err("string block extends past the column data");
        }
        Ok(head + chars)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        check_index(n, i)?;
        let (s, e) = (u64_at(b, i)?, u64_at(b, i + 1)?);
        let chars = from(b, (n + 1) * 8)?;
        if s > e || e > chars.len() as u64 {
            return err("string offsets out of order or past the data");
        }
        Ok(&chars[s as usize..e as usize])
    }
}

impl<'a> Elem<'a> for &'a str {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        <&[u8]>::size(b, n)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        std::str::from_utf8(<&[u8]>::get(b, n, i)?)
            .map_err(|_| Error("String element is not valid UTF-8"))
    }
}

impl<'a> Elem<'a> for String {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        <&[u8]>::size(b, n)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        <&str>::get(b, n, i).map(str::to_owned)
    }
}

impl<'a, E: Elem<'a>> Elem<'a> for Vec<E> {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        let m = u64_at(b, n)?;
        let head = (n + 1) * 8;
        Ok(head + E::size(from(b, head)?, m)?)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        check_index(n, i)?;
        let m = u64_at(b, n)?;
        let (s, e) = (u64_at(b, i)?, u64_at(b, i + 1)?);
        if s > e || e > m {
            return err("array offsets out of order or past the elements");
        }
        let inner = from(b, (n + 1) * 8)?;
        (s..e).map(|j| E::get(inner, m, j)).collect()
    }
}

impl<'a, E: Elem<'a>> Elem<'a> for Option<E> {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        if n > b.len() as u64 {
            return err("null map extends past the column data");
        }
        Ok(n + E::size(from(b, n)?, n)?)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        check_index(n, i)?;
        if n > b.len() as u64 {
            return err("null map extends past the column data");
        }
        if b[i as usize] != 0 {
            return Ok(None);
        }
        E::get(from(b, n)?, n, i).map(Some)
    }
}

impl<'a, K: Elem<'a> + Ord, V: Elem<'a>> Elem<'a> for BTreeMap<K, V> {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        Vec::<(K, V)>::size(b, n)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        Ok(Vec::<(K, V)>::get(b, n, i)?.into_iter().collect())
    }
}

impl<'a, K: Elem<'a> + Eq + Hash, V: Elem<'a>> Elem<'a> for HashMap<K, V> {
    fn size(b: &'a [u8], n: u64) -> Result<u64> {
        Vec::<(K, V)>::size(b, n)
    }
    fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
        Ok(Vec::<(K, V)>::get(b, n, i)?.into_iter().collect())
    }
}

/// The block of field `T` at the start of `rest`; moves `rest` past it.
fn next_field<'a, T: Elem<'a>>(rest: &mut &'a [u8], n: u64) -> Result<&'a [u8]> {
    let field = *rest;
    *rest = from(field, T::size(field, n)?)?;
    Ok(field)
}

// Tuples: field blocks one after another.
macro_rules! tuple_impls {
    ($($T:ident),+) => {
        impl<'a, $($T: Elem<'a>),+> Elem<'a> for ($($T,)+) {
            fn size(b: &'a [u8], n: u64) -> Result<u64> {
                let mut off = 0u64;
                $( off += $T::size(from(b, off)?, n)?; )+
                Ok(off)
            }
            fn get(b: &'a [u8], n: u64, i: u64) -> Result<Self> {
                check_index(n, i)?;
                let mut rest = b;
                Ok(($($T::get(next_field::<$T>(&mut rest, n)?, n, i)?,)+))
            }
        }

        impl<'a, $($T: Elem<'a>),+> Arg<'a> for ($($T,)+) {
            fn read(col: &ColView<'a>, row: u32) -> Result<Self> {
                complex_arg(col, row)
            }
        }
    };
}
tuple_impls!(A);
tuple_impls!(A, B);
tuple_impls!(A, B, C);
tuple_impls!(A, B, C, D);
tuple_impls!(A, B, C, D, E);
tuple_impls!(A, B, C, D, E, F);
tuple_impls!(A, B, C, D, E, F, G);
tuple_impls!(A, B, C, D, E, F, G, H);
