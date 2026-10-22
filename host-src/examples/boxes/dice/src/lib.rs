//! dice: a toy box for playing with the host. GET /roll/{sides} rolls a die with
//! the host's replayable randomness; GET /hello/{name} says hello in HTML.

mod goober;

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_roll() -> i32 {
    let doc = goober::request();
    let sides: u64 = goober::field(&doc, "params", "sides").and_then(|s| s.parse().ok()).unwrap_or(6);
    if sides < 2 || sides > 1000 {
        goober::respond(400, "application/json", br#"{"error":"sides must be 2 to 1000"}"#);
        return 0;
    }
    let roll = goober::random_u64() % sides + 1;
    goober::log(&format!("rolled {roll} on a d{sides}"));
    let body = format!(r#"{{"sides":"{sides}","roll":"{roll}","at_ms":"{}"}}"#, goober::now_ms());
    goober::header("cache-control", "no-store");
    goober::respond(200, "application/json", body.as_bytes());
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_hello() -> i32 {
    let doc = goober::request();
    let name = goober::field(&doc, "params", "name").unwrap_or_else(|| "stranger".to_string());
    let body = format!(
        "<!doctype html><meta charset=\"utf-8\"><title>Hello</title><h1>Hello, {}!</h1><p>From a WASM box.</p>\n",
        goober::html_escape(&name)
    );
    goober::respond(200, "text/html; charset=utf-8", body.as_bytes());
    0
}
