//! ranches: a directory of local ranches, farms, and butchers, a log of every outreach, and an
//! index of what they sell, kept current by reading their websites through goober's polite
//! robot (design/human/800.Crawl.md, design/machine/017.Crawl.md).
//!
//! Its database lives in the fixed tenant `meat`, so every domain this box serves shares one
//! index. A timer reads one watched website every five minutes (each site about weekly):
//! schema.org Product and Offer data, a Shopify store's /products.json, or the ranch's own
//! extract spec. Agents work it through MCP tools; anyone may read /meat/offerings and
//! /meat/ranches.

#[path = "../../shared/goober.rs"]
mod goober;

use goober::{Request, error, respond_json, respond_raw_json, sql, sql_or_500};
use serde_json::{Map, Value, json};

const DB: &str = "meat";
const WEEK_MS: i64 = 7 * 86_400_000;
const STATUSES: [&str; 6] = ["prospect", "contacted", "replied", "interested", "partner", "declined"];

fn input(req: &Request) -> Map<String, Value> {
    req.json_body().unwrap_or_default()
}

fn s<'a>(m: &'a Map<String, Value>, k: &str) -> Option<&'a str> {
    m.get(k).and_then(|v| v.as_str()).map(str::trim).filter(|v| !v.is_empty())
}

fn slugify(name: &str) -> String {
    let mut out = String::new();
    for c in name.to_lowercase().chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c);
        } else if !out.ends_with('-') && !out.is_empty() {
            out.push('-');
        }
    }
    out.trim_end_matches('-').chars().take(80).collect()
}

fn valid_website(w: &str) -> bool {
    (w.starts_with("https://") || w.starts_with("http://")) && w.len() <= 500 && !w.contains(char::is_whitespace)
}

/// What kind of meat a name sounds like.
fn category(name: &str) -> Option<&'static str> {
    let n = name.to_lowercase();
    let has = |words: &[&str]| words.iter().any(|w| n.contains(w));
    if has(&["egg"]) {
        Some("eggs")
    } else if has(&["bison", "buffalo"]) {
        Some("bison")
    } else if has(&["lamb", "mutton", "sheep"]) {
        Some("lamb")
    } else if has(&["goat", "chevon"]) {
        Some("goat")
    } else if has(&["turkey"]) {
        Some("turkey")
    } else if has(&["chicken", "poultry", "broiler", "hen"]) {
        Some("chicken")
    } else if has(&["pork", "bacon", "ham", "sausage", "hog", "pig", "chop", "belly", "lard"]) {
        Some("pork")
    } else if has(&["beef", "steak", "ribeye", "rib eye", "brisket", "sirloin", "chuck", "ground", "roast", "tenderloin",
                    "strip", "wagyu", "angus", "cow", "burger", "oxtail", "short rib", "tri-tip", "tri tip"]) {
        Some("beef")
    } else {
        None
    }
}

/// "$1,800.00" -> 180000 cents; "12.5" -> 1250.
fn price_cents(text: &str) -> Option<i64> {
    let start = text.find(|c: char| c.is_ascii_digit())?;
    let mut whole = String::new();
    let mut frac = String::new();
    let mut in_frac = false;
    for c in text[start..].chars() {
        match c {
            '0'..='9' if in_frac => {
                if frac.len() < 2 {
                    frac.push(c)
                }
            }
            '0'..='9' => whole.push(c),
            ',' if !in_frac => {}
            '.' if !in_frac => in_frac = true,
            _ => break,
        }
    }
    while frac.len() < 2 {
        frac.push('0');
    }
    let w: i64 = whole.parse().ok()?;
    let f: i64 = frac.parse().ok()?;
    w.checked_mul(100)?.checked_add(f)
}

fn unit_of(name: &str, price: &str) -> Option<&'static str> {
    let t = format!("{} {}", name, price).to_lowercase();
    if t.contains("/lb") || t.contains("per lb") || t.contains("/ lb") || t.contains("per pound") {
        Some("lb")
    } else if t.contains("quarter") || t.contains("half") || t.contains("whole") || t.contains("share") {
        Some("share")
    } else if t.contains("box") || t.contains("bundle") {
        Some("box")
    } else if t.contains("dozen") {
        Some("dozen")
    } else {
        None
    }
}

