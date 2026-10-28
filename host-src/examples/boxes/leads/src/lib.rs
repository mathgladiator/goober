//! leads: a landing page's form, the emails and survey answers it collects, and the tools an
//! owner's agent uses to count and export them (READINESS.md, the ads experiment).
//!
//! The form posts to /leads/{campaign}/signup with `email`, `consent`, optional `name`,
//! survey answers as `q_<question>` fields, and the ad's `utm_*` fields; `website` is a
//! honeypot people never fill in, and `next` is where to send them afterwards (a path on
//! this site, default /thanks). A second signup with the same email updates the answers.
//! When the manifest declares a `mail` service (see README.md), new signups get a
//! confirmation link through the outbox, so the mail goes only if the signup is stored.

#[path = "../../shared/goober.rs"]
mod goober;

use goober::{Request, error, respond, respond_json, respond_raw_json, sql, sql_or_500};
use serde_json::{Map, Value, json};

const DB: &str = "leads";
const UTM: [&str; 4] = ["utm_source", "utm_medium", "utm_campaign", "utm_content"];

fn valid_email(e: &str) -> bool {
    let Some((local, domain)) = e.rsplit_once('@') else { return false };
    !local.is_empty()
        && local.len() <= 64
        && domain.contains('.')
        && !domain.starts_with('.')
        && !domain.ends_with('.')
        && e.len() <= 254
        && e.bytes().all(|b| b > 0x20 && b < 0x7f && b != b'<' && b != b'>' && b != b'"' && b != b',')
}

