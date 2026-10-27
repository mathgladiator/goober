//! desk: where the executive's agents work with the terminal (design/machine/010.Terminal.md).
//! Agents ask for approvals and read the answers, and set the alarms, dashboards, and
//! settings the terminal shows. Every route says "agents": true. The box's strategy can
//! insert approvals but has no grant on the decisions table beyond reading it, so the
//! checker's question "can an agent approve its own request?" has the answer no, by grant.

#[path = "../../shared/goober.rs"]
mod goober;

use goober::{Request, error, respond_json, respond_raw_json, sql_or_500};
use serde_json::{Map, Value, json};

const DB: &str = "desk";
const DAYS: [&str; 7] = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"];
const SOLAR: [&str; 11] = [
    "sunrise", "sunset", "civil_dawn", "civil_dusk", "nautical_dawn", "nautical_dusk", "astronomical_dawn",
    "astronomical_dusk", "golden_hour_start", "golden_hour_end", "solar_noon",
];
const DECISIONS: [&str; 3] = ["approve", "deny", "approve_with_feedback"];

/// The status of an approval as SQLite sees it, shared by the get and list routes.
const STATUS: &str = "json_remove(json_object(
    'id', CAST(a.id AS TEXT), 'task', a.task, 'requester', a.requester, 'risk', a.risk,
    'created_at', strftime('%Y-%m-%dT%H:%M:%SZ', a.created_ms / 1000, 'unixepoch'),
    'status', CASE WHEN d.decision = 'approve' THEN 'approved'
                   WHEN d.decision = 'deny' THEN 'denied'
                   WHEN d.decision = 'approve_with_feedback' THEN 'approved_with_feedback'
                   WHEN a.expires_ms IS NOT NULL AND a.expires_ms <= :now THEN 'expired'
                   ELSE 'pending' END,
    'feedback', d.feedback,
    'decided_at', strftime('%Y-%m-%dT%H:%M:%SZ', d.decided_ms / 1000, 'unixepoch')),
  CASE WHEN d.feedback IS NULL THEN '$.feedback' ELSE '$.none' END,
  CASE WHEN d.decided_ms IS NULL THEN '$.decided_at' ELSE '$.none' END)";

/// POST /desk/v1/approvals: an agent asks the person. Answers 201 with the new id.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_approval_create() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(body) => approval_create(&req, body),
        Err(e) => bad(&e),
    }
}

/// The tool twin of POST /desk/v1/approvals (050): the input is an Ask.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_ask() -> i32 {
    goober_route_desk_approval_create()
}

fn approval_create(req: &Request, body: Map<String, Value>) -> i32 {
    let checked = (|| -> Result<Value, String> {
        goober::closed(&body, &["task", "description", "requester", "risk", "expires_at", "actions"], "ApprovalRequest")?;
        let task = goober::string_field(&body, "task", true, 200)?.unwrap();
        if task.trim().is_empty() {
            return Err("'task' can't be empty".into());
        }
        let description = goober::string_field(&body, "description", false, 64000)?.unwrap_or("");
        lint_markdown(description)?;
        lint_plain(task, "task")?;
        let requester = goober::string_field(&body, "requester", false, 120)?;
        let risk = goober::enum_field(&body, "risk", false, &["unknown", "low", "medium", "high"])?.unwrap_or("unknown");
        let expires = goober::instant_field(&body, "expires_at", false)?;
        let actions = match body.get("actions") {
            None => String::new(),
            Some(Value::Array(a)) if !a.is_empty() && a.len() <= 3 => {
                for v in a {
                    if !v.as_str().is_some_and(|s| DECISIONS.contains(&s)) {
                        return Err("'actions' holds approve, deny, or approve_with_feedback".into());
                    }
                }
                Value::Array(a.clone()).to_string()
            }
            Some(_) => return Err("'actions' is a list of 1 to 3 decisions".into()),
        };
        // Who asked: the agent's client, or the person themselves.
        let by = req.user()["client_id"].as_str().map(str::to_string)
            .unwrap_or_else(|| format!("user:{}", req.user_id().unwrap_or("?")));
        let requester = requester.map(str::to_string).unwrap_or_else(|| by.clone());
        Ok(json!({ "task": task, "description": description, "requester": requester, "risk": risk,
                   "actions": actions, "created": goober::now_ms(), "expires": expires, "by": by }))
    })();
    let p = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let Some(r) = sql_or_500(
        DB,
        "INSERT INTO approvals (task, description, requester, risk, actions, created_ms, expires_ms, requested_by)
           VALUES (:task, :description, :requester, :risk, :actions, :created, :expires, :by)",
        p,
    ) else { return 0 };
    respond_json(201, &json!({ "id": r.last_insert_id().to_string(), "status": "pending" }));
    0
}

