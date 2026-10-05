//! Reading a COLUMNAR_V1 frame. Every offset is bounds-checked against the
//! frame before it is used; a malformed frame is an `Error`, never a panic or
//! an out-of-bounds read.
use crate::wire::*;
use std::fmt;

/// Why a frame or column was rejected.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub &'static str);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "columnar: {}", self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn err<T>(msg: &'static str) -> Result<T> {
    Err(Error(msg))
}

fn u32_at(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}

fn u64_at(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

/// `len` bytes at `off`, if they are inside `b`.
fn extent(b: &[u8], off: u64, len: u64) -> Result<&[u8]> {
    let total = b.len() as u64;
    if off > total || len > total - off {
        return err("extent past end of frame");
    }
    Ok(&b[off as usize..(off + len) as usize])
}

/// A parsed frame header plus the frame bytes.
#[derive(Clone, Copy)]
pub struct Frame<'a> {
    bytes: &'a [u8],
    num_rows: u32,
    num_cols: u32,
}

impl<'a> Frame<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self> {
        if bytes.len() < HEADER_BYTES {
            return err("frame shorter than header");
        }
        if u32_at(bytes, 0) != FRAME_MAGIC {
            return err("bad frame magic");
        }
        if u16::from_le_bytes([bytes[4], bytes[5]]) != FRAME_VERSION {
            return err("unsupported frame version");
        }
        if bytes[6] != 0 || bytes[7] != 0 {
            return err("reserved frame header field is non-zero");
        }
        let num_rows = u32_at(bytes, 8);
        let num_cols = u32_at(bytes, 12);
        if num_cols as usize > (bytes.len() - HEADER_BYTES) / COL_DESC_BYTES {
            return err("descriptor table extends past end of frame");
        }
        Ok(Self { bytes, num_rows, num_cols })
    }

    pub fn num_rows(&self) -> u32 { self.num_rows }
    pub fn num_cols(&self) -> u32 { self.num_cols }
    pub fn bytes(&self) -> &'a [u8] { self.bytes }

    pub fn descriptor(&self, i: u32) -> Result<ColDescriptor> {
        if i >= self.num_cols {
            return err("column index out of range");
        }
        let off = HEADER_BYTES + i as usize * COL_DESC_BYTES;
        Ok(ColDescriptor::read(&self.bytes[off..off + COL_DESC_BYTES]))
    }

    pub fn col(&self, i: u32) -> Result<ColView<'a>> {
        self.view(&self.descriptor(i)?, self.num_rows)
    }

    /// Resolve any descriptor in this frame holding `rows` rows: a top-level
    /// column, a Variant alternative or a LowCardinality dictionary.
    pub fn view(&self, d: &ColDescriptor, rows: u32) -> Result<ColView<'a>> {
        if d.type_ & !(COL_IS_CONST | COL_IS_NULLABLE | 0x0F) != 0 {
            return err("descriptor type word has bits outside the known flags");
        }
        let nullable = d.type_ & COL_IS_NULLABLE != 0;
        let is_const = d.type_ & COL_IS_CONST != 0;
        let tag = ColTag::from_wire(d.type_ & 0x0F)?;
        if tag == ColTag::LowCard && nullable {
            return err("LowCardinality(Nullable) is not supported");
        }
        let stored = if is_const { 1 } else { rows };
        let b = self.bytes;

        let null_map = if d.null_offset != 0 {
            Some(extent(b, d.null_offset, stored as u64)?)
        } else {
            None
        };
        let data = extent(b, d.data_offset, d.data_size)?;

        let fixed_width = match tag {
            ColTag::Fixed8 => 1,
            ColTag::Fixed16 => 2,
            ColTag::Fixed32 => 4,
            ColTag::Fixed64 => 8,
            ColTag::FixedN if stored == 0 => {
                if d.data_size != 0 {
                    return err("COL_FIXEDN data_size must be 0 for an empty column");
                }
                0
            }
            ColTag::FixedN => {
                if d.data_size % stored as u64 != 0 {
                    return err("COL_FIXEDN data_size is not a multiple of row count");
                }
                let w = d.data_size / stored as u64;
                if w == 0 || w > u32::MAX as u64 {
                    return err("COL_FIXEDN element width is out of range");
                }
                w as u32
            }
            _ => 0,
        };
        if fixed_width != 0 && d.data_size != stored as u64 * fixed_width as u64 {
            return err("fixed-width data_size does not match row count");
        }

        let mut lowcard = None;
        if tag == ColTag::LowCard {
            lowcard = Some(self.lowcard_header(d)?);
        }
        let offsets = if d.offsets_offset != 0 {
            let len = match tag {
                ColTag::Bytes => (stored as u64 + 1) * 8,
                ColTag::LowCard => stored as u64 * lowcard.as_ref().unwrap().width as u64,
                ColTag::Variant => stored as u64 * 4,
                _ => stored as u64 * 8,
            };
            Some(extent(b, d.offsets_offset, len)?)
        } else {
            None
        };
        if tag == ColTag::Bytes && offsets.is_none() {
            return err("String column has no offsets array");
        }
        if tag == ColTag::LowCard && offsets.is_none() {
            return err("LowCardinality descriptor has no index array");
        }

        Ok(ColView { frame: *self, tag, is_const, stored, null_map, offsets, data, fixed_width, lowcard })
    }

    fn lowcard_header(&self, d: &ColDescriptor) -> Result<LowCard<'a>> {
        let header = extent(self.bytes, d.data_offset, 8 + COL_DESC_BYTES as u64)?;
        if d.data_size < 8 + COL_DESC_BYTES as u64 {
            return err("LowCardinality header truncated");
        }
        let dict_rows = u32_at(header, 0);
        let width = header[4];
        if !matches!(width, 1 | 2 | 4 | 8) {
            return err("LowCardinality index element width is not 1/2/4/8");
        }
        let dict = ColDescriptor::read(&header[8..8 + COL_DESC_BYTES]);
        if dict.type_ & (COL_IS_CONST | COL_IS_NULLABLE) != 0 {
            return err("LowCardinality dictionary must not be const or nullable");
        }
        if matches!(dict.type_ & 0x0F, COL_LOWCARD | COL_VARIANT) {
            return err("LowCardinality dictionary has an unsupported column tag");
        }
        if dict_rows < 1 {
            return err("LowCardinality dictionary is empty");
        }
        let dict = Box::new(self.view(&dict, dict_rows)?);
        Ok(LowCard { width, dict })
    }
}

