//! Writing the result column.
use crate::wire::*;
use std::collections::{BTreeMap, HashMap};
use std::fmt::Display;

/// Raw bytes as a `String` result (a `Vec<u8>` is an `Array(UInt8)`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Bytes(pub Vec<u8>);

/// A type the function can return.
///
/// `rows[i]` is `None` when an argument of row `i` was NULL and the function
/// was not called.
pub trait Ret: Sized {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String>;
}

fn align8(x: usize) -> usize {
    (x + 7) & !7
}

/// A one-column frame: header, `desc`, then `body` placed at offset 56.
pub(crate) fn single_col_frame(rows: usize, desc: ColDescriptor, body: &[u8]) -> Result<Vec<u8>, String> {
    let rows = u32::try_from(rows).map_err(|_| "columnar: too many result rows".to_string())?;
    let start = HEADER_BYTES + COL_DESC_BYTES;
    if (start + body.len()) as u64 > u32::MAX as u64 {
        return Err("columnar: result frame exceeds 4 GiB".into());
    }
    let mut out = vec![0u8; start];
    out[0..4].copy_from_slice(&FRAME_MAGIC.to_le_bytes());
    out[4..6].copy_from_slice(&FRAME_VERSION.to_le_bytes());
    out[8..12].copy_from_slice(&rows.to_le_bytes());
    out[12..16].copy_from_slice(&1u32.to_le_bytes());
    desc.write(&mut out[HEADER_BYTES..start]);
    out.extend_from_slice(body);
    Ok(out)
}

const BODY: u64 = (HEADER_BYTES + COL_DESC_BYTES) as u64;

fn fixed_tag(width: usize) -> u64 {
    match width {
        1 => COL_FIXED8,
        2 => COL_FIXED16,
        4 => COL_FIXED32,
        8 => COL_FIXED64,
        _ => COL_FIXEDN,
    }
}

/// A fixed-width value: written as its little-endian bytes.
pub trait FixedValue: Sized {
    const WIDTH: usize;
    /// Value written for a row whose function call was skipped.
    fn skipped() -> Self;
    fn put(&self, out: &mut Vec<u8>);
}

macro_rules! fixed_value {
    ($($t:ty => $skip:expr),*) => {$(
        impl FixedValue for $t {
            const WIDTH: usize = std::mem::size_of::<$t>();
            fn skipped() -> Self { $skip }
            fn put(&self, out: &mut Vec<u8>) { out.extend_from_slice(&self.to_le_bytes()); }
        }
    )*};
}
fixed_value!(i8 => 0, i16 => 0, i32 => 0, i64 => 0, i128 => 0,
             u8 => 0, u16 => 0, u32 => 0, u64 => 0, u128 => 0,
             f32 => f32::NAN, f64 => f64::NAN);

impl FixedValue for bool {
    const WIDTH: usize = 1;
    fn skipped() -> Self { false }
    fn put(&self, out: &mut Vec<u8>) { out.push(*self as u8); }
}

impl<const N: usize> FixedValue for [u8; N] {
    const WIDTH: usize = N;
    fn skipped() -> Self { [0; N] }
    fn put(&self, out: &mut Vec<u8>) { out.extend_from_slice(self); }
}

fn write_fixed<T: FixedValue>(rows: Vec<Option<T>>) -> Result<Vec<u8>, String> {
    let mut data = Vec::with_capacity(rows.len() * T::WIDTH);
    for r in &rows {
        match r {
            Some(v) => v.put(&mut data),
            None => T::skipped().put(&mut data),
        }
    }
    let desc = ColDescriptor { type_: fixed_tag(T::WIDTH), data_offset: BODY, data_size: data.len() as u64, ..Default::default() };
    single_col_frame(rows.len(), desc, &data)
}

fn write_nullable_fixed<T: FixedValue>(rows: Vec<Option<T>>) -> Result<Vec<u8>, String> {
    let n = rows.len();
    let data_off = align8(BODY as usize + n);
    let mut body = vec![0u8; data_off - BODY as usize];
    for (i, r) in rows.iter().enumerate() {
        body[i] = r.is_none() as u8;
    }
    for r in &rows {
        match r {
            Some(v) => v.put(&mut body),
            None => body.extend(std::iter::repeat(0).take(T::WIDTH)),
        }
    }
    let desc = ColDescriptor {
        type_: fixed_tag(T::WIDTH) | COL_IS_NULLABLE,
        null_offset: BODY,
        data_offset: data_off as u64,
        data_size: (n * T::WIDTH) as u64,
        ..Default::default()
    };
    single_col_frame(n, desc, &body)
}

