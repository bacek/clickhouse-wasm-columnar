//! The same functions as examples/demo.cpp, so tests/e2e.py checks both.
//!
//!     cargo build --release --target wasm32-unknown-unknown
use clickhouse_wasm_columnar::{column_variant, columnar_udf};
use std::collections::BTreeMap;

fn parse_uint(s: &str) -> Option<i64> {
    if s.is_empty() || s.len() > 18 || !s.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

columnar_udf! {
    /// String × UInt32 → String
    fn demo_repeat(s: &str, n: u32) -> String { s.repeat(n as usize) }
}

columnar_udf! {
    /// String → Nullable(Int64): NULL when the text is not a non-negative integer.
    fn demo_parse_uint(s: &str) -> Option<i64> { parse_uint(s) }
}

columnar_udf! {
    /// Array(Float64) → Float64
    fn demo_array_mean(xs: Vec<f64>) -> f64 {
        if xs.is_empty() { 0.0 } else { xs.iter().sum::<f64>() / xs.len() as f64 }
    }
}

columnar_udf! {
    /// String → Array(String), split on a single-character separator.
    fn demo_split(s: &str, sep: &str) -> Vec<String> {
        if sep.len() != 1 { return vec![s.to_owned()]; }
        s.split(sep).map(str::to_owned).collect()
    }
}

columnar_udf! {
    /// Int64 × Int64 → Tuple(Int64, Int64): quotient and remainder.
    fn demo_divmod(a: i64, b: i64) -> Result<(i64, i64), &'static str> {
        if b == 0 { Err("divmod: division by zero") } else { Ok((a / b, a % b)) }
    }
}

columnar_udf! {
    /// Tuple(Int64, Float64) → Float64
    fn demo_tuple_sum(t: (i64, f64)) -> f64 { t.0 as f64 + t.1 }
}

columnar_udf! {
    /// Array(Array(Int64)) → Array(Int64)
    fn demo_flatten(xss: Vec<Vec<i64>>) -> Vec<i64> { xss.concat() }
}

columnar_udf! {
    /// Array(Nullable(String)) → Array(Nullable(String)): upper-cases, keeps NULLs.
    fn demo_upper_all(xs: Vec<Option<String>>) -> Vec<Option<String>> {
        xs.into_iter().map(|x| x.map(|s| s.to_ascii_uppercase())).collect()
    }
}

columnar_udf! {
    /// Map(String, Int64) × String → Nullable(Int64)
    fn demo_map_get(m: BTreeMap<&str, i64>, key: &str) -> Option<i64> { m.get(key).copied() }
}

columnar_udf! {
    /// Map(String, Int64) → Map(Int64, String)
    fn demo_map_invert(m: BTreeMap<String, i64>) -> BTreeMap<i64, String> {
        m.into_iter().map(|(k, v)| (v, k)).collect()
    }
}

column_variant! {
    /// Variant(Int64, String): alternatives in ClickHouse's order (by type name).
    pub enum IntOrStr { Int(i64), Str(String) }
}

columnar_udf! {
    /// Variant(Int64, String) → String
    fn demo_describe(v: IntOrStr) -> String {
        match v { IntOrStr::Int(i) => format!("int:{i}"), IntOrStr::Str(s) => format!("str:{s}") }
    }
}

columnar_udf! {
    /// String → Variant(Int64, String)
    fn demo_classify(s: &str) -> IntOrStr {
        parse_uint(s).map(IntOrStr::Int).unwrap_or_else(|| IntOrStr::Str(s.to_owned()))
    }
}

columnar_udf! {
    /// Nullable(Int64) → String: the argument may be NULL.
    fn demo_show_nullable(v: Option<i64>) -> String { v.map_or_else(|| "null".into(), |v| v.to_string()) }
}

columnar_udf! {
    /// Int64 → Nullable(Tuple(Int64, Int64)): NULL for negative input.
    fn demo_halves(v: i64) -> Option<(i64, i64)> { (v >= 0).then(|| (v / 2, v - v / 2)) }
}

columnar_udf! {
    /// UUID → String: the 16 raw bytes as hex, as stored.
    fn demo_uuid_bytes(u: [u8; 16]) -> String { u.iter().map(|b| format!("{b:02x}")).collect() }
}

columnar_udf! {
    /// LowCardinality(FixedString(3)) → String
    fn demo_lc_fixed(s: &str) -> String { format!("{s}!") }
}

columnar_udf! {
    /// LowCardinality(UInt32) → UInt64
    fn demo_lc_double(v: u32) -> u64 { v as u64 * 2 }
}
