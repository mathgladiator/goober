//! A hand-written binding to goober-abi@1 (design/machine/005.FirstVersion.md §3, and
//! goober_sql from design/machine/008.Databases.md §5).
//! The generators will stamp this out per manifest; until then it is small enough to copy.

#![allow(dead_code)]

#[link(wasm_import_module = "goober")]
unsafe extern "C" {
    fn goober_request_size() -> i32;
    fn goober_request_read(ptr: *mut u8, len: i32) -> i32;
    fn goober_respond(status: i32, type_ptr: *const u8, type_len: i32, body_ptr: *const u8, body_len: i32) -> i32;
    fn goober_header_set(name_ptr: *const u8, name_len: i32, value_ptr: *const u8, value_len: i32) -> i32;
    fn goober_now_ms() -> i64;
    fn goober_random_fill(ptr: *mut u8, len: i32) -> i32;
    fn goober_log(level: i32, ptr: *const u8, len: i32) -> i32;
    fn goober_sql(st_ptr: *const u8, st_len: i32, sql_ptr: *const u8, sql_len: i32, p_ptr: *const u8, p_len: i32) -> i32;
    fn goober_result_read(ptr: *mut u8, len: i32) -> i32;
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

/// Runs one statement through a strategy of the manifest's data.sqlite3, with params as
/// a JSON array, and returns the result JSON: {"columns","rows",...} or {"error"}.
pub fn sql(strategy: &str, sql: &str, params: &str) -> String {
    unsafe {
        let n = goober_sql(
            strategy.as_ptr(), strategy.len() as i32, sql.as_ptr(), sql.len() as i32, params.as_ptr(), params.len() as i32,
        );
        if n < 0 {
            return String::from(r#"{"error":"bad pointer"}"#);
        }
        let mut buf = vec![0u8; n as usize];
        let got = goober_result_read(buf.as_mut_ptr(), n);
        buf.truncate(got.max(0) as usize);
        String::from_utf8(buf).unwrap_or_default()
    }
}

fn hex4(chars: &mut std::str::CharIndices) -> Option<u32> {
    let mut v = 0;
    for _ in 0..4 {
        v = v * 16 + chars.next()?.1.to_digit(16)?;
    }
    Some(v)
}

/// Reads a JSON string starting at its opening quote; returns it and the rest.
pub fn read_json_string(s: &str) -> Option<(String, &str)> {
    let mut chars = s.strip_prefix('"')?.char_indices();
    let mut out = String::new();
    let body = &s[1..];
    while let Some((i, c)) = chars.next() {
        match c {
            '"' => return Some((out, &body[i + 1..])),
            '\\' => match chars.next()?.1 {
                'n' => out.push('\n'),
                't' => out.push('\t'),
                'r' => out.push('\r'),
                'b' => out.push('\u{8}'),
                'f' => out.push('\u{c}'),
                'u' => {
                    let mut code = hex4(&mut chars)?;
                    if (0xD800..0xDC00).contains(&code) {
                        chars.next()?;
                        chars.next()?;
                        let low = hex4(&mut chars)?;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    out.push(char::from_u32(code)?);
                }
                other => out.push(other),
            },
            c => out.push(c),
        }
    }
    None
}

/// A top-level string field of the request document, like "body" or "method". The
/// request's later fields hold no key named like an earlier one, so the last match is it.
pub fn top_field(doc: &str, key: &str) -> Option<String> {
    let at = doc.rfind(&format!("\"{key}\":\""))? + key.len() + 3;
    read_json_string(&doc[at..]).map(|(s, _)| s)
}

/// The text in the first cell of a result, like the JSON a json_object() query built.
pub fn first_text(result: &str) -> Option<String> {
    let at = result.find("\"rows\":[[")? + 9;
    read_json_string(&result[at..]).map(|(s, _)| s)
}

/// A string as a JSON string literal.
pub fn json_string(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// A field of an application/x-www-form-urlencoded body.
pub fn form_field(body: &str, key: &str) -> Option<String> {
    for pair in body.split('&') {
        let (k, v) = pair.split_once('=').unwrap_or((pair, ""));
        if percent_decode(k) == key {
            return Some(percent_decode(v));
        }
    }
    None
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let hex = |at: usize| bytes.get(at..at + 2).and_then(|h| std::str::from_utf8(h).ok()).and_then(|h| u8::from_str_radix(h, 16).ok());
        match bytes[i] {
            b'+' => out.push(b' '),
            b'%' => match hex(i + 1) {
                Some(b) => {
                    out.push(b);
                    i += 2;
                }
                None => out.push(b'%'),
            },
            b => out.push(b),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}