/// Base type tag of a column.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColTag {
    Bytes,
    Fixed8,
    Fixed16,
    Fixed32,
    Fixed64,
    Complex,
    Variant,
    FixedN,
    LowCard,
}

impl ColTag {
    fn from_wire(t: u64) -> Result<Self> {
        Ok(match t {
            COL_BYTES => Self::Bytes,
            COL_FIXED8 => Self::Fixed8,
            COL_FIXED16 => Self::Fixed16,
            COL_FIXED32 => Self::Fixed32,
            COL_FIXED64 => Self::Fixed64,
            COL_COMPLEX => Self::Complex,
            COL_VARIANT => Self::Variant,
            COL_FIXEDN => Self::FixedN,
            COL_LOWCARD => Self::LowCard,
            _ => return err("unsupported column tag in descriptor"),
        })
    }
}

#[derive(Clone)]
struct LowCard<'a> {
    width: u8,
    dict: Box<ColView<'a>>,
}

/// One validated column of a frame.
#[derive(Clone)]
pub struct ColView<'a> {
    frame: Frame<'a>,
    tag: ColTag,
    is_const: bool,
    stored: u32,
    null_map: Option<&'a [u8]>,
    offsets: Option<&'a [u8]>,
    data: &'a [u8],
    fixed_width: u32,
    lowcard: Option<LowCard<'a>>,
}

impl<'a> ColView<'a> {
    pub fn tag(&self) -> ColTag { self.tag }
    pub fn is_const(&self) -> bool { self.is_const }
    /// Rows actually stored: 1 for a const column.
    pub fn stored_rows(&self) -> u32 { self.stored }
    /// Element width of a fixed-width column, 0 otherwise.
    pub fn fixed_width(&self) -> u32 { self.fixed_width }
    pub fn frame(&self) -> &Frame<'a> { &self.frame }
    pub fn data(&self) -> &'a [u8] { self.data }
    pub fn offsets_bytes(&self) -> Option<&'a [u8]> { self.offsets }
    pub fn null_map(&self) -> Option<&'a [u8]> { self.null_map }

    /// Stored row that logical `row` reads.
    pub fn stored_row(&self, row: u32) -> Result<u32> {
        let r = if self.is_const { 0 } else { row };
        if r >= self.stored {
            return err("row out of range");
        }
        Ok(r)
    }

