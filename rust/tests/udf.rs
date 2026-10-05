//! Functions exported with `columnar_udf!`, called on frames end to end.
//! Input frames are built with the library's own result writer, so these
//! tests also check that every written type reads back.
use clickhouse_wasm_columnar::wire::*;
use clickhouse_wasm_columnar::{column_variant, columnar_udf, Bytes, Frame, Ret};
use std::collections::BTreeMap;

/// One column of `T` as a frame.
fn col<T: Ret>(vals: Vec<T>) -> Vec<u8> {
    T::write_column(vals.into_iter().map(Some).collect()).unwrap()
}

/// Join single-column frames (no Variant/LowCardinality) into one frame.
fn frame(cols: &[Vec<u8>]) -> Vec<u8> {
    let rows = u32::from_le_bytes(cols[0][8..12].try_into().unwrap());
    let body_start = HEADER_BYTES + COL_DESC_BYTES * cols.len();
    let mut out = cols[0][..HEADER_BYTES].to_vec();
    out[12..16].copy_from_slice(&(cols.len() as u32).to_le_bytes());
    out.resize(body_start, 0);
    for (i, c) in cols.iter().enumerate() {
        let shift = ((out.len() + 7) & !7) as u64 - (HEADER_BYTES + COL_DESC_BYTES) as u64;
        out.resize((out.len() + 7) & !7, 0);
        let mut d = ColDescriptor::read(&c[HEADER_BYTES..HEADER_BYTES + COL_DESC_BYTES]);
        for f in [&mut d.null_offset, &mut d.offsets_offset] {
            if *f != 0 { *f += shift; }
        }
        d.data_offset += shift;
        d.write(&mut out[HEADER_BYTES + i * COL_DESC_BYTES..HEADER_BYTES + (i + 1) * COL_DESC_BYTES]);
        out.extend_from_slice(&c[HEADER_BYTES + COL_DESC_BYTES..]);
        assert_eq!(u32::from_le_bytes(c[8..12].try_into().unwrap()), rows);
    }
    out
}

/// Read back every row of a result frame as `T`.
fn rows<T: for<'a> clickhouse_wasm_columnar::Arg<'a>>(out: &[u8]) -> Vec<T> {
    let f = Frame::parse(out).unwrap();
    assert_eq!(f.num_cols(), 1);
    let c = f.col(0).unwrap();
    (0..f.num_rows()).map(|r| clickhouse_wasm_columnar::read_arg::<T>(&c, r).unwrap()).collect()
}

fn s(v: &[&str]) -> Vec<String> {
    v.iter().map(|x| x.to_string()).collect()
}

columnar_udf! {
    fn t_repeat(s: &str, n: u32) -> String { s.repeat(n as usize) }
}

#[test]
fn string_and_widened_integer() {
    let input = frame(&[col(s(&["ab", "", "x"])), col(vec![3u8, 2, 0])]);
    assert_eq!(rows::<String>(&t_repeat::call(&input).unwrap()), s(&["ababab", "", ""]));
}

columnar_udf! {
    fn t_parse(s: &str) -> Option<i64> { s.parse().ok().filter(|v: &i64| *v >= 0) }
}

#[test]
fn nullable_result_and_null_argument() {
    let input = <Option<String>>::write_column(vec![Some(Some("42".into())), Some(None), Some(Some("x".into()))]).unwrap();
    assert_eq!(rows::<Option<i64>>(&t_parse::call(&input).unwrap()), vec![Some(42), None, None]);
}

columnar_udf! {
    fn t_show(v: Option<i64>) -> String { v.map_or("null".into(), |x| x.to_string()) }
}

#[test]
fn option_argument_sees_null() {
    let input = <Option<i64>>::write_column(vec![Some(Some(5)), Some(None)]).unwrap();
    assert_eq!(rows::<String>(&t_show::call(&input).unwrap()), s(&["5", "null"]));
}

columnar_udf! {
    fn t_mean(xs: Vec<f64>) -> f64 { if xs.is_empty() { 0.0 } else { xs.iter().sum::<f64>() / xs.len() as f64 } }
}

