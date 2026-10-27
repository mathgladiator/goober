//! A hand-written binding to goober-abi@1 (design/machine/005.FirstVersion.md §3, and
//! goober_sql from design/machine/008.Databases.md §5), with serde_json for the request
//! document and SQL results. Shared by the terminal and desk boxes through #[path]; the
//! generators of 110.Languages.md will stamp this out per manifest.

#![allow(dead_code)]

use serde_json::{Value, json};

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
    fn goober_call(sv_ptr: *const u8, sv_len: i32, m_ptr: *const u8, m_len: i32, r_ptr: *const u8, r_len: i32) -> i32;
}

/// Calls a method of a service declared under "external" (design/machine/015.Network.md).
/// `request` is {"path": {...}, "query": {...}, "body": ...}. A safe call answers
/// {"status", "content_type", "body", "json"?}; an outbox call answers {"accepted": true,
/// "key"} and goes out only if the request commits; failures answer {"error"}.
pub fn call(service: &str, method: &str, request: Value) -> Value {
    let r = request.to_string();
    let text = unsafe {
        let n = goober_call(
            service.as_ptr(), service.len() as i32,
            method.as_ptr(), method.len() as i32,
            r.as_ptr(), r.len() as i32,
        );
        let mut buf = vec![0u8; n.max(0) as usize];
        let got = goober_result_read(buf.as_mut_ptr(), n.max(0));
        buf.truncate(got.max(0) as usize);
        buf
    };
    serde_json::from_slice(&text).unwrap_or_else(|_| json!({ "error": "unreadable result" }))
}

/// The request document (005 §3): method, path, params, query, headers, body, user, ...
pub struct Request {
    pub doc: Value,
}

impl Request {
    pub fn read() -> Request {
        let text = unsafe {
            let size = goober_request_size();
            let mut buf = vec![0u8; size.max(0) as usize];
            let n = goober_request_read(buf.as_mut_ptr(), size);
            buf.truncate(n.max(0) as usize);
            buf
        };
        Request { doc: serde_json::from_slice(&text).unwrap_or(Value::Null) }
    }
    pub fn param(&self, name: &str) -> Option<&str> {
        self.doc["params"][name].as_str()
    }
    pub fn query(&self, name: &str) -> Option<&str> {
        self.doc["query"][name].as_str()
    }
    /// A request header, by its lowercase name.
    pub fn header(&self, name: &str) -> Option<&str> {
        self.doc["headers"][name].as_str()
    }
    pub fn body(&self) -> &str {
        self.doc["body"].as_str().unwrap_or("")
    }
    /// The signed-in person's user_id, as the host's string form.
    pub fn user_id(&self) -> Option<&str> {
        self.doc["user"]["user_id"].as_str()
    }
    pub fn user(&self) -> &Value {
        &self.doc["user"]
    }
    /// The body parsed as a JSON object, or a 400-ready message.
    pub fn json_body(&self) -> Result<serde_json::Map<String, Value>, String> {
        // A tool's input is the body, already checked by the host; it has no content type.
        let ct = self.header("content-type").unwrap_or("");
        if self.doc["method"] != "tool" && !ct.starts_with("application/json") {
            return Err("Content-Type must be application/json".into());
        }
        match serde_json::from_str::<Value>(self.body()) {
            Ok(Value::Object(m)) => Ok(m),
            Ok(_) => Err("the body must be a JSON object".into()),
            Err(_) => Err("the body is not JSON".into()),
        }
    }
}

pub fn respond(status: i32, content_type: &str, body: &[u8]) {
    unsafe {
        goober_respond(status, content_type.as_ptr(), content_type.len() as i32, body.as_ptr(), body.len() as i32);
    }
}

pub fn respond_json(status: i32, v: &Value) {
    header("cache-control", "no-store");
    respond(status, "application/json", v.to_string().as_bytes());
}

/// A JSON object written by SQLite, passed through as it is.
pub fn respond_raw_json(status: i32, text: &str) {
    header("cache-control", "no-store");
    respond(status, "application/json", text.as_bytes());
}

pub fn respond_empty(status: i32) {
    respond(status, "application/json", b"");
}

pub fn error(status: i32, message: &str) {
    respond_json(status, &json!({ "error": message }));
}

pub fn header(name: &str, value: &str) -> bool {
    unsafe { goober_header_set(name.as_ptr(), name.len() as i32, value.as_ptr(), value.len() as i32) == 0 }
}

/// The request's time in Unix milliseconds, fixed for the whole request.
pub fn now_ms() -> i64 {
    unsafe { goober_now_ms() }
}

/// `bytes` random bytes from the host (seeded per request, so a replay repeats them), as hex.
pub fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    unsafe {
        goober_random_fill(buf.as_mut_ptr(), bytes as i32);
    }
    buf.iter().map(|b| format!("{b:02x}")).collect()
}

/// The fields of a plain HTML form post (the host decodes them), if this request is one.
pub fn form(req: &Request) -> Option<&serde_json::Map<String, Value>> {
    req.doc.get("form").and_then(|f| f.as_object())
}

/// Answers 303 See Other to a path on this site.
pub fn redirect(path: &str) {
    header("location", path);
    respond(303, "text/plain", b"");
}

pub fn log(text: &str) {
    unsafe {
        goober_log(1, text.as_ptr(), text.len() as i32);
    }
}

/// The result of one statement: columns, rows, changes, last_insert_id; or an error.
pub struct Rows {
    pub v: Value,
}