/// GET /desk/v1/approvals/{approval_id}: pending, approved, denied, approved_with_feedback, or expired.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_approval_get() -> i32 {
    let req = Request::read();
    approval_get(req.param("approval_id").and_then(|s| s.parse::<i64>().ok()).unwrap_or(0))
}

/// The tool twin: the input is {id}, an int64 the host passes as a string.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_check() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(b) => approval_get(b.get("id").and_then(|v| v.as_str()).and_then(|s| s.parse().ok()).unwrap_or(0)),
        Err(e) => bad(&e),
    }
}

fn approval_get(id: i64) -> i32 {
    let q = format!("SELECT {STATUS} FROM approvals a LEFT JOIN decisions d ON d.approval_id = a.id WHERE a.id = :id");
    let Some(r) = sql_or_500(DB, &q, json!({ "id": id, "now": goober::now_ms() })) else { return 0 };
    match r.first().and_then(|v| v.as_str()) {
        Some(text) => respond_raw_json(200, text),
        None => error(404, "no such approval"),
    }
    0
}

/// GET /desk/v1/approvals: the newest 50, with their status.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_recent() -> i32 {
    goober_route_desk_approvals()
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_approvals() -> i32 {
    let q = format!(
        "SELECT json_object('approvals', json((SELECT json_group_array(json(s)) FROM (SELECT {STATUS} AS s
           FROM approvals a LEFT JOIN decisions d ON d.approval_id = a.id ORDER BY a.id DESC LIMIT 50))))"
    );
    let Some(r) = sql_or_500(DB, &q, json!({ "now": goober::now_ms() })) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{}"));
    0
}

/// GET /desk/v1/alarms: every alarm the snapshot carries.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_list_alarms() -> i32 {
    goober_route_desk_alarms()
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_alarms() -> i32 {
    let Some(r) = sql_or_500(
        DB,
        "SELECT json_object('alarms', json((SELECT json_group_array(json(spec)) FROM
           (SELECT spec FROM alarms ORDER BY updated_ms DESC, id DESC LIMIT 200))))",
        json!([]),
    ) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{}"));
    0
}

/// PUT /desk/v1/alarms/{alarm_id}: an alarm is a rule, and the phone computes the times
/// (999 § Alarms). The body is the Alarm without its id, or with the same id.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_alarm_put() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(body) => alarm_put(req.param("alarm_id").unwrap_or(""), &body),
        Err(e) => bad(&e),
    }
}

/// The tool twin: the input is {id, alarm}.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_set_alarm() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(b) => alarm_put(b.get("id").and_then(|v| v.as_str()).unwrap_or(""),
                           b.get("alarm").and_then(|v| v.as_object()).unwrap_or(&Map::new())),
        Err(e) => bad(&e),
    }
}