#[derive(Default)]
struct Offer {
    name: String,
    price_text: Option<String>,
    currency: Option<String>,
    unit: Option<String>,
    in_stock: Option<bool>,
    url: Option<String>,
    category: Option<String>,
    source: &'static str,
}

fn text_of(v: &Value) -> Option<String> {
    match v {
        Value::String(s) if !s.trim().is_empty() => Some(s.trim().to_string()),
        Value::Number(n) => Some(n.to_string()),
        _ => None,
    }
}

fn types_of(item: &Value) -> Vec<String> {
    match &item["@type"] {
        Value::String(t) => vec![t.clone()],
        Value::Array(a) => a.iter().filter_map(|t| t.as_str().map(str::to_string)).collect(),
        _ => vec![],
    }
}

/// schema.org data: Products (with their Offers), ItemLists and OfferCatalogs of them, and
/// Offers naming what they offer. LocalBusiness fills in contact details.
fn walk_jsonld(item: &Value, out: &mut Vec<Offer>, business: &mut Map<String, Value>, depth: usize) {
    if depth > 6 || out.len() >= 500 {
        return;
    }
    if let Value::Array(a) = item {
        for i in a {
            walk_jsonld(i, out, business, depth + 1);
        }
        return;
    }
    let types = types_of(item);
    let is = |t: &str| types.iter().any(|x| x == t);
    if is("Product") || is("IndividualProduct") || is("ProductGroup") {
        let Some(name) = text_of(&item["name"]) else { return };
        let mut o = Offer { name, url: text_of(&item["url"]), source: "jsonld", ..Default::default() };
        let offers = match &item["offers"] {
            Value::Array(a) => a.first().cloned().unwrap_or(Value::Null),
            v => v.clone(),
        };
        o.price_text = text_of(&offers["price"]).or_else(|| text_of(&offers["lowPrice"]));
        o.currency = text_of(&offers["priceCurrency"]);
        if let Some(av) = offers["availability"].as_str() {
            o.in_stock = Some(av.contains("InStock") || av.contains("LimitedAvailability") || av.contains("PreOrder"));
        }
        if o.url.is_none() {
            o.url = text_of(&offers["url"]);
        }
        out.push(o);
        return;
    }
    if is("Offer") {
        if let Some(name) = text_of(&item["itemOffered"]["name"]).or_else(|| text_of(&item["name"])) {
            out.push(Offer {
                name,
                price_text: text_of(&item["price"]),
                currency: text_of(&item["priceCurrency"]),
                in_stock: item["availability"].as_str().map(|a| a.contains("InStock")),
                url: text_of(&item["url"]),
                source: "jsonld",
                ..Default::default()
            });
        }
        return;
    }
    if is("ItemList") || is("OfferCatalog") {
        if let Value::Array(a) = &item["itemListElement"] {
            for e in a {
                let inner = if e["item"].is_object() { &e["item"] } else { e };
                walk_jsonld(inner, out, business, depth + 1);
            }
        }
    }
    if types.iter().any(|t| t.contains("Business") || t == "Store" || t == "Farm" || t == "Organization" || t == "ButcherShop") {
        if let Some(p) = text_of(&item["telephone"]) {
            business.entry("phone").or_insert(Value::String(p));
        }
        if let Some(e) = text_of(&item["email"]) {
            business.entry("email").or_insert(Value::String(e.trim_start_matches("mailto:").to_string()));
        }
        let a = &item["address"];
        if let Some(city) = text_of(&a["addressLocality"]) {
            business.entry("city").or_insert(Value::String(city));
        }
        if let Some(region) = text_of(&a["addressRegion"]) {
            business.entry("region").or_insert(Value::String(region));
        }
        if let Some(street) = text_of(&a["streetAddress"]) {
            business.entry("address").or_insert(Value::String(street));
        }
        if let Some(catalog) = item.get("hasOfferCatalog") {
            walk_jsonld(catalog, out, business, depth + 1);
        }
    }
}

