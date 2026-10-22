//! A hand-written binding to goober-abi@1 (design/machine/005.FirstVersion.md §3).
//! The generators will stamp this out per manifest; until then it is small enough to copy.

#[link(wasm_import_module = "goober")]
unsafe extern "C" {
    fn goober_request_size() -> i32;
    fn goober_request_read(ptr: *mut u8, len: i32) -> i32;
    fn goober_respond(status: i32, type_ptr: *const u8, type_len: i32, body_ptr: *const u8, body_len: i32) -> i32;
    fn goober_header_set(name_ptr: *const u8, name_len: i32, value_ptr: *const u8, value_len: i32) -> i32;
    fn goober_now_ms() -> i64;
    fn goober_random_fill(ptr: *mut u8, len: i32) -> i32;
    fn goober_log(level: i32, ptr: *const u8, len: i32) -> i32;
}

/// The request document, as JSON text.
pub fn request() -> String {
    unsafe {
        let size = goober_request_size();
        let mut buf = vec![0u8; size as usize];
        let n = goober_request_read(buf.as_mut_ptr(), size);
        buf.truncate(n.max(0) as usize);
        String::from_utf8(buf).unwrap_or_default()
    }
}

pub fn respond(status: i32, content_type: &str, body: &[u8]) {
    unsafe {
        goober_respond(status, content_type.as_ptr(), content_type.len() as i32, body.as_ptr(), body.len() as i32);
    }
}

/// Adds a response header. Host-owned headers are refused (returns false).
pub fn header(name: &str, value: &str) -> bool {
    unsafe { goober_header_set(name.as_ptr(), name.len() as i32, value.as_ptr(), value.len() as i32) == 0 }
}

/// The request's time in Unix milliseconds, fixed for the whole request.
pub fn now_ms() -> i64 {
    unsafe { goober_now_ms() }
}

/// Bytes from the host's per-request generator. Replayable from the seed in the access log.
pub fn random_u64() -> u64 {
    let mut b = [0u8; 8];
    unsafe {
        goober_random_fill(b.as_mut_ptr(), 8);
    }
    u64::from_le_bytes(b)
}

pub fn log(text: &str) {
    unsafe {
        goober_log(1, text.as_ptr(), text.len() as i32);
    }
}

/// Finds a string field inside an object field of the request, like
/// field(doc, "params", "name"). A tiny scanner, so the box needs no crates.
pub fn field(doc: &str, object: &str, key: &str) -> Option<String> {
    let start = doc.find(&format!("\"{object}\":{{"))? + object.len() + 4;
    let rest = &doc[start..];
    let end = rest.find('}')?;
    let obj = &rest[..end];
    let k = obj.find(&format!("\"{key}\":\""))? + key.len() + 4;
    let mut out = String::new();
    let mut chars = obj[k..].chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next()? {
                'n' => out.push('\n'),
                't' => out.push('\t'),
                other => out.push(other),
            },
            c => out.push(c),
        }
    }
    None
}

/// Escapes text for HTML.
pub fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;").replace('\'', "&#39;")
}