fn alarm_put(id: &str, body: &Map<String, Value>) -> i32 {
    let id = id.to_string();
    if !is_key(&id, 200) {
        return bad("an alarm id is 1 to 200 of [A-Za-z0-9._-]");
    }
    let spec = match check_alarm(&id, body) {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let Some(count) = sql_or_500(DB, "SELECT count(*) FROM alarms WHERE alarm_key != ?", json!([id])) else { return 0 };
    if count.first().and_then(|v| v.as_i64()).unwrap_or(0) >= 64 {
        return bad("the terminal holds at most 64 alarms; delete one first");
    }
    let Some(_) = sql_or_500(
        DB,
        "INSERT INTO alarms (alarm_key, spec, updated_ms) VALUES (?, ?, ?)
         ON CONFLICT (alarm_key) DO UPDATE SET spec = excluded.spec, updated_ms = excluded.updated_ms",
        json!([id, spec.to_string(), goober::now_ms()]),
    ) else { return 0 };
    respond_json(200, &spec);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_alarm_delete() -> i32 {
    let req = Request::read();
    alarm_delete(req.param("alarm_id").unwrap_or(""))
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_remove_alarm() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(b) => alarm_delete(b.get("id").and_then(|v| v.as_str()).unwrap_or("")),
        Err(e) => bad(&e),
    }
}

fn alarm_delete(id: &str) -> i32 {
    let Some(r) = sql_or_500(DB, "DELETE FROM alarms WHERE alarm_key = ?", json!([id])) else { return 0 };
    if r.changes() == 0 {
        error(404, "no such alarm");
    } else {
        respond_json(200, &json!({ "removed": id }));
    }
    0
}

/// PUT /desk/v1/dashboards/{dashboard_id}: a multi-page dashboard (999 § Dashboards).
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_dashboard_put() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(body) => dashboard_put(req.param("dashboard_id").unwrap_or(""), body),
        Err(e) => bad(&e),
    }
}

/// The tool twin: the input is {id, dashboard}.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_set_dashboard() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(b) => dashboard_put(b.get("id").and_then(|v| v.as_str()).unwrap_or(""),
                               b.get("dashboard").and_then(|v| v.as_object()).cloned().unwrap_or_default()),
        Err(e) => bad(&e),
    }
}