/// A Shopify store's /products.json: one offering per product, or per variant when they differ.
fn shopify_offers(v: &Value, origin: &str) -> Vec<Offer> {
    let mut out = Vec::new();
    for p in v["products"].as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        let Some(title) = text_of(&p["title"]) else { continue };
        let url = p["handle"].as_str().map(|h| format!("{origin}/products/{h}"));
        let variants = p["variants"].as_array().map(|a| a.as_slice()).unwrap_or(&[]);
        let distinct = variants.len() > 1;
        for var in variants.iter().take(if distinct { 5 } else { 1 }) {
            let vt = var["title"].as_str().unwrap_or("");
            let name = if distinct && vt != "Default Title" { format!("{title} — {vt}") } else { title.clone() };
            out.push(Offer {
                name,
                price_text: text_of(&var["price"]),
                in_stock: var["available"].as_bool(),
                url: url.clone(),
                category: category(&format!("{} {}", title, p["product_type"].as_str().unwrap_or(""))).map(str::to_string),
                source: "shopify",
                ..Default::default()
            });
        }
        if out.len() >= 500 {
            break;
        }
    }
    out
}

fn origin_of(url: &str) -> String {
    let after = url.find("://").map(|i| i + 3).unwrap_or(0);
    match url[after..].find('/') {
        Some(slash) => url[..after + slash].to_string(),
        None => url.to_string(),
    }
}

/// Upserts offerings for a ranch; answers how many.
fn store_offers(ranch_id: i64, offers: &[Offer], now: i64) -> Result<usize, String> {
    let mut n = 0;
    for o in offers {
        let name: String = o.name.chars().take(300).collect();
        let key = format!("{ranch_id}|{}|{}", name.to_lowercase(), o.url.as_deref().unwrap_or("")).chars().take(400).collect::<String>();
        let price_text = o.price_text.as_deref().map(|p| p.chars().take(100).collect::<String>());
        let cents = price_text.as_deref().and_then(price_cents);
        let unit = o.unit.clone().or_else(|| unit_of(&name, price_text.as_deref().unwrap_or("")).map(str::to_string));
        let cat = o.category.clone().or_else(|| category(&name).map(str::to_string));
        let url = o.url.as_deref().filter(|u| u.starts_with("http") && u.len() <= 1000);
        let r = sql(
            DB,
            "INSERT INTO offerings (ranch_id, offer_key, name, category, price_cents, price_text, currency, unit, in_stock, url, source, seen_ms) \
             VALUES (:r, :k, :n, :c, :pc, :pt, :cur, :u, :s, :url, :src, :now) \
             ON CONFLICT (offer_key) DO UPDATE SET name = excluded.name, category = excluded.category, \
               price_cents = excluded.price_cents, price_text = excluded.price_text, currency = excluded.currency, \
               unit = excluded.unit, in_stock = excluded.in_stock, url = excluded.url, source = excluded.source, \
               seen_ms = excluded.seen_ms",
            json!({ "r": ranch_id, "k": key, "n": name, "c": cat, "pc": cents, "pt": price_text,
                    "cur": o.currency.as_deref().map(|c| c.chars().take(8).collect::<String>()), "u": unit,
                    "s": o.in_stock.map(|b| b as i64), "url": url, "src": o.source, "now": now }),
        );
        if let Some(e) = r.error() {
            return Err(e.to_string());
        }
        n += 1;
    }
    Ok(n)
}

