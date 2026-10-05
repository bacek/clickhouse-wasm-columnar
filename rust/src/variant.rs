//! `Variant(...)` arguments and results, through user enums declared with
//! [`column_variant!`](crate::column_variant).
use crate::ret::single_col_frame;
use crate::wire::*;

/// An enum that is written as a ClickHouse `Variant`. Implemented by
/// [`column_variant!`](crate::column_variant).
pub trait VariantOut: Sized {
    /// Number of alternatives.
    const ALTERNATIVES: usize;
    /// Index of this value's alternative, in ClickHouse's order.
    fn discriminator(&self) -> u8;
    /// A one-column frame with `vals`, all of alternative `disc`.
    fn write_alternative(disc: u8, vals: Vec<Self>) -> Result<Vec<u8>, String>;
}

fn align(x: usize, a: usize) -> usize {
    (x + a - 1) & !(a - 1)
}

/// Write a Variant column (see SPEC.md, "Variant").
pub fn write_variant<V: VariantOut>(rows: Vec<Option<V>>) -> Result<Vec<u8>, String> {
    let n = rows.len();
    let mut discs = vec![VARIANT_NULL_DISCRIMINATOR; n];
    let mut offs = vec![0u32; n];
    let mut groups: Vec<Vec<V>> = (0..V::ALTERNATIVES).map(|_| Vec::new()).collect();
    for (i, r) in rows.into_iter().enumerate() {
        let Some(v) = r else { continue };
        let d = v.discriminator();
        discs[i] = d;
        offs[i] = groups[d as usize].len() as u32;
        groups[d as usize].push(v);
    }
    // One sub-frame per non-empty alternative; empty ones get no record.
    let mut subs = Vec::new();
    for (d, g) in groups.into_iter().enumerate() {
        if !g.is_empty() {
            let count = g.len() as u64;
            subs.push((d as u8, count, V::write_alternative(d as u8, g)?));
        }
    }
    const RECORD: usize = 4 + COL_DESC_BYTES;
    const SUB_PAYLOAD: usize = HEADER_BYTES + COL_DESC_BYTES;
    let base = SUB_PAYLOAD; // body starts right after our own descriptor
    let disc_off = base;
    let offs_off = align(disc_off + n, 4);
    let data_off = align(offs_off + n * 4, 8);
    let mut pos = align(data_off + 4 + subs.len() * RECORD, 8);
    let mut placed = Vec::with_capacity(subs.len());
    for (_, _, f) in &subs {
        placed.push(pos);
        pos = align(pos + f.len() - SUB_PAYLOAD, 8);
    }
    let mut body = vec![0u8; pos - base];
    body[0..n].copy_from_slice(&discs);
    for (i, o) in offs.iter().enumerate() {
        let p = offs_off - base + i * 4;
        body[p..p + 4].copy_from_slice(&o.to_le_bytes());
    }
    let mut rec = data_off - base;
    body[rec..rec + 4].copy_from_slice(&(subs.len() as u32).to_le_bytes());
    rec += 4;
    for ((d, count, f), at) in subs.iter().zip(&placed) {
        let mut inner = ColDescriptor::read(&f[HEADER_BYTES..SUB_PAYLOAD]);
        let shift = (at - SUB_PAYLOAD) as u64;
        if inner.offsets_offset != 0 {
            inner.offsets_offset += shift;
        }
        if inner.null_offset != 0 {
            return Err("columnar: a Variant alternative cannot be Nullable".into());
        }
        inner.data_offset += shift;
        inner.null_offset = *count;
        body[rec] = *d;
        inner.write(&mut body[rec + 4..rec + RECORD]);
        rec += RECORD;
        let p = at - base;
        body[p..p + f.len() - SUB_PAYLOAD].copy_from_slice(&f[SUB_PAYLOAD..]);
    }
    let desc = ColDescriptor {
        type_: COL_VARIANT,
        null_offset: disc_off as u64,
        offsets_offset: offs_off as u64,
        data_offset: data_off as u64,
        data_size: (pos - data_off) as u64,
    };
    single_col_frame(n, desc, &body)
}

/// Declare an enum that maps to a ClickHouse `Variant`. List the alternatives
/// in ClickHouse's order (by type name): `Variant(Int64, String)` is
///
/// ```ignore
/// column_variant! { pub enum IntOrStr { Int(i64), Str(String) } }
/// ```
///
/// The enum can then be an argument (`IntOrStr` or `Option<IntOrStr>`) and a
/// result (`IntOrStr`; a skipped row is NULL).
#[macro_export]
macro_rules! column_variant {
    ($(#[$m:meta])* $vis:vis enum $name:ident { $($alt:ident($ty:ty)),+ $(,)? }) => {
        $(#[$m])* $vis enum $name { $($alt($ty)),+ }

        impl<'a> $crate::Arg<'a> for $name {
            fn read(col: &$crate::frame::ColView<'a>, row: u32) -> $crate::frame::Result<Self> {
                let Some(v) = col.variant_row(row)? else {
                    return Err($crate::frame::Error("NULL Variant row passed to an argument that is not an Option"));
                };
                let mut _i = 0u8;
                $(
                    if v.discriminator == _i {
                        return Ok($name::$alt($crate::read_arg::<$ty>(&v.alternative, v.offset)?));
                    }
                    _i += 1;
                )+
                Err($crate::frame::Error("Variant discriminator has no matching enum alternative"))
            }
        }

        impl $crate::variant::VariantOut for $name {
            const ALTERNATIVES: usize = [$(stringify!($alt)),+].len();
            fn discriminator(&self) -> u8 {
                let mut _i = 0u8;
                $(
                    if let $name::$alt(_) = self { return _i; }
                    _i += 1;
                )+
                unreachable!()
            }
            #[allow(unreachable_patterns)]
            fn write_alternative(disc: u8, vals: Vec<Self>) -> Result<Vec<u8>, String> {
                let mut _i = 0u8;
                $(
                    if disc == _i {
                        let vals: Vec<Option<$ty>> = vals.into_iter().map(|v| match v {
                            $name::$alt(x) => Some(x),
                            _ => unreachable!(),
                        }).collect();
                        return <$ty as $crate::Ret>::write_column(vals);
                    }
                    _i += 1;
                )+
                unreachable!()
            }
        }

        impl $crate::Ret for $name {
            fn write_column(rows: Vec<Option<Self>>) -> Result<Vec<u8>, String> {
                $crate::variant::write_variant(rows)
            }
        }
    };
}

