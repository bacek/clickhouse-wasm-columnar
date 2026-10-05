#!/usr/bin/env python3
"""Load examples/demo.wasm into a running ClickHouse server and check the results.

    tests/e2e.py --clickhouse /path/to/clickhouse --wasm build_wasm/examples/demo.wasm [--port 9000]

The server needs WASM UDFs with the COLUMNAR_V1 ABI.  The module is sent inline
(base64), so no access to the server's user_files directory is needed.  The
module and its functions are dropped at the end, also on failure.
"""
import argparse
import base64
import pathlib
import subprocess
import sys

MODULE = "cwc_demo_e2e"

FUNCTIONS = {
    "demo_repeat":     ("s String, n UInt32",      "String"),
    "demo_parse_uint": ("s String",                "Nullable(Int64)"),
    "demo_array_mean": ("xs Array(Float64)",       "Float64"),
    "demo_split":      ("s String, sep String",    "Array(String)"),
    "demo_divmod":     ("a Int64, b Int64",        "Tuple(Int64, Int64)"),
}

# (query, expected TSV output)
CASES = [
    ("SELECT demo_repeat('ab', 3)", "ababab"),
    ("SELECT demo_repeat(s, 2) FROM values('s String', 'x', '', 'yz') ORDER BY s", "\nxx\nyzyz"),
    ("SELECT demo_parse_uint(s) FROM values('s String', '42', 'x1', '', '007') ORDER BY s",
     "\\N\n7\n42\n\\N"),
    ("SELECT demo_parse_uint(NULL::Nullable(String))", "\\N"),
    ("SELECT demo_array_mean([1., 2., 6.])", "3"),
    ("SELECT demo_array_mean(arr) FROM values('arr Array(Float64)', [], [10.], [1., 3.]) ORDER BY length(arr)",
     "0\n10\n2"),
    ("SELECT demo_split('a,b,,c', ',')", "['a','b','','c']"),
    ("SELECT demo_divmod(17, 5)", "(3,2)"),
    ("SELECT demo_divmod(-7, 2)", "(-3,-1)"),
    # A constant argument broadcast over many rows, then aggregated.
    ("SELECT sum(length(demo_repeat('abc', toUInt32(number % 4)))) FROM numbers(100000)", "450000"),
]

ERROR_CASES = [
    ("SELECT demo_divmod(1, 0)", "division by zero"),
]


def run(ch, port, query, stdin=None):
    return subprocess.run(
        [ch, "client", "--port", str(port), "--query", query],
        input=stdin, capture_output=True, text=True, timeout=120)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clickhouse", required=True, help="clickhouse binary")
    ap.add_argument("--wasm", required=True, help="demo.wasm")
    ap.add_argument("--port", type=int, default=9000)
    args = ap.parse_args()

    code = base64.b64encode(pathlib.Path(args.wasm).read_bytes()).decode()

    def q(query, stdin=None):
        r = run(args.clickhouse, args.port, query, stdin)
        if r.returncode != 0:
            raise RuntimeError(f"{query[:80]}...\n{r.stderr.strip()}")
        return r.stdout.rstrip("\n")

    def cleanup():
        for name in FUNCTIONS:
            run(args.clickhouse, args.port, f"DROP FUNCTION IF EXISTS {name}")
        run(args.clickhouse, args.port, f"DELETE FROM system.webassembly_modules WHERE name = '{MODULE}'")

    cleanup()
    failures = 0
    try:
        q(f"INSERT INTO system.webassembly_modules (name, code) SELECT '{MODULE}', base64Decode('{code}')")
        for name, (args_sql, ret) in FUNCTIONS.items():
            q(f"CREATE FUNCTION {name} LANGUAGE WASM FROM '{MODULE}' "
              f"ARGUMENTS ({args_sql}) RETURNS {ret} ABI COLUMNAR_V1 DETERMINISTIC")

        for query, want in CASES:
            try:
                got = q(query)
            except RuntimeError as e:
                got = f"ERROR: {e}"
            ok = got == want
            failures += not ok
            print(("ok   " if ok else "FAIL ") + query)
            if not ok:
                print(f"     want: {want!r}\n     got:  {got!r}")

        for query, needle in ERROR_CASES:
            r = run(args.clickhouse, args.port, query)
            ok = r.returncode != 0 and needle in r.stderr
            failures += not ok
            print(("ok   " if ok else "FAIL ") + query + "  (expects error)")
            if not ok:
                print(f"     want error containing {needle!r}, got rc={r.returncode} {r.stderr.strip()[:200]!r}")
    finally:
        cleanup()

    total = len(CASES) + len(ERROR_CASES)
    print(f"\n{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
