//! terminal: the backend of the dumb mobile terminal (design/human/999.Mobile.md), as a box
//! under /terminal/v1/. Every route requires the executive's seat ("owner") and none says
//! "agents": true, so only the person, in a browser or through a person client like the
//! terminal, can reach it (design/machine/010.Terminal.md).
//!
//! Approvals are written by the desk box, which agents call; decisions live in their own
//! table, which only this box may insert into, so no agent has a write path to a decision.

#[path = "../../shared/goober.rs"]
mod goober;

use goober::{Request, error, respond_empty, respond_json, respond_raw_json, sql, sql_or_500};
use serde_json::{Value, json};

const DB: &str = "terminal";

/// GET /terminal/v1/me: who is signed in (999 decision 4).
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_me() -> i32 {
    let req = Request::read();
    let user = req.user();
    let email = user["email"].as_str().unwrap_or("");
    respond_json(200, &json!({
        "id": user["user_id"].as_str().unwrap_or(""),
        "name": email.split('@').next().unwrap_or(email),
        "email": email,
    }));
    0
}

/// GET /terminal/v1/config: the backend's display name; no push configuration yet, so the
/// terminal relies on its hourly pull (999 decision 7 is open).
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_config() -> i32 {
    let req = Request::read();
    let Some(r) = sql_or_500(DB, "SELECT name FROM settings ORDER BY id LIMIT 1", json!([])) else { return 0 };
    let name = r.first().and_then(|v| v.as_str()).map(str::to_string)
        .unwrap_or_else(|| req.doc["domain"].as_str().unwrap_or("goober").to_string());
    respond_json(200, &json!({ "name": name }));
    0
}

/// The snapshot, built by SQLite in one statement (999 § Snapshot): at most 64 alarms, the
/// pending approvals newest first (at most 200), the dashboard menu, and the default location.
/// Absent optional fields are left out rather than sent as null.
const SNAPSHOT: &str = "SELECT json_remove(json_object(
  'server_time', strftime('%Y-%m-%dT%H:%M:%SZ', :now / 1000, 'unixepoch'),
  'alarms', json((SELECT json_group_array(json(spec)) FROM
      (SELECT spec FROM alarms ORDER BY updated_ms DESC, id DESC LIMIT 64))),
  'approvals', json((SELECT json_group_array(json(item)) FROM (
      SELECT json_remove(json_object(
          'id', CAST(a.id AS TEXT), 'task', a.task, 'description', a.description, 'requester', a.requester,
          'risk', a.risk, 'created_at', strftime('%Y-%m-%dT%H:%M:%SZ', a.created_ms / 1000, 'unixepoch'),
          'expires_at', strftime('%Y-%m-%dT%H:%M:%SZ', a.expires_ms / 1000, 'unixepoch'),
          'actions', CASE WHEN a.actions = '' THEN NULL ELSE json(a.actions) END),
        CASE WHEN a.expires_ms IS NULL THEN '$.expires_at' ELSE '$.none' END,
        CASE WHEN a.actions = '' THEN '$.actions' ELSE '$.none' END) AS item
      FROM approvals a
      WHERE NOT EXISTS (SELECT 1 FROM decisions d WHERE d.approval_id = a.id)
        AND (a.expires_ms IS NULL OR a.expires_ms > :now)
      ORDER BY a.created_ms DESC, a.id DESC LIMIT 200))),
  'dashboards', json((SELECT json_group_array(json(item)) FROM (
      SELECT json_remove(json_object('id', dashboard_key, 'title', title, 'subtitle', subtitle),
        CASE WHEN subtitle IS NULL THEN '$.subtitle' ELSE '$.none' END) AS item
      FROM dashboards ORDER BY dashboard_key LIMIT 50))),
  'default_location', json((SELECT json_object('lat', lat, 'lon', lon, 'label', coalesce(label, ''))
      FROM settings WHERE lat IS NOT NULL AND lon IS NOT NULL ORDER BY id LIMIT 1))),
  CASE WHEN EXISTS (SELECT 1 FROM settings WHERE lat IS NOT NULL AND lon IS NOT NULL)
    THEN '$.none' ELSE '$.default_location' END)";