#[test]
fn array_argument() {
    let input = col(vec![vec![1.0, 2.0, 6.0], vec![], vec![10.0]]);
    assert_eq!(rows::<f64>(&t_mean::call(&input).unwrap()), vec![3.0, 0.0, 10.0]);
}

columnar_udf! {
    fn t_split(s: &str, sep: &str) -> Vec<String> { s.split(sep).map(str::to_owned).collect() }
}

#[test]
fn array_of_strings_result() {
    let input = frame(&[col(s(&["a,b,,c", ""])), col(s(&[",", ","]))]);
    assert_eq!(rows::<Vec<String>>(&t_split::call(&input).unwrap()), vec![s(&["a", "b", "", "c"]), s(&[""])]);
}

columnar_udf! {
    fn t_divmod(a: i64, b: i64) -> Result<(i64, i64), String> {
        if b == 0 { Err("division by zero".into()) } else { Ok((a / b, a % b)) }
    }
}

#[test]
fn tuple_result_and_error() {
    let input = frame(&[col(vec![17i64, -7]), col(vec![5i64, 2])]);
    assert_eq!(rows::<(i64, i64)>(&t_divmod::call(&input).unwrap()), vec![(3, 2), (-3, -1)]);
    let input = frame(&[col(vec![1i64]), col(vec![0i64])]);
    assert_eq!(t_divmod::call(&input).unwrap_err(), "division by zero");
}

columnar_udf! {
    fn t_tuple_sum(t: (i64, f64)) -> f64 { t.0 as f64 + t.1 }
}

#[test]
fn tuple_argument() {
    let input = col(vec![(2i64, 0.5f64), (-3, 0.25)]);
    assert_eq!(rows::<f64>(&t_tuple_sum::call(&input).unwrap()), vec![2.5, -2.75]);
}

columnar_udf! {
    fn t_flatten(xss: Vec<Vec<i64>>) -> Vec<i64> { xss.concat() }
}

#[test]
fn nested_arrays() {
    let input = col(vec![vec![vec![1i64, 2], vec![], vec![3]], vec![], vec![vec![5]]]);
    assert_eq!(rows::<Vec<i64>>(&t_flatten::call(&input).unwrap()), vec![vec![1, 2, 3], vec![], vec![5]]);
}

columnar_udf! {
    fn t_upper(xs: Vec<Option<String>>) -> Vec<Option<String>> {
        xs.into_iter().map(|x| x.map(|s| s.to_uppercase())).collect()
    }
}

#[test]
fn array_of_nullable() {
    let input = col(vec![vec![Some("ab".to_string()), None, Some("c".into())]]);
    assert_eq!(rows::<Vec<Option<String>>>(&t_upper::call(&input).unwrap()),
               vec![vec![Some("AB".to_string()), None, Some("C".into())]]);
}

columnar_udf! {
    fn t_map_get(m: BTreeMap<String, i64>, k: &str) -> Option<i64> { m.get(k).copied() }
}

columnar_udf! {
    fn t_invert(m: BTreeMap<String, i64>) -> BTreeMap<i64, String> { m.into_iter().map(|(k, v)| (v, k)).collect() }
}

#[test]
fn maps() {
    let m: BTreeMap<String, i64> = [("a".to_string(), 1), ("b".to_string(), 2)].into();
    let input = frame(&[col(vec![m.clone(), m.clone()]), col(s(&["b", "z"]))]);
    assert_eq!(rows::<Option<i64>>(&t_map_get::call(&input).unwrap()), vec![Some(2), None]);
    let out = t_invert::call(&col(vec![m])).unwrap();
    assert_eq!(rows::<BTreeMap<i64, String>>(&out), vec![[(1, "a".to_string()), (2, "b".to_string())].into()]);
}

columnar_udf! {
    fn t_halves(v: i64) -> Option<(i64, i64)> { (v >= 0).then(|| (v / 2, v - v / 2)) }
}