fn dashboard_put(id: &str, body: Map<String, Value>) -> i32 {
    let id = id.to_string();
    if !is_key(&id, 64) {
        return bad("a dashboard id is 1 to 64 of [A-Za-z0-9._-]");
    }
    let checked = (|| -> Result<(String, Option<String>), String> {
        goober::closed(&body, &["id", "title", "subtitle", "refresh_seconds", "settings", "pages"], "Dashboard")?;
        if let Some(v) = body.get("id") {
            if v.as_str() != Some(id.as_str()) {
                return Err("'id' must match the path".into());
            }
        }
        let title = goober::string_field(&body, "title", true, 200)?.unwrap();
        let subtitle = goober::string_field(&body, "subtitle", false, 200)?;
        goober::int32_field(&body, "refresh_seconds", 15, 86400)?;
        bounded_array(&body, "settings", 20, false)?;
        let pages = bounded_array(&body, "pages", 20, true)?;
        for p in pages {
            let widgets = p.get("widgets").and_then(|w| w.as_array()).ok_or("each page needs widgets")?;
            if widgets.len() > 50 {
                return Err("a page holds at most 50 widgets".into());
            }
            for w in widgets {
                if !w.get("type").is_some_and(|t| t.is_string()) {
                    return Err("each widget needs a type".into());
                }
                if let Some(text) = w.get("text").and_then(|t| t.as_str()) {
                    lint_markdown(text)?;
                }
            }
        }
        Ok((title.to_string(), subtitle.map(str::to_string)))
    })();
    let (title, subtitle) = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let mut spec = Value::Object(body);
    spec["id"] = json!(id);
    let Some(_) = sql_or_500(
        DB,
        "INSERT INTO dashboards (dashboard_key, title, subtitle, spec, updated_ms) VALUES (?, ?, ?, ?, ?)
         ON CONFLICT (dashboard_key) DO UPDATE SET title = excluded.title, subtitle = excluded.subtitle,
           spec = excluded.spec, updated_ms = excluded.updated_ms",
        json!([id, title, subtitle, spec.to_string(), goober::now_ms()]),
    ) else { return 0 };
    respond_json(200, &json!({ "id": id, "title": title }));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_dashboard_delete() -> i32 {
    let req = Request::read();
    dashboard_delete(req.param("dashboard_id").unwrap_or(""))
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_remove_dashboard() -> i32 {
    let req = Request::read();
    match req.json_body() {
        Ok(b) => dashboard_delete(b.get("id").and_then(|v| v.as_str()).unwrap_or("")),
        Err(e) => bad(&e),
    }
}

fn dashboard_delete(id: &str) -> i32 {
    let Some(r) = sql_or_500(DB, "DELETE FROM dashboards WHERE dashboard_key = ?", json!([id])) else { return 0 };
    if r.changes() == 0 {
        error(404, "no such dashboard");
    } else {
        respond_json(200, &json!({ "removed": id }));
    }
    0
}

/// PUT /desk/v1/settings: the backend's display name and the default location for solar alarms.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_set_settings() -> i32 {
    goober_route_desk_settings_put()
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_settings_put() -> i32 {
    let req = Request::read();
    let body = match req.json_body() {
        Ok(b) => b,
        Err(e) => return bad(&e),
    };
    let checked = (|| -> Result<Value, String> {
        goober::closed(&body, &["name", "default_location"], "Settings")?;
        let name = goober::string_field(&body, "name", true, 120)?.unwrap();
        let (lat, lon, label) = match body.get("default_location") {
            None => (Value::Null, Value::Null, Value::Null),
            Some(Value::Object(loc)) => {
                let (lat, lon, label) = check_location(loc)?;
                (json!(lat), json!(lon), json!(label))
            }
            Some(_) => return Err("'default_location' is {lat, lon, label}".into()),
        };
        Ok(json!({ "name": name, "lat": lat, "lon": lon, "label": label }))
    })();
    let p = match checked {
        Ok(v) => v,
        Err(e) => return bad(&e),
    };
    let Some(_) = sql_or_500(
        DB,
        "INSERT INTO settings (id, name, lat, lon, label) VALUES (1, :name, :lat, :lon, :label)
         ON CONFLICT (id) DO UPDATE SET name = excluded.name, lat = excluded.lat, lon = excluded.lon,
           label = excluded.label",
        p.clone(),
    ) else { return 0 };
    respond_json(200, &p);
    0
}

/// GET /desk/v1/devices: the person's installs and their time zones, for agents that plan
/// "6am in their zone". Push tokens stay out of agents' reach.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_list_devices() -> i32 {
    goober_route_desk_devices()
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_devices() -> i32 {
    let Some(r) = sql_or_500(
        DB,
        "SELECT json_object('devices', json((SELECT json_group_array(json_object(
           'device_name', device_name, 'platform', platform, 'timezone', timezone,
           'updated_at', strftime('%Y-%m-%dT%H:%M:%SZ', updated_ms / 1000, 'unixepoch')))
           FROM (SELECT device_name, platform, timezone, updated_ms FROM devices ORDER BY updated_ms DESC LIMIT 50))))",
        json!([]),
    ) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{}"));
    0
}