/// GET /terminal/v1/sync: the whole truth as of now.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_sync() -> i32 {
    let Some(r) = sql_or_500(DB, SNAPSHOT, json!({ "now": goober::now_ms() })) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{}"));
    0
}

/// GET /terminal/v1/dashboards/{dashboard_id}: one dashboard, stamped with when it was
/// served. Settings arrive as query parameters; stored dashboards don't use them yet.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_dashboard() -> i32 {
    let req = Request::read();
    let id = req.param("dashboard_id").unwrap_or("");
    let Some(r) = sql_or_500(DB, "SELECT spec FROM dashboards WHERE dashboard_key = ?", json!([id])) else { return 0 };
    let Some(spec) = r.first().and_then(|v| v.as_str()) else {
        error(404, "no such dashboard");
        return 0;
    };
    let mut d: Value = serde_json::from_str(spec).unwrap_or(json!({}));
    d["id"] = json!(id);
    if d.get("generated_at").is_none() {
        d["generated_at"] = json!(iso(goober::now_ms()));
    }
    respond_json(200, &d);
    0
}

/// POST /terminal/v1/approvals/{approval_id}/decision (999 § Approvals). Idempotent by key:
/// the same key again is a success that changes nothing; a different key on a decided
/// approval is 409.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_decide() -> i32 {
    let req = Request::read();
    let body = match req.json_body() {
        Ok(b) => b,
        Err(e) => return bad(&e),
    };
    let checked = (|| -> Result<(String, Option<String>, String), String> {
        goober::closed(&body, &["decision", "feedback", "decided_at", "idempotency_key"], "DecisionRequest")?;
        let decision = goober::enum_field(&body, "decision", true, &["approve", "deny", "approve_with_feedback"])?.unwrap();
        let feedback = goober::string_field(&body, "feedback", false, 4000)?;
        goober::instant_field(&body, "decided_at", true)?;
        let key = goober::string_field(&body, "idempotency_key", true, 200)?.unwrap();
        if key.is_empty() {
            return Err("'idempotency_key' can't be empty".into());
        }
        if req.header("idempotency-key") != Some(key) {
            return Err("the Idempotency-Key header must equal the body's idempotency_key".into());
        }
        match (decision, feedback) {
            ("approve", Some(_)) => return Err("approve carries no feedback".into()),
            ("approve_with_feedback", None) => return Err("approve_with_feedback needs feedback".into()),
            ("approve_with_feedback", Some(f)) if f.trim().is_empty() => return Err("approve_with_feedback needs feedback".into()),
            _ => {}
        }
        Ok((decision.to_string(), feedback.map(str::to_string), key.to_string()))
    })();
    let (decision, feedback, key) = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let Some(id) = req.param("approval_id").and_then(|s| s.parse::<i64>().ok()) else { return bad("bad approval id") };
    let user_id = req.user_id().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);

    let Some(found) = sql_or_500(
        DB,
        "SELECT a.actions, a.expires_ms, d.idempotency_key FROM approvals a
           LEFT JOIN decisions d ON d.approval_id = a.id WHERE a.id = ?",
        json!([id]),
    ) else { return 0 };
    let Some(row) = found.rows().first() else {
        error(404, "no such approval");
        return 0;
    };
    if let Some(existing) = row[2].as_str() {
        if existing == key {
            respond_empty(204); // a retry of a decision that already landed
        } else {
            error(409, "this approval was already decided");
        }
        return 0;
    }
    if let Some(expires) = row[1].as_i64() {
        if expires <= goober::now_ms() {
            error(409, "this approval expired");
            return 0;
        }
    }
    let actions = row[0].as_str().unwrap_or("");
    if !actions.is_empty() {
        let offered: Value = serde_json::from_str(actions).unwrap_or(json!([]));
        if !offered.as_array().is_some_and(|a| a.iter().any(|v| v.as_str() == Some(decision.as_str()))) {
            return bad(&format!("'{decision}' isn't offered for this approval"));
        }
    }
    // The decision is the write: one row, unique per approval, so a race can't land two.
    let r = sql(
        DB,
        "INSERT INTO decisions (approval_id, decision, feedback, decided_ms, user_id, idempotency_key)
           VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (approval_id) DO NOTHING",
        json!([id, decision, feedback, goober::now_ms(), user_id, key]),
    );
    if let Some(e) = r.error() {
        goober::log(&format!("decision failed: {e}"));
        error(500, "storage failed");
        return 0;
    }
    if r.changes() == 0 {
        error(409, "this approval was already decided");
        return 0;
    }
    goober::log(&format!("approval {id}: {decision}"));
    respond_empty(204);
    0
}

