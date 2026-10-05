//! Frame parsing, checked against frames written by the ClickHouse host
//! (../tests/wire_fixtures, shared with the C++ library).
use clickhouse_wasm_columnar::frame::{ColTag, Frame};

fn fixture(name: &str) -> Vec<u8> {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../tests/wire_fixtures/");
    std::fs::read(format!("{path}{name}.bin")).expect("fixture")
}

#[test]
fn every_fixture_parses() {
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../tests/wire_fixtures");
    let mut n = 0;
    for e in std::fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.extension().map_or(true, |x| x != "bin") { continue; }
        let bytes = std::fs::read(&p).unwrap();
        let f = Frame::parse(&bytes).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        for i in 0..f.num_cols() {
            f.col(i).unwrap_or_else(|e| panic!("{} col {i}: {e}", p.display()));
        }
        n += 1;
    }
    assert!(n >= 18, "found only {n} fixtures");
}

#[test]
fn fixed_string16_rows() {
    let b = fixture("fixedn_fs16");
    let c = Frame::parse(&b).unwrap().col(0).unwrap();
    assert_eq!(c.tag(), ColTag::FixedN);
    assert_eq!(c.fixed_width(), 16);
    assert_eq!(c.fixed_bytes(0).unwrap(), b"0123456789abcdef");
    assert_eq!(c.fixed_bytes(1).unwrap(), b"fedcba9876543210");
}

#[test]
fn nullable_fixed_string16() {
    let b = fixture("fixedn_fs16_nullable");
    let c = Frame::parse(&b).unwrap().col(0).unwrap();
    assert!(!c.is_null(0));
    assert!(c.is_null(1));
    assert!(!c.is_null(2));
    assert_eq!(c.fixed_bytes(0).unwrap(), b"abcdefgh\0\0\0\0\0\0\0\0");
}

#[test]
fn zero_rows_fixedn() {
    let b = fixture("fixedn_zero_rows");
    let c = Frame::parse(&b).unwrap().col(0).unwrap();
    assert_eq!(c.tag(), ColTag::FixedN);
    assert_eq!(c.stored_rows(), 0);
}

#[test]
fn fixed_string8_is_fixed64_width_class() {
    let b = fixture("fixedwidth_fs8");
    let c = Frame::parse(&b).unwrap().col(0).unwrap();
    assert_eq!(c.tag(), ColTag::Fixed64);
    assert_eq!(c.fixed_bytes(1).unwrap(), b"ijklmnop");
}

#[test]
fn const_column_broadcasts() {
    let b = fixture("fixedwidth_fs8_const");
    let c = Frame::parse(&b).unwrap().col(0).unwrap();
    assert!(c.is_const());
    assert_eq!(c.stored_rows(), 1);
    assert_eq!(c.fixed_bytes(4).unwrap(), b"const888");
}

#[test]
fn rejects_bad_header() {
    let mut b = fixture("fixed64_plain");
    assert!(Frame::parse(&b[..15]).is_err());
    b[0] ^= 1;
    assert!(Frame::parse(&b).is_err());
    let mut b = fixture("fixed64_plain");
    b[4] = 2; // version
    assert!(Frame::parse(&b).is_err());
    let mut b = fixture("fixed64_plain");
    b[6] = 1; // reserved
    assert!(Frame::parse(&b).is_err());
}

#[test]
fn rejects_column_out_of_frame() {
    let mut b = fixture("fixed64_plain");
    // data_offset of descriptor 0 at 16 + 24
    let past = b.len() as u64 + 8;
    b[16 + 24..16 + 32].copy_from_slice(&past.to_le_bytes());
    assert!(Frame::parse(&b).unwrap().col(0).is_err());
    let f = fixture("fixed64_plain");
    assert!(Frame::parse(&f).unwrap().col(1).is_err());
}

#[test]
fn rejects_unknown_tag_and_stray_bits() {
    for t in [9u64, 0x40] {
        let mut b = fixture("fixed64_plain");
        b[16..24].copy_from_slice(&t.to_le_bytes());
        assert!(Frame::parse(&b).unwrap().col(0).is_err(), "tag {t:#x}");
    }
}

#[test]
fn wire_constants_match_wire_h() {
    use clickhouse_wasm_columnar::wire::*;
    let h = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"), "/../include/clickhouse_wasm/wire.h")).unwrap();
    let val = |name: &str| -> u64 {
        let line = h.lines().find(|l| l.split_whitespace().nth(1) == Some(name))
            .unwrap_or_else(|| panic!("{name} not in wire.h"));
        let v = line.split_whitespace().nth(2).unwrap().trim_end_matches('u');
        if let Some(x) = v.strip_prefix("0x") { u64::from_str_radix(x, 16).unwrap() } else { v.parse().unwrap() }
    };
    assert_eq!(val("CHW_FRAME_MAGIC"), FRAME_MAGIC as u64);
    assert_eq!(val("CHW_FRAME_VERSION"), FRAME_VERSION as u64);
    assert_eq!(val("CHW_HEADER_BYTES"), HEADER_BYTES as u64);
    assert_eq!(val("CHW_COL_DESC_BYTES"), COL_DESC_BYTES as u64);
    for (n, v) in [("CHW_COL_BYTES", COL_BYTES), ("CHW_COL_FIXED8", COL_FIXED8),
                   ("CHW_COL_FIXED16", COL_FIXED16), ("CHW_COL_FIXED32", COL_FIXED32),
                   ("CHW_COL_FIXED64", COL_FIXED64), ("CHW_COL_COMPLEX", COL_COMPLEX),
                   ("CHW_COL_VARIANT", COL_VARIANT), ("CHW_COL_FIXEDN", COL_FIXEDN),
                   ("CHW_COL_LOWCARD", COL_LOWCARD), ("CHW_COL_IS_NULLABLE", COL_IS_NULLABLE),
                   ("CHW_COL_IS_CONST", COL_IS_CONST)] {
        assert_eq!(val(n), v, "{n}");
    }
    assert_eq!(std::mem::size_of::<ColDescriptor>(), COL_DESC_BYTES);
}