/// A timer (045): at 6am in the zone each device registered, put a notification on the
/// person's devices when approvals are waiting, so the morning starts with what's blocked
/// on them; with nothing waiting, the notification is removed. The host hands the fire the
/// device's row, which this box doesn't need: one alarm id serves every device.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_desk_morning() -> i32 {
    let now = goober::now_ms();
    let r = goober::sql(
        DB,
        "SELECT count(*) FROM approvals a WHERE NOT EXISTS (SELECT 1 FROM decisions d WHERE d.approval_id = a.id)
           AND (a.expires_ms IS NULL OR a.expires_ms > ?)",
        json!([now]),
    );
    if let Some(e) = r.error() {
        goober::log(&format!("morning briefing: {e}"));
        return 1;  // a failed fire: nothing is written
    }
    let waiting = r.first().and_then(|v| v.as_i64()).unwrap_or(0);
    if waiting == 0 {
        goober::sql(DB, "DELETE FROM alarms WHERE alarm_key = 'morning-briefing'", json!([]));
        return 0;
    }
    let at = now + 60_000;
    let spec = json!({
        "id": "morning-briefing",
        "kind": "notification",
        "priority": "high",
        "title": "Good morning",
        "body": if waiting == 1 { "1 approval is waiting for you.".to_string() } else { format!("{waiting} approvals are waiting for you.") },
        "trigger": { "type": "at", "at": iso(at) },
    });
    goober::sql(
        DB,
        "INSERT INTO alarms (alarm_key, spec, updated_ms) VALUES ('morning-briefing', ?, ?)
         ON CONFLICT (alarm_key) DO UPDATE SET spec = excluded.spec, updated_ms = excluded.updated_ms",
        json!([spec.to_string(), now]),
    );
    0
}

/// Unix milliseconds as an RFC 3339 instant, to the second.
fn iso(ms: i64) -> String {
    let secs = ms.div_euclid(1000);
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
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

// ---- Checks

fn bad(message: &str) -> i32 {
    error(400, message);
    0
}

fn is_key(s: &str, max: usize) -> bool {
    !s.is_empty() && s.len() <= max && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'.' || c == b'_' || c == b'-')
}

fn bounded_array<'a>(body: &'a Map<String, Value>, name: &str, max: usize, required: bool) -> Result<&'a [Value], String> {
    match body.get(name) {
        None if !required => Ok(&[]),
        None => Err(format!("'{name}' is required")),
        Some(Value::Array(a)) if a.len() <= max => Ok(a.as_slice()),
        Some(_) => Err(format!("'{name}' is a list of at most {max}")),
    }
}

fn check_location(loc: &Map<String, Value>) -> Result<(f64, f64, String), String> {
    goober::closed(loc, &["lat", "lon", "label"], "Location")?;
    let lat = loc.get("lat").and_then(|v| v.as_f64()).filter(|v| (-90.0..=90.0).contains(v));
    let lon = loc.get("lon").and_then(|v| v.as_f64()).filter(|v| (-180.0..=180.0).contains(v));
    let (Some(lat), Some(lon)) = (lat, lon) else { return Err("a location needs lat (-90..90) and lon (-180..180)".into()) };
    let label = goober::string_field(loc, "label", false, 120)?.unwrap_or("").to_string();
    Ok((lat, lon, label))
}

