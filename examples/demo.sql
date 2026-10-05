-- Load demo.wasm (copied into the server's user_files directory) and register
-- its functions.
INSERT INTO system.webassembly_modules (name, code) VALUES ('cwc_demo', file('demo.wasm'));

CREATE OR REPLACE FUNCTION demo_repeat LANGUAGE WASM FROM 'cwc_demo'
ARGUMENTS (s String, n UInt32) RETURNS String ABI COLUMNAR_V1 DETERMINISTIC;

CREATE OR REPLACE FUNCTION demo_parse_uint LANGUAGE WASM FROM 'cwc_demo'
ARGUMENTS (s String) RETURNS Nullable(Int64) ABI COLUMNAR_V1 DETERMINISTIC;

CREATE OR REPLACE FUNCTION demo_array_mean LANGUAGE WASM FROM 'cwc_demo'
ARGUMENTS (xs Array(Float64)) RETURNS Float64 ABI COLUMNAR_V1 DETERMINISTIC;

CREATE OR REPLACE FUNCTION demo_split LANGUAGE WASM FROM 'cwc_demo'
ARGUMENTS (s String, sep String) RETURNS Array(String) ABI COLUMNAR_V1 DETERMINISTIC;

CREATE OR REPLACE FUNCTION demo_divmod LANGUAGE WASM FROM 'cwc_demo'
ARGUMENTS (a Int64, b Int64) RETURNS Tuple(Int64, Int64) ABI COLUMNAR_V1 DETERMINISTIC;