    pub fn is_null(&self, row: u32) -> bool {
        let Some(m) = self.null_map else { return false };
        let r = if self.is_const { 0 } else { row as usize };
        match m.get(r) {
            Some(&v) if self.tag == ColTag::Variant => v == VARIANT_NULL_DISCRIMINATOR,
            Some(&v) => v != 0,
            None => false,
        }
    }

    /// Value bytes of a `COL_BYTES` row.
    pub fn bytes(&self, row: u32) -> Result<&'a [u8]> {
        if self.tag != ColTag::Bytes {
            return err("not a String column");
        }
        let r = self.stored_row(row)? as usize;
        let offs = self.offsets.unwrap();
        let (start, end) = (u64_at(offs, r * 8), u64_at(offs, r * 8 + 8));
        if start > end || end > self.data.len() as u64 {
            return err("String offsets exceed column data");
        }
        Ok(&self.data[start as usize..end as usize])
    }

    /// Raw bytes of a fixed-width row.
    pub fn fixed_bytes(&self, row: u32) -> Result<&'a [u8]> {
        if self.fixed_width == 0 {
            return err("fixed-width read of a column with no width");
        }
        let r = self.stored_row(row)? as usize;
        let w = self.fixed_width as usize;
        Ok(&self.data[r * w..r * w + w])
    }

    /// LowCardinality: the dictionary column.
    pub fn dictionary(&self) -> Option<&ColView<'a>> {
        self.lowcard.as_ref().map(|l| l.dict.as_ref())
    }

    /// LowCardinality: dictionary row for logical `row`, checked against the dictionary.
    pub fn dict_index(&self, row: u32) -> Result<u32> {
        let Some(lc) = &self.lowcard else { return err("not a LowCardinality column") };
        let r = self.stored_row(row)? as usize;
        let w = lc.width as usize;
        let mut buf = [0u8; 8];
        buf[..w].copy_from_slice(&self.offsets.unwrap()[r * w..r * w + w]);
        let i = u64::from_le_bytes(buf);
        if i >= lc.dict.stored as u64 {
            return err("LowCardinality index exceeds dictionary");
        }
        Ok(i as u32)
    }
}

/// One row of a Variant column: which alternative holds it, and where.
pub struct VariantRow<'a> {
    pub discriminator: u8,
    /// The alternative's column.
    pub alternative: ColView<'a>,
    /// The row's position inside `alternative`.
    pub offset: u32,
}

impl<'a> ColView<'a> {
    /// A Variant row, or `None` for NULL.
    pub fn variant_row(&self, row: u32) -> Result<Option<VariantRow<'a>>> {
        if self.tag != ColTag::Variant {
            return err("Variant argument against a column that is not COL_VARIANT");
        }
        let (Some(discs), Some(offs)) = (self.null_map, self.offsets) else {
            return err("COL_VARIANT column without discriminators or row offsets");
        };
        let r = self.stored_row(row)? as usize;
        let d = discs[r];
        if d == VARIANT_NULL_DISCRIMINATOR {
            return Ok(None);
        }
        let offset = u32_at(offs, r * 4);
        if self.data.len() < 4 {
            return err("COL_VARIANT header truncated");
        }
        let k = u32_at(self.data, 0) as usize;
        const RECORD: usize = 4 + COL_DESC_BYTES;
        if k > (self.data.len() - 4) / RECORD {
            return err("COL_VARIANT header records extend past the column data");
        }
        for i in 0..k {
            let rec = &self.data[4 + i * RECORD..4 + (i + 1) * RECORD];
            if rec[0] != d {
                continue;
            }
            let mut inner = ColDescriptor::read(&rec[4..]);
            let rows = inner.null_offset; // repurposed: the alternative's row count
            if rows > u32::MAX as u64 {
                return err("COL_VARIANT alternative row count too large");
            }
            inner.null_offset = 0;
            // The alternative must lie inside this column's data.
            let end = self.data.as_ptr() as usize + self.data.len() - self.frame.bytes.as_ptr() as usize;
            let region = Frame { bytes: &self.frame.bytes[..end], ..self.frame };
            let alternative = region.view(&inner, rows as u32)?;
            if offset as u64 >= rows {
                return err("COL_VARIANT row offset past its alternative");
            }
            return Ok(Some(VariantRow { discriminator: d, alternative, offset }));
        }
        err("COL_VARIANT row refers to an alternative missing from the header")
    }
}