impl Rows {
    pub fn error(&self) -> Option<&str> {
        self.v["error"].as_str()
    }
    pub fn rows(&self) -> &[Value] {
        self.v["rows"].as_array().map(|a| a.as_slice()).unwrap_or(&[])
    }
    pub fn first(&self) -> Option<&Value> {
        self.rows().first().and_then(|r| r.get(0))
    }
    pub fn changes(&self) -> i64 {
        self.v["changes"].as_i64().unwrap_or(0)
    }
    pub fn last_insert_id(&self) -> i64 {
        self.v["last_insert_id"].as_i64().unwrap_or(0)
    }
}

/// Runs one statement through a data strategy (008 §5).
pub fn sql(strategy: &str, statement: &str, params: Value) -> Rows {
    let p = params.to_string();
    let text = unsafe {
        let n = goober_sql(
            strategy.as_ptr(), strategy.len() as i32,
            statement.as_ptr(), statement.len() as i32,
            p.as_ptr(), p.len() as i32,
        );
        let mut buf = vec![0u8; n.max(0) as usize];
        let got = goober_result_read(buf.as_mut_ptr(), n.max(0));
        buf.truncate(got.max(0) as usize);
        buf
    };
    Rows { v: serde_json::from_slice(&text).unwrap_or_else(|_| json!({ "error": "unreadable result" })) }
}

/// Runs a statement whose failure means the request can't go on: logs it, answers 500,
/// and returns None so the route can stop.
pub fn sql_or_500(strategy: &str, statement: &str, params: Value) -> Option<Rows> {
    let r = sql(strategy, statement, params);
    if let Some(e) = r.error() {
        log(&format!("sql failed: {e}"));
        error(500, "storage failed");
        return None;
    }
    Some(r)
}

// ---- Checks for the strict JSON subset (036), until the host validates bodies itself.

/// Refuses fields the type doesn't name: objects are closed.
pub fn closed(body: &serde_json::Map<String, Value>, fields: &[&str], type_name: &str) -> Result<(), String> {
    for k in body.keys() {
        if !fields.contains(&k.as_str()) {
            return Err(format!("{type_name}: unknown field '{k}'"));
        }
    }
    Ok(())
}

pub fn string_field<'a>(
    body: &'a serde_json::Map<String, Value>, name: &str, required: bool, max_chars: usize,
) -> Result<Option<&'a str>, String> {
    match body.get(name) {
        None => if required { Err(format!("'{name}' is required")) } else { Ok(None) },
        Some(Value::String(s)) => {
            if s.chars().count() > max_chars {
                Err(format!("'{name}' is longer than {max_chars} characters"))
            } else {
                Ok(Some(s.as_str()))
            }
        }
        Some(_) => Err(format!("'{name}' must be a string")),
    }
}

pub fn enum_field<'a>(
    body: &'a serde_json::Map<String, Value>, name: &str, required: bool, values: &[&str],
) -> Result<Option<&'a str>, String> {
    let v = string_field(body, name, required, 64)?;
    if let Some(s) = v {
        if !values.contains(&s) {
            return Err(format!("'{name}' must be one of {}", values.join(", ")));
        }
    }
    Ok(v)
}

pub fn int32_field(body: &serde_json::Map<String, Value>, name: &str, min: i64, max: i64) -> Result<Option<i64>, String> {
    match body.get(name) {
        None => Ok(None),
        Some(v) => match v.as_i64() {
            Some(n) if v.is_i64() && n >= min && n <= max => Ok(Some(n)),
            _ => Err(format!("'{name}' must be an integer from {min} to {max}")),
        },
    }
}

/// An RFC 3339 instant in UTC, like 2026-10-27T13:00:00Z, as Unix milliseconds.
pub fn parse_instant(s: &str) -> Option<i64> {
    let b = s.as_bytes();
    if b.len() < 20 || *b.last()? != b'Z' || b[4] != b'-' || b[7] != b'-' || b[10] != b'T' || b[13] != b':' || b[16] != b':' {
        return None;
    }
    let num = |r: std::ops::Range<usize>| -> Option<i64> {
        let t = s.get(r)?;
        if !t.bytes().all(|c| c.is_ascii_digit()) { return None; }
        t.parse().ok()
    };
    let (y, mo, d, h, mi, se) = (num(0..4)?, num(5..7)?, num(8..10)?, num(11..13)?, num(14..16)?, num(17..19)?);
    let mut ms = 0i64;
    if b.len() > 20 {
        if b[19] != b'.' { return None; }
        let frac = s.get(20..s.len() - 1)?;
        if frac.is_empty() || frac.len() > 9 || !frac.bytes().all(|c| c.is_ascii_digit()) { return None; }
        ms = format!("{:0<3}", &frac[..frac.len().min(3)]).parse().ok()?;
    }
    if !(1..=12).contains(&mo) || !(1..=31).contains(&d) || h > 23 || mi > 59 || se > 60 {
        return None;
    }
    // Days from the civil date (Howard Hinnant's algorithm).
    let (y2, m2) = if mo <= 2 { (y - 1, mo + 9) } else { (y, mo - 3) };
    let era = y2.div_euclid(400);
    let yoe = y2 - era * 400;
    let doy = (153 * m2 + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146097 + doe - 719468;
    Some(((days * 24 + h) * 60 + mi) * 60_000 + se * 1000 + ms)
}

pub fn instant_field(body: &serde_json::Map<String, Value>, name: &str, required: bool) -> Result<Option<i64>, String> {
    match string_field(body, name, required, 40)? {
        None => Ok(None),
        Some(s) => parse_instant(s).map(Some).ok_or_else(|| format!("'{name}' must be an RFC 3339 UTC instant, like 2026-10-27T13:00:00Z")),
    }
}