fn check_alarm(id: &str, body: &Map<String, Value>) -> Result<Value, String> {
    goober::closed(body, &["id", "kind", "title", "body", "priority", "snooze_minutes", "enabled", "until", "trigger"], "Alarm")?;
    if let Some(v) = body.get("id") {
        if v.as_str() != Some(id) {
            return Err("'id' must match the path".into());
        }
    }
    goober::enum_field(body, "kind", true, &["alarm", "notification"])?;
    let title = goober::string_field(body, "title", true, 200)?.unwrap();
    lint_plain(title, "title")?;
    if let Some(text) = goober::string_field(body, "body", false, 2000)? {
        lint_plain(text, "body")?;
    }
    goober::enum_field(body, "priority", false, &["low", "default", "high"])?;
    goober::int32_field(body, "snooze_minutes", 1, 120)?;
    if body.get("enabled").is_some_and(|v| !v.is_boolean()) {
        return Err("'enabled' must be true or false".into());
    }
    goober::instant_field(body, "until", false)?;
    let Some(Value::Object(t)) = body.get("trigger") else { return Err("'trigger' is required".into()) };
    match goober::enum_field(t, "type", true, &["at", "daily", "solar"])?.unwrap() {
        "at" => {
            goober::closed(t, &["type", "at"], "Trigger(at)")?;
            goober::instant_field(t, "at", true)?;
        }
        kind => {
            let solar = kind == "solar";
            let allowed: &[&str] = if solar {
                &["type", "event", "offset_minutes", "location", "days", "date", "tz"]
            } else {
                &["type", "time", "days", "date", "tz"]
            };
            goober::closed(t, allowed, if solar { "Trigger(solar)" } else { "Trigger(daily)" })?;
            if solar {
                goober::enum_field(t, "event", true, &SOLAR)?;
                goober::int32_field(t, "offset_minutes", -1440, 1440)?;
                if let Some(loc) = t.get("location") {
                    check_location(loc.as_object().ok_or("'location' is {lat, lon, label}")?)?;
                }
            } else {
                let time = goober::string_field(t, "time", true, 5)?.unwrap();
                let b = time.as_bytes();
                let ok = b.len() == 5 && b[2] == b':' && time[..2].parse::<u8>().is_ok_and(|h| h < 24)
                    && time[3..].parse::<u8>().is_ok_and(|m| m < 60);
                if !ok {
                    return Err("'time' is a wall-clock HH:mm".into());
                }
            }
            if let Some(days) = t.get("days") {
                let a = days.as_array().filter(|a| a.len() <= 7).ok_or("'days' is a list of MON through SUN")?;
                if !a.iter().all(|d| d.as_str().is_some_and(|s| DAYS.contains(&s))) {
                    return Err("'days' is a list of MON through SUN".into());
                }
            }
            if let Some(date) = goober::string_field(t, "date", false, 10)? {
                let b = date.as_bytes();
                if b.len() != 10 || b[4] != b'-' || b[7] != b'-' || goober::parse_instant(&format!("{date}T00:00:00Z")).is_none() {
                    return Err("'date' is YYYY-MM-DD".into());
                }
            }
            goober::string_field(t, "tz", false, 64)?;
        }
    }
    let mut spec = Value::Object(body.clone());
    spec["id"] = json!(id);
    Ok(spec)
}

/// Plain text fields shown on the lock screen: no hidden or reordering characters.
fn lint_plain(text: &str, field: &str) -> Result<(), String> {
    for c in text.chars() {
        if hidden(c) || (c.is_control() && c != '\n') {
            return Err(format!("'{field}' holds a hidden or control character (U+{:04X})", c as u32));
        }
    }
    Ok(())
}

/// The quarantine rules of 999 § The markdown dialect that are cheap to check here, so an
/// agent's bad description is refused early. The terminal checks everything regardless.
fn lint_markdown(text: &str) -> Result<(), String> {
    if text.chars().count() > 64000 || text.lines().count() > 2000 {
        return Err("the description is longer than 64,000 characters or 2,000 lines".into());
    }
    for c in text.chars() {
        if hidden(c) || (c.is_control() && c != '\n' && c != '\t' && c != '\r') {
            return Err(format!(
                "the description holds a hidden, control, or reordering character (U+{:04X}); the terminal would quarantine it",
                c as u32
            ));
        }
    }
    let lower = text.to_ascii_lowercase();
    for scheme in ["javascript:", "data:", "file:", "intent:", "vbscript:"] {
        if lower.contains(&format!("]({scheme}")) || lower.contains(&format!("](<{scheme}")) {
            return Err(format!("the description links to {scheme}, which the terminal would quarantine"));
        }
    }
    Ok(())
}

/// Bidi controls, zero-width characters, and Unicode tags (Trojan Source and friends).
fn hidden(c: char) -> bool {
    matches!(c as u32,
        0x200B..=0x200F | 0x202A..=0x202E | 0x2060..=0x2064 | 0x2066..=0x2069 | 0xFEFF | 0x061C | 0xE0000..=0xE007F)
}