/// `rows` as a String column; `None` rows are empty, and NULL if `nullable`.
fn write_bytes<B: AsRef<[u8]>>(rows: &[Option<B>], nullable: bool) -> Result<Vec<u8>, String> {
    let n = rows.len();
    let null_len = if nullable { n } else { 0 };
    let offs_off = align8(BODY as usize + null_len);
    let data_off = offs_off + (n + 1) * 8;
    let mut body = vec![0u8; offs_off - BODY as usize];
    if nullable {
        for (i, r) in rows.iter().enumerate() {
            body[i] = r.is_none() as u8;
        }
    }
    let mut pos = 0u64;
    body.extend_from_slice(&pos.to_le_bytes());
    for r in rows {
        pos += r.as_ref().map_or(0, |b| b.as_ref().len()) as u64;
        body.extend_from_slice(&pos.to_le_bytes());
    }
    for r in rows.iter().flatten() {
        body.extend_from_slice(r.as_ref());
    }
    let desc = ColDescriptor {
        type_: COL_BYTES | if nullable { COL_IS_NULLABLE } else { 0 },
        null_offset: if nullable { BODY } else { 0 },
        offsets_offset: offs_off as u64,
        data_offset: data_off as u64,
        data_size: pos,
    };
    single_col_frame(n, desc, &body)
}

/// `rows` as a COL_COMPLEX column; with `nulls` the column is `Nullable`.
fn write_complex<T: ElemOut>(rows: Vec<T>, nulls: Option<Vec<u8>>) -> Result<Vec<u8>, String> {
    let n = rows.len();
    let mut body = Vec::new();
    let mut desc = ColDescriptor { type_: COL_COMPLEX, data_offset: BODY, ..Default::default() };
    if let Some(nulls) = nulls {
        desc.type_ |= COL_IS_NULLABLE;
        desc.null_offset = BODY;
        desc.data_offset = align8(BODY as usize + n) as u64;
        body.extend_from_slice(&nulls);
        body.resize(desc.data_offset as usize - BODY as usize, 0);
    }
    let start = body.len();
    T::write_block(&mut body, rows);
    desc.data_size = (body.len() - start) as u64;
    single_col_frame(n, desc, &body)
}

macro_rules! fixed_ret {
    ($($t:ty),*) => {$(
        impl Ret for $t {
            fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_fixed(rows) }
        }
        impl NullableRet for $t {
            fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_nullable_fixed(rows) }
        }
    )*};
}
fixed_ret!(i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, f32, f64, bool);

impl<const N: usize> Ret for [u8; N] {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_fixed(rows) }
}
impl<const N: usize> NullableRet for [u8; N] {
    fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_nullable_fixed(rows) }
}

impl Ret for String {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_bytes(&rows, false) }
}
impl NullableRet for String {
    fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_bytes(&rows, true) }
}

impl AsRef<[u8]> for Bytes {
    fn as_ref(&self) -> &[u8] { &self.0 }
}
impl Ret for Bytes {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_bytes(&rows, false) }
}
impl NullableRet for Bytes {
    fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> { write_bytes(&rows, true) }
}

/// A type that can be returned as `Option<T>`, written as `Nullable(T)`.
/// ClickHouse has no `Nullable(Array)` or `Nullable(Map)`.
pub trait NullableRet: Sized {
    fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String>;
}

impl<T: NullableRet> Ret for Option<T> {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
        T::write_nullable(rows.into_iter().map(Option::flatten).collect())
    }
}

/// An error result aborts the call with the error's message.
impl<T: Ret, E: Display> Ret for Result<T, E> {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
        let mut ok = Vec::with_capacity(rows.len());
        for r in rows {
            ok.push(match r {
                Some(Err(e)) => return Err(e.to_string()),
                Some(Ok(v)) => Some(v),
                None => None,
            });
        }
        T::write_column(ok)
    }
}

impl<T: ElemOut> Ret for Vec<T> {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
        write_complex(rows.into_iter().map(Option::unwrap_or_default).collect(), None)
    }
}

impl<K: ElemOut + Ord, V: ElemOut> Ret for BTreeMap<K, V> {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
        write_complex(rows.into_iter().map(Option::unwrap_or_default).collect(), None)
    }
}

impl<K: ElemOut + Eq + std::hash::Hash, V: ElemOut> Ret for HashMap<K, V> {
    fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
        write_complex(rows.into_iter().map(Option::unwrap_or_default).collect(), None)
    }
}