/// Reads one ranch's website and updates its offerings. Answers a summary.
fn refresh(ranch: &Value) -> Value {
    let id = ranch["id"].as_i64().unwrap_or(0);
    let site = ranch["website"].as_str().unwrap_or("").to_string();
    let now = goober::now_ms();
    if !valid_website(&site) {
        return json!({ "error": "no website to read" });
    }
    let spec: Option<Value> = ranch["extract"].as_str().and_then(|s| serde_json::from_str(s).ok());
    let mut req = json!({ "url": site, "views": ["jsonld", "meta"], "max_age_ms": 3_600_000 });
    if let Some(s) = &spec {
        req["extract"] = s.clone();
    }
    let page = goober::call("farms", "fetch", req);
    if let Some(e) = page["error"].as_str() {
        if page["retry_after_ms"].is_number() {
            return json!({ "later": e });  // the robot's pacing; the timer comes back
        }
        let _ = sql(DB, "UPDATE ranches SET checked_ms = :now, check_error = :e WHERE id = :id",
                    json!({ "now": now, "e": e.chars().take(500).collect::<String>(), "id": id }));
        return json!({ "error": e });
    }
    let mut offers = Vec::new();
    let mut business = Map::new();
    walk_jsonld(&page["jsonld"], &mut offers, &mut business, 0);
    if let Some(recs) = page["extract"].as_array() {
        for r in recs {
            if let Some(name) = text_of(&r["name"]) {
                offers.push(Offer {
                    name,
                    price_text: text_of(&r["price"]),
                    unit: text_of(&r["unit"]),
                    url: text_of(&r["url"]),
                    source: "extract",
                    ..Default::default()
                });
            }
        }
    }
    let platform = page["meta"]["platform"].as_str().map(str::to_string);
    let final_url = page["url"].as_str().unwrap_or(&site).to_string();
    if platform.as_deref() == Some("shopify") {
        let origin = origin_of(&final_url);
        let catalog = goober::call("farms", "fetch",
                                   json!({ "url": format!("{origin}/products.json?limit=250"), "views": ["json"], "max_age_ms": 3_600_000 }));
        if catalog["error"].is_null() {
            offers.extend(shopify_offers(&catalog["json"], &origin));
        }
    }
    let stored = match store_offers(id, &offers, now) {
        Ok(n) => n,
        Err(e) => return json!({ "error": format!("storing offerings: {e}") }),
    };
    let r = sql(
        DB,
        "UPDATE ranches SET checked_ms = :now, check_error = :err, platform = coalesce(:platform, platform), \
           phone = coalesce(phone, :phone), email = coalesce(email, :email), city = coalesce(city, :city), \
           region = coalesce(region, :region), address = coalesce(address, :address) WHERE id = :id",
        json!({ "now": now, "id": id, "platform": platform,
                "err": if offers.is_empty() { Some("no product data found: try an extract spec") } else { None },
                "phone": business.get("phone"), "email": business.get("email"), "city": business.get("city"),
                "region": business.get("region"), "address": business.get("address") }),
    );
    if let Some(e) = r.error() {
        goober::log(&format!("updating ranch {id}: {e}"));
    }
    json!({ "url": final_url, "platform": platform, "offerings": stored, "from_cache": page["from_cache"] })
}

const RANCH: &str = "json_object('slug', slug, 'name', name, 'kind', kind, 'website', website, 'email', email, \
    'phone', phone, 'address', address, 'city', city, 'region', region, 'status', status, 'platform', platform, \
    'watch', watch = 1, 'check_error', check_error, \
    'checked_at', strftime('%Y-%m-%dT%H:%M:%SZ', checked_ms / 1000, 'unixepoch'), \
    'last_contact_at', strftime('%Y-%m-%dT%H:%M:%SZ', last_contact_ms / 1000, 'unixepoch'), \
    'offerings', (SELECT count(*) FROM offerings o WHERE o.ranch_id = ranches.id))";

const OFFERING: &str = "json_object('name', o.name, 'category', o.category, 'price', o.price_text, \
    'price_cents', o.price_cents, 'currency', o.currency, 'unit', o.unit, \
    'in_stock', CASE o.in_stock WHEN 1 THEN json('true') WHEN 0 THEN json('false') ELSE NULL END, \
    'url', o.url, 'source', o.source, 'seen_at', strftime('%Y-%m-%dT%H:%M:%SZ', o.seen_ms / 1000, 'unixepoch'), \
    'ranch', r.name, 'ranch_slug', r.slug, 'city', r.city, 'region', r.region, 'website', r.website)";

fn ranch_by_slug(slug: &str) -> Option<Value> {
    let r = sql_or_500(DB, "SELECT id, website, extract, name FROM ranches WHERE slug = :slug", json!({ "slug": slug }))?;
    let row = r.rows().first()?;
    Some(json!({ "id": row[0], "website": row[1], "extract": row[2], "name": row[3] }))
}