/// POST /terminal/v1/devices: one row per install, scoped by the person (999 § Devices and events).
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_devices() -> i32 {
    let req = Request::read();
    let body = match req.json_body() {
        Ok(b) => b,
        Err(e) => return bad(&e),
    };
    let checked = (|| -> Result<Value, String> {
        goober::closed(&body, &["platform", "app_version", "device_name", "timezone", "push_token"], "DeviceRegistration")?;
        let platform = goober::string_field(&body, "platform", true, 20)?;
        let version = goober::string_field(&body, "app_version", true, 40)?;
        let name = goober::string_field(&body, "device_name", true, 120)?;
        let tz = goober::string_field(&body, "timezone", true, 64)?;
        let push = goober::string_field(&body, "push_token", false, 4096)?;
        Ok(json!([platform, version, name, tz, push]))
    })();
    let v = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let user_id = req.user_id().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
    let Some(_) = sql_or_500(
        DB,
        "INSERT INTO devices (user_id, platform, app_version, device_name, timezone, push_token, updated_ms)
           VALUES (:user, :platform, :version, :name, :tz, :push, :now)
         ON CONFLICT (user_id, device_name) DO UPDATE SET platform = excluded.platform,
           app_version = excluded.app_version, timezone = excluded.timezone,
           push_token = excluded.push_token, updated_ms = excluded.updated_ms",
        json!({ "user": user_id, "platform": v[0], "version": v[1], "name": v[2], "tz": v[3], "push": v[4],
                "now": goober::now_ms() }),
    ) else { return 0 };
    respond_empty(204);
    0
}

/// POST /terminal/v1/events: alarm fired, snoozed, or dismissed; a repeated key is a success.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_terminal_events() -> i32 {
    let req = Request::read();
    let body = match req.json_body() {
        Ok(b) => b,
        Err(e) => return bad(&e),
    };
    let checked = (|| -> Result<Value, String> {
        goober::closed(&body, &["type", "at", "idempotency_key", "alarm_id", "minutes"], "ClientEvent")?;
        let kind = goober::enum_field(&body, "type", true, &["alarm_fired", "alarm_snoozed", "alarm_dismissed"])?;
        let at = goober::instant_field(&body, "at", true)?;
        let key = goober::string_field(&body, "idempotency_key", true, 200)?;
        let alarm = goober::string_field(&body, "alarm_id", false, 200)?;
        let minutes = goober::int32_field(&body, "minutes", 1, 120)?;
        if minutes.is_some() && kind != Some("alarm_snoozed") {
            return Err("'minutes' is only for alarm_snoozed".into());
        }
        Ok(json!([kind, at, key, alarm, minutes]))
    })();
    let v = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let user_id = req.user_id().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
    let Some(_) = sql_or_500(
        DB,
        "INSERT INTO events (user_id, type, at_ms, idempotency_key, alarm_id, minutes, received_ms)
           VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (user_id, idempotency_key) DO NOTHING",
        json!([user_id, v[0], v[1], v[2], v[3], v[4], goober::now_ms()]),
    ) else { return 0 };
    respond_empty(204);
    0
}

fn bad(message: &str) -> i32 {
    error(400, message);
    0
}

/// Unix milliseconds as an RFC 3339 instant, to the second.
fn iso(ms: i64) -> String {
    let secs = ms.div_euclid(1000);
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    // Civil date from days (Howard Hinnant's algorithm).
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z", rem / 3600, rem % 3600 / 60, rem % 60)
}