// ── Elements of a COL_COMPLEX block ─────────────────────────────────────────

/// A value that can be written inside a complex block.
pub trait ElemOut: Sized {
    /// The value stored for a NULL or skipped row.
    fn zero() -> Self;
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>);
}

macro_rules! fixed_elem_out {
    ($($t:ty),*) => {$(
        impl ElemOut for $t {
            fn zero() -> Self { Default::default() }
            fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
                for v in vals { v.put(out); }
            }
        }
    )*};
}
fixed_elem_out!(i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, f32, f64, bool);

impl<const N: usize> ElemOut for [u8; N] {
    fn zero() -> Self { [0; N] }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
        for v in vals { out.extend_from_slice(&v); }
    }
}

fn write_bytes_block<B: AsRef<[u8]>>(out: &mut Vec<u8>, vals: &[B]) {
    let mut pos = 0u64;
    out.extend_from_slice(&pos.to_le_bytes());
    for v in vals {
        pos += v.as_ref().len() as u64;
        out.extend_from_slice(&pos.to_le_bytes());
    }
    for v in vals {
        out.extend_from_slice(v.as_ref());
    }
}

impl ElemOut for String {
    fn zero() -> Self { String::new() }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) { write_bytes_block(out, &vals) }
}

impl ElemOut for Bytes {
    fn zero() -> Self { Bytes::default() }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) { write_bytes_block(out, &vals) }
}

impl<E: ElemOut> ElemOut for Vec<E> {
    fn zero() -> Self { Vec::new() }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
        let mut pos = 0u64;
        out.extend_from_slice(&pos.to_le_bytes());
        for v in &vals {
            pos += v.len() as u64;
            out.extend_from_slice(&pos.to_le_bytes());
        }
        E::write_block(out, vals.into_iter().flatten().collect());
    }
}

impl<E: ElemOut> ElemOut for Option<E> {
    fn zero() -> Self { None }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
        out.extend(vals.iter().map(|v| v.is_none() as u8));
        E::write_block(out, vals.into_iter().map(|v| v.unwrap_or_else(E::zero)).collect());
    }
}

impl<K: ElemOut + Ord, V: ElemOut> ElemOut for BTreeMap<K, V> {
    fn zero() -> Self { BTreeMap::new() }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
        Vec::<(K, V)>::write_block(out, vals.into_iter().map(|m| m.into_iter().collect()).collect());
    }
}

impl<K: ElemOut + Eq + std::hash::Hash, V: ElemOut> ElemOut for HashMap<K, V> {
    fn zero() -> Self { HashMap::new() }
    fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
        Vec::<(K, V)>::write_block(out, vals.into_iter().map(|m| m.into_iter().collect()).collect());
    }
}

macro_rules! tuple_out {
    ($($idx:tt $T:ident $v:ident),+) => {
        impl<$($T: ElemOut),+> ElemOut for ($($T,)+) {
            fn zero() -> Self { ($($T::zero(),)+) }
            fn write_block(out: &mut Vec<u8>, vals: Vec<Self>) {
                $( let mut $v: Vec<$T> = Vec::with_capacity(vals.len()); )+
                for t in vals { $( $v.push(t.$idx); )+ }
                $( $T::write_block(out, $v); )+
            }
        }

        impl<$($T: ElemOut),+> Ret for ($($T,)+) {
            fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
                write_complex(rows.into_iter().map(|r| r.unwrap_or_else(Self::zero)).collect(), None)
            }
        }

        impl<$($T: ElemOut),+> NullableRet for ($($T,)+) {
            fn write_nullable(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
                let nulls = rows.iter().map(|r| r.is_none() as u8).collect();
                write_complex(rows.into_iter().map(|r| r.unwrap_or_else(Self::zero)).collect(), Some(nulls))
            }
        }
    };
}
#[allow(non_snake_case)]
mod tuples {
    use super::*;
    tuple_out!(0 A a);
    tuple_out!(0 A a, 1 B b);
    tuple_out!(0 A a, 1 B b, 2 C c);
    tuple_out!(0 A a, 1 B b, 2 C c, 3 D d);
    tuple_out!(0 A a, 1 B b, 2 C c, 3 D d, 4 E e);
    tuple_out!(0 A a, 1 B b, 2 C c, 3 D d, 4 E e, 5 F f);
    tuple_out!(0 A a, 1 B b, 2 C c, 3 D d, 4 E e, 5 F f, 6 G g);
    tuple_out!(0 A a, 1 B b, 2 C c, 3 D d, 4 E e, 5 F f, 6 G g, 7 H h);
}