fn not_found(slug: &str) -> i32 {
    error(404, &format!("no ranch with slug {slug}; ranch_list shows them"));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranch_add() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let name = s(&i, "name").unwrap_or("").to_string();
    let slug = s(&i, "slug").map(slugify).unwrap_or_else(|| slugify(&name));
    if slug.is_empty() {
        error(400, "the name needs letters or digits to make a slug from");
        return 0;
    }
    if let Some(w) = s(&i, "website") {
        if !valid_website(w) {
            error(400, "website must be an http(s) URL");
            return 0;
        }
    }
    if let Some(x) = s(&i, "extract") {
        if serde_json::from_str::<Value>(x).map(|v| !v.is_object()).unwrap_or(true) {
            error(400, "extract must be a JSON object like {\"items\": \".product\", \"fields\": {...}}");
            return 0;
        }
    }
    let now = goober::now_ms();
    let r = sql(
        DB,
        "INSERT INTO ranches (slug, name, kind, website, email, phone, address, city, region, notes, status, extract, created_ms, updated_ms) \
         VALUES (:slug, :name, :kind, :website, :email, :phone, :address, :city, :region, :notes, :status, :extract, :now, :now) \
         ON CONFLICT (slug) DO NOTHING",
        json!({ "slug": slug, "name": name, "kind": s(&i, "kind").unwrap_or("ranch"), "website": s(&i, "website"),
                "email": s(&i, "email"), "phone": s(&i, "phone"), "address": s(&i, "address"), "city": s(&i, "city"),
                "region": s(&i, "region"), "notes": s(&i, "notes").unwrap_or(""), "status": s(&i, "status").unwrap_or("prospect"),
                "extract": s(&i, "extract"), "now": now }),
    );
    if let Some(e) = r.error() {
        goober::log(&format!("ranch_add: {e}"));
        error(500, "storage failed");
    } else if r.changes() == 0 {
        error(409, &format!("{slug} is already in the directory; use ranch_update"));
    } else {
        respond_json(201, &json!({ "slug": slug, "name": name }));
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranch_update() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let slug = s(&i, "slug").unwrap_or("").to_string();
    let mut sets = vec!["updated_ms = :now".to_string()];
    let mut p = Map::new();
    for f in ["name", "kind", "website", "email", "phone", "address", "city", "region", "notes", "status", "extract"] {
        if let Some(v) = i.get(f).and_then(|v| v.as_str()) {
            if f == "website" && !v.is_empty() && !valid_website(v) {
                error(400, "website must be an http(s) URL");
                return 0;
            }
            sets.push(format!("{f} = :{f}"));
            let v = v.trim();
            p.insert(f.to_string(), if v.is_empty() && f != "notes" && f != "name" { Value::Null } else { Value::String(v.to_string()) });
        }
    }
    if let Some(w) = i.get("watch").and_then(|v| v.as_bool()) {
        sets.push("watch = :watch".into());
        p.insert("watch".into(), json!(w as i64));
    }
    if i.contains_key("website") || i.contains_key("extract") {
        sets.push("checked_ms = NULL".into());  // read it again soon
    }
    p.insert("now".into(), json!(goober::now_ms()));
    p.insert("slug".into(), json!(slug));
    let Some(r) = sql_or_500(DB, &format!("UPDATE ranches SET {} WHERE slug = :slug", sets.join(", ")), Value::Object(p)) else {
        return 0;
    };
    if r.changes() == 0 {
        return not_found(&slug);
    }
    respond_json(200, &json!({ "slug": slug, "updated": sets.len() - 1 }));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranch_list() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let Some(r) = sql_or_500(
        DB,
        &format!(
            "SELECT json_object('ranches', json_group_array(json(j))) FROM (SELECT {RANCH} AS j FROM ranches \
             WHERE (:status IS NULL OR status = :status) AND (:kind IS NULL OR kind = :kind) \
               AND (:region IS NULL OR region = :region COLLATE NOCASE) \
               AND (:q IS NULL OR instr(lower(name || ' ' || coalesce(city, '') || ' ' || notes || ' ' || coalesce(website, '')), lower(:q)) > 0) \
             ORDER BY name LIMIT :limit OFFSET :offset)"
        ),
        json!({ "status": s(&i, "status"), "kind": s(&i, "kind"), "region": s(&i, "region"), "q": s(&i, "query"),
                "limit": i.get("limit").and_then(|v| v.as_i64()).unwrap_or(100),
                "offset": i.get("offset").and_then(|v| v.as_i64()).unwrap_or(0) }),
    ) else { return 0 };
    respond_raw_json(200, r.first().and_then(|v| v.as_str()).unwrap_or("{\"ranches\":[]}"));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranch_get() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let slug = s(&i, "slug").unwrap_or("").to_string();
    let Some(r) = sql_or_500(
        DB,
        &format!(
            "SELECT json_set({RANCH}, '$.notes', notes, \
               '$.contacts', (SELECT json_group_array(json_object('at', strftime('%Y-%m-%dT%H:%M:%SZ', c.at_ms / 1000, 'unixepoch'), \
                  'channel', c.channel, 'direction', c.direction, 'summary', c.summary, 'by', c.actor)) \
                  FROM (SELECT * FROM contacts WHERE ranch_id = ranches.id ORDER BY at_ms DESC LIMIT 100) c), \
               '$.offerings', (SELECT json_group_array(json_object('name', name, 'category', category, 'price', price_text, \
                  'unit', unit, 'in_stock', in_stock, 'url', url, 'source', source, \
                  'seen_at', strftime('%Y-%m-%dT%H:%M:%SZ', seen_ms / 1000, 'unixepoch'))) \
                  FROM (SELECT * FROM offerings WHERE ranch_id = ranches.id ORDER BY name LIMIT 500))) \
             FROM ranches WHERE slug = :slug"
        ),
        json!({ "slug": slug }),
    ) else { return 0 };
    match r.first().and_then(|v| v.as_str()) {
        Some(j) => respond_raw_json(200, j),
        None => return not_found(&slug),
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_contact_log() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let slug = s(&i, "slug").unwrap_or("").to_string();
    let Some(ranch) = ranch_by_slug(&slug) else { return not_found(&slug) };
    let now = goober::now_ms();
    let by = req.user()["client_id"].as_str().map(str::to_string)
        .unwrap_or_else(|| req.user()["email"].as_str().unwrap_or("owner").to_string());
    if sql_or_500(
        DB,
        "INSERT INTO contacts (ranch_id, at_ms, channel, direction, summary, actor) VALUES (:r, :now, :c, :d, :s, :by)",
        json!({ "r": ranch["id"], "now": now, "c": s(&i, "channel"), "d": s(&i, "direction"), "s": s(&i, "summary"), "by": by }),
    ).is_none() {
        return 0;
    }
    let status = s(&i, "status").filter(|st| STATUSES.contains(st));
    if sql_or_500(
        DB,
        "UPDATE ranches SET last_contact_ms = :now, updated_ms = :now, status = coalesce(:status, \
           CASE WHEN status = 'prospect' AND :dir = 'out' THEN 'contacted' WHEN status IN ('prospect', 'contacted') AND :dir = 'in' THEN 'replied' ELSE status END) \
         WHERE id = :r",
        json!({ "now": now, "status": status, "dir": s(&i, "direction"), "r": ranch["id"] }),
    ).is_none() {
        return 0;
    }
    respond_json(201, &json!({ "slug": slug, "logged": true }));
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranch_check() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let slug = s(&i, "slug").unwrap_or("").to_string();
    let Some(ranch) = ranch_by_slug(&slug) else { return not_found(&slug) };
    let out = refresh(&ranch);
    respond_json(if out["error"].is_null() { 200 } else { 422 }, &out);
    0
}

/// Every five minutes: the watched site read longest ago, if a week has passed.
#[unsafe(no_mangle)]
pub extern "C" fn goober_route_ranches_refresh() -> i32 {
    let now = goober::now_ms();
    let Some(r) = sql_or_500(
        DB,
        "SELECT id, website, extract, name FROM ranches WHERE watch = 1 AND website IS NOT NULL \
           AND (checked_ms IS NULL OR checked_ms < :due) ORDER BY checked_ms IS NOT NULL, checked_ms LIMIT 1",
        json!({ "due": now - WEEK_MS }),
    ) else { return 0 };
    if let Some(row) = r.rows().first() {
        let out = refresh(&json!({ "id": row[0], "website": row[1], "extract": row[2], "name": row[3] }));
        goober::log(&format!("refreshed {}: {}", row[3].as_str().unwrap_or("?"), out));
    }
    0
}

fn offerings(q: Option<&str>, cat: Option<&str>, region: Option<&str>, in_stock: Option<bool>, limit: i64) -> Option<String> {
    let r = sql_or_500(
        DB,
        &format!(
            "SELECT json_object('offerings', json_group_array(json(j))) FROM (SELECT {OFFERING} AS j \
             FROM offerings o JOIN ranches r ON r.id = o.ranch_id WHERE r.status != 'declined' \
               AND (:q IS NULL OR instr(lower(o.name), lower(:q)) > 0) AND (:cat IS NULL OR o.category = :cat) \
               AND (:region IS NULL OR r.region = :region COLLATE NOCASE) AND (:stock IS NULL OR o.in_stock = :stock) \
             ORDER BY o.category, o.price_cents IS NULL, o.price_cents LIMIT :limit)"
        ),
        json!({ "q": q, "cat": cat, "region": region, "stock": in_stock.map(|b| b as i64), "limit": limit }),
    )?;
    Some(r.first().and_then(|v| v.as_str()).unwrap_or("{\"offerings\":[]}").to_string())
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_offering_search() -> i32 {
    let req = Request::read();
    let i = input(&req);
    if let Some(j) = offerings(s(&i, "query"), s(&i, "category"), s(&i, "region"), i.get("in_stock").and_then(|v| v.as_bool()),
                               i.get("limit").and_then(|v| v.as_i64()).unwrap_or(100)) {
        respond_raw_json(200, &j);
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_offering_add() -> i32 {
    let req = Request::read();
    let i = input(&req);
    let slug = s(&i, "slug").unwrap_or("").to_string();
    let Some(ranch) = ranch_by_slug(&slug) else { return not_found(&slug) };
    let offer = Offer {
        name: s(&i, "name").unwrap_or("").to_string(),
        price_text: s(&i, "price").map(str::to_string),
        unit: s(&i, "unit").map(str::to_string),
        in_stock: i.get("in_stock").and_then(|v| v.as_bool()),
        url: s(&i, "url").map(str::to_string),
        category: s(&i, "category").map(str::to_string),
        source: "manual",
        ..Default::default()
    };
    match store_offers(ranch["id"].as_i64().unwrap_or(0), &[offer], goober::now_ms()) {
        Ok(_) => respond_json(201, &json!({ "slug": slug, "added": true })),
        Err(e) => {
            goober::log(&format!("offering_add: {e}"));
            error(500, "storage failed");
        }
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_meat_offerings() -> i32 {
    let req = Request::read();
    let q = &req.doc["query"];
    if let Some(j) = offerings(q["q"].as_str(), q["category"].as_str(), q["region"].as_str(), None, q["limit"].as_i64().unwrap_or(100)) {
        goober::header("cache-control", "public, max-age=300");
        goober::respond(200, "application/json", j.as_bytes());
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn goober_route_meat_ranches() -> i32 {
    let req = Request::read();
    let Some(r) = sql_or_500(
        DB,
        "SELECT json_object('ranches', json_group_array(json_object('name', name, 'slug', slug, 'kind', kind, \
           'city', city, 'region', region, 'website', website, 'phone', phone, \
           'offerings', (SELECT count(*) FROM offerings o WHERE o.ranch_id = ranches.id)))) \
         FROM (SELECT * FROM ranches WHERE status != 'declined' AND (:region IS NULL OR region = :region COLLATE NOCASE) \
               ORDER BY name LIMIT 1000) AS ranches",
        json!({ "region": req.doc["query"]["region"] }),
    ) else { return 0 };
    goober::header("cache-control", "public, max-age=300");
    goober::respond(200, "application/json", r.first().and_then(|v| v.as_str()).unwrap_or("{}").as_bytes());
    0
}
