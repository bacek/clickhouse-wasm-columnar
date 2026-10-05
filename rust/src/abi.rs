//! The WebAssembly boundary: buffers ClickHouse allocates in the module,
//! host imports, and the per-call entry point used by `columnar_udf!`.

/// The buffer handle ClickHouse passes in and gets back. On wasm32 the layout
/// is `{u32 data, u32 size, u32 capacity}`, the same as the C++ library's.
#[repr(C)]
pub struct RawBuffer {
    data: *mut u8,
    size: u32,
    cap: u32,
}

impl RawBuffer {
    fn from_vec(v: Vec<u8>) -> *mut RawBuffer {
        let mut v = std::mem::ManuallyDrop::new(v);
        let (data, size, cap) = (v.as_mut_ptr(), v.len() as u32, v.capacity() as u32);
        Box::into_raw(Box::new(RawBuffer { data, size, cap }))
    }

    #[cfg(target_arch = "wasm32")]
    /// # Safety
    /// `b` must come from `from_vec` and not be used afterwards.
    unsafe fn into_vec(b: *mut RawBuffer) -> Vec<u8> {
        let b = Box::from_raw(b);
        Vec::from_raw_parts(b.data, b.size as usize, b.cap as usize)
    }
}

#[cfg(target_arch = "wasm32")]
#[link(wasm_import_module = "env")]
extern "C" {
    fn clickhouse_throw(msg: *const u8, len: u32) -> !;
    fn clickhouse_log(level: u32, msg: *const u8, len: u32);
}

/// Abort the call with a ClickHouse exception carrying `msg`.
pub fn throw(msg: &str) -> ! {
    #[cfg(target_arch = "wasm32")]
    unsafe {
        clickhouse_throw(msg.as_ptr(), msg.len() as u32)
    }
    #[cfg(not(target_arch = "wasm32"))]
    panic!("{msg}")
}

/// Log levels understood by the host.
#[derive(Clone, Copy, Debug)]
pub enum LogLevel {
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
}

/// Write `msg` to the ClickHouse server log.
pub fn log(level: LogLevel, msg: &str) {
    #[cfg(target_arch = "wasm32")]
    unsafe {
        clickhouse_log(level as u32, msg.as_ptr(), msg.len() as u32)
    }
    #[cfg(not(target_arch = "wasm32"))]
    eprintln!("[{level:?}] {msg}");
}

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub extern "C" fn clickhouse_create_buffer(size: u32) -> *mut RawBuffer {
    RawBuffer::from_vec(vec![0; size as usize])
}

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub unsafe extern "C" fn clickhouse_destroy_buffer(b: *mut RawBuffer) {
    if !b.is_null() {
        drop(RawBuffer::into_vec(b));
    }
}

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub unsafe extern "C" fn clickhouse_reallocate_buffer(b: *mut RawBuffer, size: u32) -> *mut RawBuffer {
    if b.is_null() {
        return clickhouse_create_buffer(size);
    }
    let mut v = RawBuffer::into_vec(b);
    v.resize(size as usize, 0);
    RawBuffer::from_vec(v)
}

/// Run one exported call: route panics to the host, read the input buffer,
/// and hand back the result frame. Used by `columnar_udf!`.
///
/// # Safety
/// `input` must be null or a live buffer from `clickhouse_create_buffer`.
pub unsafe fn run(input: *mut RawBuffer, call: fn(&[u8]) -> Result<Vec<u8>, String>) -> *mut RawBuffer {
    static HOOK: std::sync::Once = std::sync::Once::new();
    HOOK.call_once(|| std::panic::set_hook(Box::new(|info| throw(&info.to_string()))));
    if input.is_null() {
        throw("columnar: no input frame (null buffer handle)");
    }
    let b = &*input;
    let bytes = if b.size == 0 { &[][..] } else { std::slice::from_raw_parts(b.data, b.size as usize) };
    match call(bytes) {
        Ok(out) => RawBuffer::from_vec(out),
        Err(e) => throw(&e),
    }
}