fn valid_slug(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

/// A form gets a small page it can go back from; an API caller gets JSON.
fn refuse(is_form: bool, message: &str) -> i32 {
    if is_form {
        let page = format!(
            "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width\">\
             <title>Almost</title><p style=\"font:18px system-ui;margin:2em\">{}<br><br>\
             <a href=\"javascript:history.back()\">Go back</a></p>",
            html_escape(message)
        );
        respond(400, "text/html; charset=utf-8", page.as_bytes());
    } else {
        error(400, message);
    }
    0
}

fn finish(is_form: bool, next: &str) -> i32 {
    if is_form {
        goober::redirect(next);
    } else {
        respond_json(201, &json!({ "ok": true }));
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_lead_signup() -> i32 {
    let req = Request::read();
    let is_form = goober::form(&req).is_some();
    let fields: Map<String, Value> = match goober::form(&req) {
        Some(f) => f.clone(),
        None => match req.json_body() {
            Ok(b) => b,
            Err(e) => return refuse(false, &e),
        },
    };
    let field = |k: &str| -> Option<String> {
        fields.get(k).and_then(|v| v.as_str()).map(|v| v.trim().to_string()).filter(|v| !v.is_empty())
    };
    let next = field("next")
        .filter(|n| n.starts_with('/') && !n.starts_with("//") && n.len() <= 200 && n.bytes().all(|b| b > 0x20 && b < 0x7f && b != b'\\'))
        .unwrap_or_else(|| "/thanks".to_string());
    // A bot that fills in every field fills in the one people never see: thank it and move on.
    if field("website").is_some() {
        return finish(is_form, &next);
    }
    let campaign = req.param("campaign").unwrap_or("").to_string();
    if !valid_slug(&campaign) {
        return refuse(is_form, "That campaign isn't one this site runs.");
    }
    let email = match field("email") {
        Some(e) if valid_email(&e) => e.to_lowercase(),
        _ => return refuse(is_form, "Please enter a valid email address."),
    };
    if field("consent").is_none() {
        return refuse(is_form, "Please check the box to say we may email you.");
    }
    let name = field("name").map(|n| n.chars().take(200).collect::<String>());
    let mut answers = Map::new();
    for (k, v) in &fields {
        if let (Some(q), Some(s)) = (k.strip_prefix("q_"), v.as_str()) {
            if answers.len() < 50 && valid_slug(q) && !s.trim().is_empty() {
                answers.insert(q.to_string(), Value::String(s.trim().chars().take(2000).collect()));
            }
        }
    }
    let mut utm = Map::new();
    for k in UTM {
        let v = field(k).or_else(|| req.query(k).map(|q| q.trim().to_string())).filter(|v| !v.is_empty());
        utm.insert(k.to_string(), v.map(|s| Value::String(s.chars().take(200).collect())).unwrap_or(Value::Null));
    }
    let now = goober::now_ms();
    let token = goober::random_hex(16);
    // One row per email per campaign. Coming back from the same place (the same client_key)
    // updates the answers, never the first source; someone else typing that email changes
    // nothing.
    let Some(r) = sql_or_500(
        DB,
        "INSERT INTO leads (campaign, email, name, answers, utm_source, utm_medium, utm_campaign, utm_content, \
           client_key, token, created_ms, updated_ms) \
         VALUES (:campaign, :email, :name, :answers, :utm_source, :utm_medium, :utm_campaign, :utm_content, \
           :client_key, :token, :now, :now) \
         ON CONFLICT (campaign, email) DO UPDATE SET answers = json_patch(leads.answers, excluded.answers), \
           name = coalesce(excluded.name, leads.name), updated_ms = excluded.updated_ms \
           WHERE leads.client_key IS excluded.client_key \
         RETURNING token = :token AS fresh",
        json!({ "campaign": campaign, "email": email, "name": name, "answers": Value::Object(answers).to_string(),
                "utm_source": utm["utm_source"], "utm_medium": utm["utm_medium"], "utm_campaign": utm["utm_campaign"],
                "utm_content": utm["utm_content"], "client_key": req.doc["client_key"], "token": token, "now": now }),
    ) else {
        return 0;
    };
    let fresh = r.first().and_then(|v| v.as_i64()) == Some(1);
    // At most one confirmation a day to an address, whatever campaigns it is entered in, so
    // the form can't be used to flood someone's inbox.
    let recent = sql(DB, "SELECT count(*) FROM leads WHERE email = :email AND created_ms > :since",
                     json!({ "email": email, "since": now - 86_400_000 }))
        .first().and_then(|v| v.as_i64()).unwrap_or(0);
    if fresh && recent <= 1 {
        let domain = req.doc["domain"].as_str().unwrap_or("");
        let link = format!("https://{domain}/leads/confirm/{token}");
        let sent = goober::call(
            "mail",
            "send",
            json!({ "to": email, "subject": "Please confirm your email",
                    "text": format!("Thanks for signing up. Confirm your email by opening this link:\n\n{link}\n\n\
                                     If this wasn't you, ignore this message and nothing more will come.\n") }),
        );
        // Without a mail service declared, signups simply stay unconfirmed.
        if let Some(e) = sent["error"].as_str() {
            if !e.starts_with("no external service named mail") {
                goober::log(&format!("confirmation mail not queued: {e}"));
            }
        }
    }
    finish(is_form, &next)
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_lead_confirm() -> i32 {
    let req = Request::read();
    let token = req.param("token").unwrap_or("");
    if token.len() != 32 || !token.bytes().all(|b| b.is_ascii_hexdigit()) {
        goober::redirect("/confirmed?ok=0");
        return 0;
    }
    let r = sql(DB, "UPDATE leads SET confirmed_ms = :now WHERE token = :token AND confirmed_ms IS NULL",
                json!({ "now": goober::now_ms(), "token": token }));
    if let Some(e) = r.error() {
        goober::log(&format!("confirm failed: {e}"));
        error(500, "storage failed");
        return 0;
    }
    goober::redirect("/confirmed");
    0
}

/// The JSON object for one lead, written by SQLite.
const LEAD: &str = "json_object('email', email, 'name', name, 'campaign', campaign, 'answers', json(answers), \
    'utm_source', utm_source, 'utm_medium', utm_medium, 'utm_campaign', utm_campaign, 'utm_content', utm_content, \
    'confirmed', confirmed_ms IS NOT NULL, \
    'created_at', strftime('%Y-%m-%dT%H:%M:%SZ', created_ms / 1000, 'unixepoch'), \
    'updated_at', strftime('%Y-%m-%dT%H:%M:%SZ', updated_ms / 1000, 'unixepoch'))";

fn input(req: &Request) -> Map<String, Value> {
    req.json_body().unwrap_or_default()
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_leads_list() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let sql_text = format!(
        "SELECT json_object('leads', coalesce(json_group_array(json(l)), json('[]'))) FROM (\
           SELECT {LEAD} AS l FROM leads \
           WHERE (:campaign IS NULL OR campaign = :campaign) \
             AND (:q IS NULL OR instr(email, :q) > 0 OR instr(coalesce(name, ''), :q) > 0 OR instr(answers, :q) > 0) \
           ORDER BY created_ms DESC, id DESC LIMIT :limit OFFSET :offset)"
    );
    let Some(r) = sql_or_500(DB, &sql_text, json!({ "campaign": i.get("campaign"), "q": i.get("query"),
        "limit": i.get("limit").and_then(|v| v.as_i64()).unwrap_or(100),
        "offset": i.get("offset").and_then(|v| v.as_i64()).unwrap_or(0) })) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{\"leads\":[]}"));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_leads_stats() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let p = json!({ "campaign": i.get("campaign"), "since": goober::now_ms() - 30 * 86_400_000 });
    let one = |q: &str| -> Option<Value> {
        let r = sql_or_500(DB, q, p.clone())?;
        Some(r.first().and_then(|v| v.as_str()).and_then(|s| serde_json::from_str(s).ok()).unwrap_or(json!([])))
    };
    let filter = "(:campaign IS NULL OR campaign = :campaign)";
    let Some(campaigns) = one(&format!(
        "SELECT json_group_array(json_object('campaign', campaign, 'leads', n, 'confirmed', c, \
           'first', strftime('%Y-%m-%dT%H:%M:%SZ', f / 1000, 'unixepoch'), \
           'last', strftime('%Y-%m-%dT%H:%M:%SZ', l / 1000, 'unixepoch'))) FROM (\
           SELECT campaign, count(*) AS n, count(confirmed_ms) AS c, min(created_ms) AS f, max(created_ms) AS l \
           FROM leads WHERE {filter} GROUP BY campaign ORDER BY n DESC)")) else { return 0 };
    let Some(sources) = one(&format!(
        "SELECT json_group_array(json_object('utm_source', s, 'utm_content', c, 'leads', n, 'confirmed', k)) FROM (\
           SELECT coalesce(utm_source, '(none)') AS s, coalesce(utm_content, '(none)') AS c, count(*) AS n, \
             count(confirmed_ms) AS k FROM leads WHERE {filter} GROUP BY s, c ORDER BY n DESC LIMIT 200)")) else { return 0 };
    let Some(days) = one(&format!(
        "SELECT json_group_array(json_object('day', d, 'leads', n)) FROM (\
           SELECT strftime('%Y-%m-%d', created_ms / 1000, 'unixepoch') AS d, count(*) AS n FROM leads \
           WHERE {filter} AND created_ms >= :since GROUP BY d ORDER BY d)")) else { return 0 };
    respond_json(200, &json!({ "campaigns": campaigns, "sources": sources, "days": days }));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_survey_summary() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let question = i.get("question").and_then(|v| v.as_str()).unwrap_or("");
    if !valid_slug(question) {
        error(400, "question is a survey field's name, like cut");
        return 0;
    }
    let Some(r) = sql_or_500(
        DB,
        "SELECT json_object('question', :question, 'answers', coalesce(json_group_array(json_object('answer', a, 'count', n)), json('[]'))) \
         FROM (SELECT json_extract(answers, '$.' || :question) AS a, count(*) AS n FROM leads \
               WHERE campaign = :campaign AND a IS NOT NULL GROUP BY a ORDER BY n DESC LIMIT 200)",
        json!({ "campaign": i.get("campaign"), "question": question }),
    ) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{}"));
    0
}

fn csv_cell(v: &Value) -> String {
    let s = match v {
        Value::Null => String::new(),
        Value::String(s) => s.clone(),
        other => other.to_string(),
    };
    // Quoted when needed, and never a formula when a spreadsheet opens it.
    let s = if s.starts_with(['=', '+', '-', '@', '\t', '\r']) { format!("'{s}") } else { s };
    if s.contains([',', '"', '\n', '\r']) { format!("\"{}\"", s.replace('"', "\"\"")) } else { s }
}

fn export_csv(campaign: &str) -> Option<String> {
    let r = sql_or_500(
        DB,
        &format!("SELECT {LEAD} FROM leads WHERE campaign = :campaign ORDER BY created_ms LIMIT 1000"),
        json!({ "campaign": campaign }),
    )?;
    let leads: Vec<Value> = r.rows().iter().filter_map(|row| row[0].as_str()).filter_map(|s| serde_json::from_str(s).ok()).collect();
    let mut questions: Vec<String> = Vec::new();
    for l in &leads {
        if let Some(a) = l["answers"].as_object() {
            for k in a.keys() {
                if !questions.contains(k) {
                    questions.push(k.clone());
                }
            }
        }
    }
    let fixed = ["email", "name", "confirmed", "created_at", "utm_source", "utm_medium", "utm_campaign", "utm_content"];
    let mut out = fixed.join(",");
    for q in &questions {
        out.push(',');
        out.push_str(&csv_cell(&Value::String(format!("q_{q}"))));
    }
    out.push('\n');
    for l in &leads {
        let mut cells: Vec<String> = fixed.iter().map(|f| csv_cell(&l[*f])).collect();
        for q in &questions {
            cells.push(csv_cell(&l["answers"][q.as_str()]));
        }
        out.push_str(&cells.join(","));
        out.push('\n');
    }
    Some(out)
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_leads_export() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let campaign = i.get("campaign").and_then(|v| v.as_str()).unwrap_or("").to_string();
    if let Some(csv) = export_csv(&campaign) {
        respond_json(200, &json!({ "campaign": campaign, "csv": csv }));
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_lead_export_csv() -> i32 {
    let req = Request::read();
    let campaign = req.param("campaign").unwrap_or("").to_string();
    if !valid_slug(&campaign) {
        error(404, "no such campaign");
        return 0;
    }
    if let Some(csv) = export_csv(&campaign) {
        goober::header("content-disposition", &format!("attachment; filename=\"{campaign}.csv\""));
        goober::header("cache-control", "no-store");
        respond(200, "text/csv; charset=utf-8", csv.as_bytes());
    }
    0
}