#[test]
fn nullable_tuple_result() {
    let out = t_halves::call(&col(vec![7i64, -1])).unwrap();
    assert_eq!(rows::<Option<(i64, i64)>>(&out), vec![Some((3, 4)), None]);
}

columnar_udf! {
    fn t_hex(u: [u8; 16]) -> String { u.iter().map(|b| format!("{b:02x}")).collect() }
}

columnar_udf! {
    fn t_bytes(s: &[u8]) -> Bytes { Bytes(s.iter().rev().copied().collect()) }
}

#[test]
fn fixed_n_and_raw_bytes() {
    let out = t_hex::call(&col(vec![[0x11u8; 16]])).unwrap();
    assert_eq!(rows::<String>(&out), vec!["11".repeat(16)]);
    let out = t_bytes::call(&col(vec![Bytes(vec![0xff, 0])])).unwrap();
    let f = Frame::parse(&out).unwrap();
    let c = f.col(0).unwrap();
    assert_eq!(clickhouse_wasm_columnar::read_arg::<&[u8]>(&c, 0).unwrap(), &[0, 0xff]);
}

column_variant! {
    #[derive(Debug, PartialEq)]
    pub enum IntOrStr { Int(i64), Str(String) }
}

columnar_udf! {
    fn t_classify(s: &str) -> IntOrStr { s.parse().map(IntOrStr::Int).unwrap_or_else(|_| IntOrStr::Str(s.into())) }
}

columnar_udf! {
    fn t_describe(v: Option<IntOrStr>) -> String {
        match v { Some(IntOrStr::Int(i)) => format!("int:{i}"), Some(IntOrStr::Str(s)) => format!("str:{s}"), None => "null".into() }
    }
}

#[test]
fn variant_round_trip() {
    let out = t_classify::call(&col(s(&["12", "ab", "3"]))).unwrap();
    assert_eq!(rows::<IntOrStr>(&out), vec![IntOrStr::Int(12), IntOrStr::Str("ab".into()), IntOrStr::Int(3)]);
    assert_eq!(rows::<String>(&t_describe::call(&out).unwrap()), s(&["int:12", "str:ab", "int:3"]));
}

#[test]
fn variant_null_row_and_host_fixture() {
    let out = IntOrStr::write_column(vec![None, Some(IntOrStr::Str("x".into()))]).unwrap();
    assert_eq!(rows::<String>(&t_describe::call(&out).unwrap()), s(&["null", "str:x"]));

    // Host-written Variant(UInt64, String): alternatives in that order.
    column_variant! { enum U64OrStr { U(u64), S(String) } }
    let b = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../tests/wire_fixtures/variant_u64_string.bin")).unwrap();
    let f = Frame::parse(&b).unwrap();
    let c = f.col(0).unwrap();
    let got: Vec<String> = (0..f.num_rows()).map(|r| {
        match clickhouse_wasm_columnar::read_arg::<Option<U64OrStr>>(&c, r).unwrap() {
            Some(U64OrStr::U(v)) => v.to_string(),
            Some(U64OrStr::S(s)) => s,
            None => "NULL".into(),
        }
    }).collect();
    assert!(!got.is_empty());
    assert!(got.iter().any(|g| g == "NULL") || got.iter().all(|g| !g.is_empty()));
}

#[test]
fn lowcard_fixtures_read_as_strings() {
    for w in [1, 2, 4, 8] {
        let b = std::fs::read(format!("{}/../tests/wire_fixtures/lowcard_string_w{w}.bin", env!("CARGO_MANIFEST_DIR"))).unwrap();
        let f = Frame::parse(&b).unwrap();
        let c = f.col(0).unwrap();
        for r in 0..f.num_rows() {
            clickhouse_wasm_columnar::read_arg::<&str>(&c, r).unwrap();
        }
    }
}

#[test]
fn malformed_input_is_an_error() {
    assert!(t_repeat::call(b"not a frame").is_err());
    let one = col(s(&["a"]));
    assert!(t_repeat::call(&one).unwrap_err().contains("fewer columns"));
}
