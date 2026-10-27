//! guestbook: a box that keeps its entries in the tenant's database
//! (design/machine/008.Databases.md). GET /guestbook answers JSON, which the host renders
//! through the "guestbook" template for a browser; POST /guestbook signs it from a form.

mod goober;

// The response is built by SQLite itself, so the box never parses rows.
const SHOW: &str = "SELECT json_object(
    'title', 'Guestbook',
    'count', (SELECT count(*) FROM entries),
    'entries', (SELECT json_group_array(json_object(
        'id', id, 'name', name, 'message', message,
        'at', strftime('%Y-%m-%d %H:%M UTC', created_ms / 1000, 'unixepoch')))
      FROM (SELECT * FROM entries ORDER BY id DESC LIMIT 50)))";

const SIGN: &str = "INSERT INTO entries (name, message, created_ms) VALUES (?, ?, ?)";

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_show() -> i32 {
    let result = goober::sql("book", SHOW, "[]");
    match goober::first_text(&result) {
        Some(json) => {
            goober::header("cache-control", "no-store");
            goober::respond(200, "application/json", json.as_bytes());
        }
        None => {
            goober::log(&format!("show failed: {result}"));
            goober::respond(500, "application/json", result.as_bytes());
        }
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_sign() -> i32 {
    let doc = goober::request();
    let body = goober::top_field(&doc, "body").unwrap_or_default();
    let name = goober::form_field(&body, "name").unwrap_or_default().trim().to_string();
    let message = goober::form_field(&body, "message").unwrap_or_default().trim().to_string();
    if name.is_empty() || message.is_empty() || name.chars().count() > 80 || message.chars().count() > 2000 {
        goober::respond(400, "application/json", br#"{"error":"a name (up to 80) and a message (up to 2000) are required"}"#);
        return 0;
    }
    let params = format!("[{},{},{}]", goober::json_string(&name), goober::json_string(&message), goober::now_ms());
    let result = goober::sql("book", SIGN, &params);
    if result.contains("\"error\"") {
        goober::log(&format!("sign failed: {result}"));
        goober::respond(500, "application/json", result.as_bytes());
        return 0;
    }
    goober::log(&format!("{name} signed"));
    goober::header("location", "/guestbook");
    goober::respond(303, "text/plain; charset=utf-8", b"Thanks for signing.\n");
    0
}
