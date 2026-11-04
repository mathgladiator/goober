// The goober operator panel (design/machine/028.Panel.md § 3). Plain ES2020, no build step.
// Everything the host sends is placed as text (textContent or SVG text), never as markup.
'use strict';

// ------------------------------------------------------------------ helpers

const $main = document.getElementById('main');
const SVGNS = 'http://www.w3.org/2000/svg';

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
function s(tag, attrs, ...kids) {
  const el = document.createElementNS(SVGNS, tag);
  setAttrs(el, attrs);
  append(el, kids);
  return el;
}
function setAttrs(el, attrs) {
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : String(v));
  }
}
function append(el, kids) {
  for (const k of kids.flat(Infinity)) {
    if (k === undefined || k === null || k === false) continue;
    el.appendChild(k instanceof Node ? k : document.createTextNode(String(k)));
  }
}
function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }

const store = {
  get(k, d) { try { const v = localStorage.getItem('panel.' + k); return v === null ? d : v; } catch { return d; } },
  set(k, v) { try { localStorage.setItem('panel.' + k, v); } catch { /* private mode */ } },
};

class ApiError extends Error {}

async function api(tool, args) {
  const res = await fetch('/api/' + tool, {
    method: 'POST', credentials: 'same-origin',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(args || {}),
  });
  if (res.status === 401) { location.href = '/identity/sign-in/as-host?return=%2F'; throw new ApiError('signed out'); }
  let body = {};
  try { body = await res.json(); } catch { /* not JSON */ }
  if (!res.ok) throw new ApiError(typeof body.error === 'string' ? body.error : tool + ' failed (' + res.status + ')');
  return body.result;
}

const fmt = {
  n(v) {
    v = Number(v || 0);
    if (v >= 1e9) return (v / 1e9).toFixed(1) + 'G';
    if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
    if (v >= 1e4) return (v / 1e3).toFixed(1) + 'k';
    return String(Math.round(v));
  },
  us(v) {
    v = Number(v || 0);
    if (!v) return '–';
    if (v >= 1e6) return (v / 1e6).toFixed(v >= 1e7 ? 0 : 1) + ' s';
    if (v >= 1e3) return (v / 1e3).toFixed(v >= 1e4 ? 0 : 1) + ' ms';
    return Math.round(v) + ' µs';
  },
  bytes(v) {
    v = Number(v || 0);
    if (v >= 1 << 30) return (v / (1 << 30)).toFixed(1) + ' GiB';
    if (v >= 1 << 20) return (v / (1 << 20)).toFixed(1) + ' MiB';
    if (v >= 1 << 10) return (v / (1 << 10)).toFixed(1) + ' KiB';
    return v + ' B';
  },
  pct(a, b) { return b ? (100 * a / b).toFixed(a / b < 0.1 ? 1 : 0) + '%' : '–'; },
  time(ms) {
    const d = new Date(ms);
    return d.toLocaleString(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  },
  clock(ms, stepMs) {
    const d = new Date(ms);
    if (stepMs >= 3600e3) return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
    return d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  },
  ago(ms) {
    const s = Math.max(0, (Date.now() - ms) / 1000);
    if (s < 90) return Math.round(s) + 's ago';
    if (s < 5400) return Math.round(s / 60) + 'm ago';
    if (s < 129600) return Math.round(s / 3600) + 'h ago';
    return Math.round(s / 86400) + 'd ago';
  },
  dur(ms) {
    const s = ms / 1000;
    if (s < 3600) return Math.round(s / 60) + ' min';
    if (s < 172800) return (s / 3600).toFixed(1) + ' h';
    return (s / 86400).toFixed(1) + ' days';
  },
};

function heatClass(share) {
  if (!share) return 0;
  if (share < 0.05) return 1;
  if (share < 0.15) return 2;
  if (share < 0.35) return 3;
  return 4;
}

function card(title, ...kids) { return h('section', { class: 'card' }, title ? h('h2', {}, title) : null, kids); }
function wideCard(title, ...kids) { return h('section', { class: 'card wide' }, title ? h('h2', {}, title) : null, kids); }
function errorBox(e) { return h('div', { class: 'notice bad' }, h('span', { class: 'err' }, 'Error: '), e.message || String(e)); }

function table(columns, rows, onClick) {
  const head = h('tr', {}, columns.map((c) => h('th', { class: c.num ? 'num' : '' }, c.label)));
  const body = rows.map((r) => {
    const tr = h('tr', { class: onClick ? 'click' : '' }, columns.map((c) => {
      const v = c.get(r);
      return h('td', { class: (c.num ? 'num ' : '') + (c.cls ? c.cls(r) : '') }, v instanceof Node ? v : String(v ?? ''));
    }));
    if (onClick) tr.addEventListener('click', () => onClick(r));
    return tr;
  });
  return h('div', { class: 'scroll' }, h('table', {}, h('thead', {}, head), h('tbody', {}, body)));
}

function segmented(options, value, onChange) {
  return h('div', { class: 'seg' }, options.map((o) =>
    h('button', { type: 'button', class: o === value ? 'on' : '', onclick: () => onChange(o) }, o)));
}

function windowPicker(onChange) {
  return h('span', {}, h('label', {}, 'Window '), segmented(['1h', '12h', '7d'], state.window, (w) => {
    state.window = w; store.set('window', w); onChange();
  }));
}

// ------------------------------------------------------------------ charts

function niceMax(v) {
  if (v <= 0) return 1;
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
  return 10 * p;
}

// Requests per point, stacked by status class.
function requestsChart(series) {
  const pts = series.points, W = 900, H = 170, L = 44, B = 22, T = 8;
  const max = niceMax(Math.max(1, ...pts.map((p) => p.n)));
  const bw = (W - L) / pts.length;
  const y = (v) => T + (H - T - B) * (1 - v / max);
  const g = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Requests over time' });
  for (const f of [0, 0.5, 1]) {
    g.appendChild(s('line', { class: 'axis', x1: L, x2: W, y1: y(max * f), y2: y(max * f) }));
    g.appendChild(s('text', { x: L - 6, y: y(max * f) + 4, 'text-anchor': 'end' }, fmt.n(max * f)));
  }
  pts.forEach((p, i) => {
    const x = L + i * bw, w = Math.max(1, bw - (bw > 4 ? 1 : 0));
    let base = 0;
    const parts = [['s2', p.n - p.n3 - p.n4 - p.n5], ['s3', p.n3], ['s4', p.n4], ['s5', p.n5]];
    for (const [cls, v] of parts) {
      if (v <= 0) continue;
      g.appendChild(s('rect', { class: cls, x, width: w, y: y(base + v), height: y(base) - y(base + v) }));
      base += v;
    }
    const hover = s('rect', { class: 'hover', x, width: bw, y: T, height: H - T - B });
    hover.appendChild(s('title', {}, `${fmt.clock(p.t, series.step_ms)} · ${p.n} requests` +
      (p.n4 ? ` · ${p.n4} 4xx` : '') + (p.n5 ? ` · ${p.n5} 5xx` : '') +
      (p.n ? ` · p50 ${fmt.us(p.p50_us)} · p99 ${fmt.us(p.p99_us)}` : '')));
    g.appendChild(hover);
  });
  timeAxis(g, series, L, W, H - 6);
  return h('div', {}, g, h('div', { class: 'legend' },
    h('span', {}, h('i', { class: 's2' }), '2xx'), h('span', {}, h('i', { class: 's3' }), '3xx'),
    h('span', {}, h('i', { class: 's4' }), '4xx'), h('span', {}, h('i', { class: 's5' }), '5xx')));
}

// Counts per point stacked by part ({key, cls, label}), with each point's rate against `of` in
// its tooltip: the Errors and Box failures views.
function stackedChart(series, parts, of, ofLabel, aria, unit = fmt.n) {
  const pts = series.points, W = 900, H = 150, L = 52, B = 22, T = 8;
  const total = (p) => parts.reduce((a, x) => a + (p[x.key] || 0), 0);
  const max = niceMax(Math.max(1, ...pts.map(total)));
  const bw = (W - L) / pts.length;
  const y = (v) => T + (H - T - B) * (1 - v / max);
  const g = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': aria });
  for (const f of [0, 0.5, 1]) {
    g.appendChild(s('line', { class: 'axis', x1: L, x2: W, y1: y(max * f), y2: y(max * f) }));
    g.appendChild(s('text', { x: L - 6, y: y(max * f) + 4, 'text-anchor': 'end' }, unit(max * f)));
  }
  pts.forEach((p, i) => {
    const x = L + i * bw, w = Math.max(1, bw - (bw > 4 ? 1 : 0));
    let base = 0;
    for (const part of parts) {
      const v = p[part.key] || 0;
      if (v <= 0) continue;
      g.appendChild(s('rect', { class: part.cls, x, width: w, y: y(base + v), height: y(base) - y(base + v) }));
      base += v;
    }
    const hover = s('rect', { class: 'hover', x, width: bw, y: T, height: H - T - B });
    const said = parts.filter((x) => p[x.key]).map((x) => `${unit(p[x.key])} ${x.label}`);
    hover.appendChild(s('title', {}, `${fmt.clock(p.t, series.step_ms)} · ${unit(p[of])} ${ofLabel}` +
      (said.length ? ' · ' + said.join(' · ') + ' (' + fmt.pct(total(p), p[of]) + ')' : '')));
    g.appendChild(hover);
  });
  timeAxis(g, series, L, W, H - 6);
  return h('div', {}, g, h('div', { class: 'legend' }, parts.map((x) => h('span', {}, h('i', { class: x.cls }), x.label))));
}

// One MCP queue's report (047 § 13): what arrived, waited, ran, and was refused, and the fixed
// limits it runs under. Agents are never named, only their class.
function mcpQueueCard(r, title) {
  const t = r.totals, l = r.limits;
  const kids = [h('p', { class: 'muted small' }, 'Every agent gets a strict rate (' + l.per_minute + ' calls a minute, ' + l.burst +
      ' at once) and a fixed queue: ' + l.agent_running + ' running and ' + l.agent_waiting + ' waiting per agent, ' + l.queue_running +
      ' running in this queue, waits up to ' + (l.max_wait_ms / 1000) + ' s. Now ' + r.now.running + ' running, ' + r.now.waiting + ' waiting.'),
    h('div', { class: 'stats' }, stat(fmt.n(t.offered), 'calls'), stat(fmt.n(t.queued), 'waited'), stat(fmt.n(t.ran), 'ran'),
      stat(fmt.n(t.refused_rate), 'refused: rate'), stat(fmt.n(t.refused_full), 'refused: full'),
      stat(fmt.n(t.refused_wait), 'refused: waited too long'), stat(fmt.n(t.gone), 'left while waiting'),
      stat(fmt.n(t.peak_waiting), 'peak waiting'), stat(fmt.us(t.wait_p99_ms * 1000), 'p99 wait'))];
  if (r.verdicts.length) kids.push(h('div', { class: 'notice' }, r.verdicts.map((v) => h('p', {}, v))));
  const series = { ...r, points: r.points.map((p) => ({ ...p, started: p.ran - Math.min(p.ran, p.queued) })) };
  kids.push(stackedChart(series, [
    { key: 'started', cls: 's2', label: 'ran at once' }, { key: 'queued', cls: 's3', label: 'waited' },
    { key: 'refused_rate', cls: 's4', label: 'refused: rate' }, { key: 'refused_full', cls: 's5', label: 'refused: full' },
    { key: 'refused_wait', cls: 'fp', label: 'refused: waited too long' }], 'offered', 'calls', title + ' over time'));
  const classes = Object.entries(r.by_class || {});
  if (classes.length) {
    kids.push(table([
      { label: 'Class', get: (x) => x[0], cls: () => 'mono' },
      { label: 'Calls', get: (x) => fmt.n(x[1].offered) }, { label: 'Waited', get: (x) => fmt.n(x[1].queued) },
      { label: 'Refused', get: (x) => fmt.n(x[1].refused) }, { label: 'p50 wait', get: (x) => fmt.us(x[1].wait_p50_ms * 1000) },
      { label: 'p99 wait', get: (x) => fmt.us(x[1].wait_p99_ms * 1000) }], classes));
  }
  return wideCard(title, kids);
}

// p50 and p99 latency per point, on a log scale.
function latencyChart(series) {
  const pts = series.points, W = 900, H = 130, L = 44, B = 22, T = 8;
  const vals = pts.flatMap((p) => (p.n ? [p.p50_us, p.p99_us] : [])).filter((v) => v > 0);
  const g = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Latency over time' });
  if (!vals.length) {
    g.appendChild(s('text', { x: W / 2, y: H / 2, 'text-anchor': 'middle' }, 'no requests in this window'));
    return g;
  }
  const lo = Math.log10(Math.max(10, Math.min(...vals) / 1.5)), hi = Math.log10(Math.max(...vals) * 1.5);
  const y = (v) => T + (H - T - B) * (1 - (Math.log10(Math.max(v, 1)) - lo) / Math.max(0.1, hi - lo));
  const bw = (W - L) / pts.length;
  for (const v of [Math.pow(10, lo), Math.pow(10, (lo + hi) / 2), Math.pow(10, hi)]) {
    g.appendChild(s('line', { class: 'axis', x1: L, x2: W, y1: y(v), y2: y(v) }));
    g.appendChild(s('text', { x: L - 6, y: y(v) + 4, 'text-anchor': 'end' }, fmt.us(v)));
  }
  for (const [key, cls] of [['p50_us', 'l50'], ['p99_us', 'l99']]) {
    let d = '';
    pts.forEach((p, i) => {
      if (!p.n) return;
      d += (d && pts[i - 1] && pts[i - 1].n ? 'L' : 'M') + (L + i * bw + bw / 2).toFixed(1) + ' ' + y(p[key]).toFixed(1);
    });
    g.appendChild(s('path', { class: cls, d }));
  }
  timeAxis(g, series, L, W, H - 6);
  return h('div', {}, g, h('div', { class: 'legend' },
    h('span', {}, h('i', { class: 'l50' }), 'p50'), h('span', {}, h('i', { class: 'l99' }), 'p99')));
}

// A sampled level per point (the outbox's waiting elements), on a linear scale; a gap where the
// host took no sample.
function levelChart(series, label) {
  const pts = series.points, W = 900, H = 120, L = 44, B = 22, T = 8;
  const vals = pts.map((p) => p.value).filter((v) => v !== null && v !== undefined);
  const g = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': label + ' over time' });
  if (!vals.length) {
    g.appendChild(s('text', { x: W / 2, y: H / 2, 'text-anchor': 'middle' }, 'no samples in this window'));
    return g;
  }
  const max = niceMax(Math.max(1, ...vals));
  const y = (v) => T + (H - T - B) * (1 - v / max);
  const bw = (W - L) / pts.length;
  for (const f of [0, 0.5, 1]) {
    g.appendChild(s('line', { class: 'axis', x1: L, x2: W, y1: y(max * f), y2: y(max * f) }));
    g.appendChild(s('text', { x: L - 6, y: y(max * f) + 4, 'text-anchor': 'end' }, fmt.n(max * f)));
  }
  let d = '';
  pts.forEach((p, i) => {
    if (p.value === null || p.value === undefined) return;
    const prev = pts[i - 1];
    d += (d && prev && prev.value !== null && prev.value !== undefined ? 'L' : 'M') +
      (L + i * bw + bw / 2).toFixed(1) + ' ' + y(p.value).toFixed(1);
    const hover = s('rect', { class: 'hover', x: L + i * bw, width: bw, y: T, height: H - T - B });
    hover.appendChild(s('title', {}, `${fmt.clock(p.t, series.step_ms)} · at most ${fmt.n(p.value)} ${label}`));
    g.appendChild(hover);
  });
  g.appendChild(s('path', { class: 'l99', d }));
  timeAxis(g, series, L, W, H - 6);
  return h('div', {}, g, h('div', { class: 'legend' }, h('span', {}, h('i', { class: 'l99' }), label + ' (the most at once)')));
}

// A service_report's series as the stacked and latency charts read them: calls that went and
// failed, and n for "had calls".
function serviceSeries(r) {
  return { ...r, points: r.points.map((p) => ({ ...p, n: p.calls, ok: p.calls - p.failed })) };
}

function timeAxis(g, series, L, W, y) {
  const pts = series.points, ticks = 6, bw = (W - L) / pts.length;
  for (let i = 0; i < ticks; i++) {
    const idx = Math.round(i * (pts.length - 1) / (ticks - 1));
    g.appendChild(s('text', { x: L + idx * bw + bw / 2, y, 'text-anchor': i === 0 ? 'start' : i === ticks - 1 ? 'end' : 'middle' },
      fmt.clock(pts[idx].t, series.step_ms)));
  }
}

// Columns of nodes, joined by curves whose width is traffic and whose color is health.
function flowGraph(graph, onNode) {
  // Three columns at least; tables (008 § 4) add a fourth, right of their databases.
  const ncol = Math.max(3, 1 + Math.max(0, ...graph.nodes.map((n) => n.column || 0)));
  const cols = Array.from({ length: ncol }, () => []);
  for (const n of graph.nodes) cols[Math.min(ncol - 1, Math.max(0, n.column))].push(n);
  // Tables sit beside their database: by database, then name.
  const order = (n) => n.kind === 'table' ? (n.database || '') + '\u0000' + n.label : n.label;
  for (const c of cols) c.sort((a, b) => (a.kind === b.kind ? order(a).localeCompare(order(b)) : a.kind.localeCompare(b.kind)));
  const NW = 210, NH = 46, GAP = 14, W = Math.max(1000, ncol * 290);
  const colX = cols.map((_, i) => 10 + i * (W - NW - 20) / (ncol - 1));
  const rows = Math.max(1, ...cols.map((c) => c.length));
  const H = rows * (NH + GAP) + GAP;
  const at = {};
  cols.forEach((c, ci) => {
    const top = (H - c.length * (NH + GAP) + GAP) / 2;
    c.forEach((n, i) => { at[n.id] = { x: colX[ci], y: top + i * (NH + GAP), col: ci }; });
  });
  const maxN = Math.max(1, ...graph.edges.map((e) => e.n));
  const totalUs = graph.nodes.filter((n) => n.kind === 'box').reduce((a, n) => a + (n.us || 0), 0);
  const g = s('svg', { class: 'flow', viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Data flow' });
  for (const e of graph.edges) {
    const a = at[e.from], b = at[e.to];
    if (!a || !b) continue;
    let d;
    if (a.col === b.col) {
      const x = a.x + NW, y1 = a.y + NH / 2, y2 = b.y + NH / 2, bend = 40 + Math.abs(y2 - y1) / 4;
      d = `M${x} ${y1} C${x + bend} ${y1} ${x + bend} ${y2} ${x} ${y2}`;
    } else {
      const x1 = a.x + NW, y1 = a.y + NH / 2, x2 = b.x, y2 = b.y + NH / 2, mid = (x1 + x2) / 2;
      d = `M${x1} ${y1} C${mid} ${y1} ${mid} ${y2} ${x2} ${y2}`;
    }
    const width = e.n ? 1.5 + 14 * Math.log1p(e.n) / Math.log1p(maxN) : 1;
    const bad = e.n && e.errors / e.n >= 0.05;
    const grant = !!e.grants;
    const path = s('path', { class: 'edge' + (e.n ? '' : ' idle') + (bad ? ' bad' : '') + (grant ? ' grant' : '') +
      (e.unused ? ' unused' : ''), d, 'stroke-width': (grant ? Math.min(width, 6) : width).toFixed(1) });
    const title = grant
      ? `${e.to.replace(/^table:/, '')}: ` + e.grants.map((gr) => `${gr.box} (${gr.strategy}) may ${gr.ops}` +
          (Array.isArray(gr.read) ? `, reads ${gr.read.join(', ') || 'nothing'}` : gr.read ? ', reads every column' : '') +
          (Array.isArray(gr.update) ? (gr.update.length ? `, updates ${gr.update.join(', ')}` : '') : gr.update ? ', updates every column' : '') +
          (gr.calls ? `; used ${gr.used_ops || 'nothing'} in ${fmt.n(gr.calls)} calls` : '; not used since the host started')).join('\n')
      : `${e.from.replace(/^\w+:/, '')} → ${e.to.replace(/^\w+:/, '')}: ${fmt.n(e.n)} ${e.what || ''}` +
        (e.errors ? `, ${fmt.n(e.errors)} failed` : '') + (e.us ? `, ${fmt.us(e.us)} CPU` : '') +
        (e.bytes_in ? `, ${fmt.bytes(e.bytes_out)} out / ${fmt.bytes(e.bytes_in)} in` : '') + (e.approx ? ' (split evenly across its databases)' : '');
    path.appendChild(s('title', {}, title));
    g.appendChild(path);
    if (grant && e.ops) {
      // The operations granted (r, i, u, d), on the edge just short of the table.
      const lx = b.x - 8, ly = b.y + NH / 2 - 4;
      g.appendChild(s('text', { class: 'ops' + (e.unused ? ' unused' : ''), x: lx, y: ly, 'text-anchor': 'end' }, e.ops));
    }
  }
  for (const n of graph.nodes) {
    const p = at[n.id];
    const clickable = (n.kind === 'box' || n.kind === 'service') && onNode;
    const grp = s('g', { class: 'node ' + n.kind + (clickable ? ' click' : ''), transform: `translate(${p.x},${p.y})` });
    grp.appendChild(s('rect', { width: NW, height: NH, rx: 8 }));
    if (n.kind === 'box') {
      grp.appendChild(s('rect', { class: 'heatbar h' + heatClass(totalUs ? (n.us || 0) / totalUs : 0) + 'f', width: 6, height: NH - 8, x: 4, y: 4, rx: 3 }));
    }
    grp.appendChild(s('text', { class: 'label', x: 16, y: 19 }, clip(n.label, 26)));
    grp.appendChild(s('text', { class: 'sub', x: 16, y: 35 }, clip(nodeSub(n), 32)));
    grp.appendChild(s('title', {}, n.label + ' — ' + nodeSub(n)));
    if (clickable) grp.addEventListener('click', () => onNode(n));
    g.appendChild(grp);
  }
  return h('div', { class: 'scroll' }, g);
}

// A box opens its page; a service its page in Services.
function openNode(n) {
  if (n.kind === 'service') location.hash = '#/services?service=' + encodeURIComponent(n.mailer ? 'mailer' : n.label);
  else location.hash = '#/box/' + encodeURIComponent(n.label);
}

function nodeSub(n) {
  switch (n.kind) {
    case 'tenant': return (n.domains || []).join(', ') || 'tenant';
    case 'box': return `${fmt.n(n.n || 0)} req · ${fmt.us(n.us)} CPU` + (n.n5 ? ` · ${fmt.n(n.n5)} 5xx` : '');
    case 'route': return `${fmt.n(n.n || 0)} · p99 ${fmt.us(n.p99_us)}` + (n.n5 ? ` · ${fmt.n(n.n5)} 5xx` : '');
    case 'database': return 'database';
    case 'table': return 'table in ' + (n.database || '');
    case 'service': {
      const what = n.mailer ? 'sends every mail' : n.mail ? 'mail' : n.web ? 'the web (robot)' : (n.base || 'service');
      if (!n.calls) return what;
      return `${fmt.n(n.calls)} calls` + (n.failed ? ` · ${fmt.n(n.failed)} failed` : '') + (n.p99_us ? ` · p99 ${fmt.us(n.p99_us)}` : '');
    }
    case 'pages': return 'pages and files';
    case 'host': return 'host routes';
    default: return n.kind;
  }
}
function clip(t, n) { t = String(t ?? ''); return t.length > n ? t.slice(0, n - 1) + '…' : t; }

// ------------------------------------------------------------------ diffs

function lineDiff(before, after) {
  const a = (before ?? '').split('\n'), b = (after ?? '').split('\n');
  if (a.length * b.length > 4e6) return null;
  const n = a.length, m = b.length, w = m + 1;
  const L = new Uint32Array((n + 1) * w);
  for (let i = n - 1; i >= 0; i--) for (let j = m - 1; j >= 0; j--) {
    L[i * w + j] = a[i] === b[j] ? L[(i + 1) * w + j + 1] + 1 : Math.max(L[(i + 1) * w + j], L[i * w + j + 1]);
  }
  const out = [];
  let i = 0, j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && a[i] === b[j]) { out.push([' ', a[i]]); i++; j++; }
    // Removals before additions, as a diff reads.
    else if (i < n && (j >= m || L[(i + 1) * w + j] >= L[i * w + j + 1])) { out.push(['-', a[i]]); i++; }
    else { out.push(['+', b[j]]); j++; }
  }
  return out;
}

function diffView(before, after) {
  const d = lineDiff(before, after);
  if (!d) return h('p', { class: 'muted' }, 'Too large to compare line by line.');
  const box = h('div', { class: 'diff' });
  // Unchanged runs longer than six lines are folded to their first and last three.
  let run = [];
  const flush = () => {
    if (run.length > 6) {
      run.slice(0, 3).forEach((l) => box.appendChild(h('div', {}, '  ' + l)));
      box.appendChild(h('div', { class: 'gap' }, `… ${run.length - 6} unchanged lines …`));
      run.slice(-3).forEach((l) => box.appendChild(h('div', {}, '  ' + l)));
    } else run.forEach((l) => box.appendChild(h('div', {}, '  ' + l)));
    run = [];
  };
  for (const [op, line] of d) {
    if (op === ' ') { run.push(line); continue; }
    flush();
    box.appendChild(h('div', { class: op === '+' ? 'add' : 'del' }, op + ' ' + line));
  }
  flush();
  if (!d.some(([op]) => op !== ' ')) return h('p', { class: 'muted' }, 'No change in the text.');
  return box;
}

// A manifest as paths: routes.http./x.get.name = "x".
function flatten(v, prefix, out) {
  if (v && typeof v === 'object' && !Array.isArray(v)) {
    const keys = Object.keys(v);
    if (!keys.length) out[prefix] = '{}';
    for (const k of keys) flatten(v[k], prefix ? prefix + '.' + k : k, out);
  } else out[prefix] = JSON.stringify(v);
  return out;
}

function manifestDiff(beforeText, afterText) {
  let a = {}, b = {};
  try { a = beforeText ? JSON.parse(beforeText) : {}; b = afterText ? JSON.parse(afterText) : {}; } catch { return null; }
  const fa = flatten(a, '', {}), fb = flatten(b, '', {});
  const keys = [...new Set([...Object.keys(fa), ...Object.keys(fb)])].sort();
  const rows = [];
  for (const k of keys) {
    if (fa[k] === fb[k]) continue;
    rows.push({ path: k, before: fa[k], after: fb[k] });
  }
  return rows;
}

// ------------------------------------------------------------------ views

const state = { window: ['1h', '12h', '7d'].includes(store.get('window', '12h')) ? store.get('window', '12h') : '12h', me: null, refresh: null };

// A focused operator (030) sees only their tenants: host-wide views narrow to the first of them.
function focus() { return state.me && state.me.focus && state.me.focus.length ? state.me.focus : null; }
function focusNote(what) {
  return h('div', { class: 'notice' }, 'You are focused on ' + focus().join(', ') + '. ' + what +
    ' An owner can widen your scope in ', h('a', { href: '#/operators' }, 'Operators'), '.');
}

// The service's key graphs (028 § 4.5): requests, the error rate, latency, box failures, calls
// out to services, the outbox (deliveries and the queue), and mail, over the window.
async function keyGraphs(tenant, traffic) {
  const args = tenant ? { window: state.window, tenant } : { window: state.window };
  const [failures, services, mailer] = await Promise.all([
    api('box_failures', { ...args, latest: 1 }), api('service_report', { ...args, latest: 1 }),
    api('service_report', { ...args, service: 'mailer', latest: 1 })]);
  const svc = serviceSeries(services), mail = serviceSeries(mailer);
  const t = traffic.series.points.reduce((a, p) => ({ n: a.n + p.n, n4: a.n4 + p.n4, n5: a.n5 + p.n5 }), { n: 0, n4: 0, n5: 0 });
  const waiting = (services.outbox.now || []).reduce((a, x) => a + x.value, 0);
  const more = (href, text) => h('p', {}, h('a', { href }, text));
  return [
    h('h2', { class: 'section' }, 'Key graphs'),
    h('div', { class: 'stats' },
      stat(fmt.n(t.n), 'requests'), stat(fmt.pct(t.n4, t.n), '4xx'), stat(fmt.pct(t.n5, t.n), '5xx'),
      stat(fmt.pct(failures.totals.failures, failures.totals.calls), 'box calls failed'),
      stat(fmt.n(services.totals.calls - mailer.totals.calls), 'calls out'),
      stat(fmt.pct(services.totals.failed - mailer.totals.failed, services.totals.calls - mailer.totals.calls), 'of them failed'),
      stat(fmt.n(waiting), 'waiting in the outbox'),
      stat(fmt.n(mailer.totals.calls - mailer.totals.failed), 'mails sent'), stat(fmt.n(mailer.totals.failed), 'mails failed')),
    h('div', { class: 'grid' },
      card('Requests', requestsChart(traffic.series)),
      card('Error rate', stackedChart(traffic.series, [
        { key: 'n4', cls: 's4', label: '4xx' }, { key: 'n5', cls: 's5', label: '5xx' }], 'n', 'requests', 'Errors over time'),
        more('#/errors', 'Errors →')),
      card('Latency', latencyChart(traffic.series)),
      card('Box failures', stackedChart(failures, [
        { key: 'failures', cls: 's5', label: 'failed calls' }, { key: 'sql', cls: 's4', label: 'SQL' },
        { key: 'outbox', cls: 'fp', label: 'deliveries' }, { key: 'internal', cls: 's3', label: 'calls to boxes' }],
        'calls', 'box calls', 'Box failures over time'), more('#/failures', 'Box failures →')),
      card('Calls out to services', stackedChart(svc, [
        { key: 'ok', cls: 's2', label: 'answered' }, { key: 'failed', cls: 's5', label: 'failed' }],
        'calls', 'calls (mail included)', 'Calls out over time'), latencyChart(svc), more('#/services', 'Services →')),
      card('Outbox', stackedChart(svc, [{ key: 'outbox', cls: 'fp', label: 'deliveries' }], 'calls', 'calls out', 'Outbox deliveries over time'),
        levelChart(services.outbox, 'waiting')),
      card('Mail', stackedChart(mail, [
        { key: 'ok', cls: 's2', label: 'sent' }, { key: 'failed', cls: 's5', label: 'failed' }], 'calls', 'mails', 'Mail over time'),
        more('#/services?service=mailer', 'The mailer →'))),
  ];
}

const views = {
  async overview() {
    if (focus()) {
      const tenant = focus()[0];
      const [traffic, changes] = await Promise.all([
        api('traffic', { window: state.window, tenant }), api('changes', { tenant, limit: 8 })]);
      return [
        h('div', { class: 'bar' }, h('h1', {}, 'Overview of ' + tenant), h('span', { class: 'muted' }, '·'), windowPicker(render)),
        focusNote('The host\'s health and heat are left out.'),
        ...await keyGraphs(tenant, traffic),
        h('div', { class: 'grid' },
          wideCard('Newest changes', changes.changes.length ? changesTable(changes.changes) : h('p', { class: 'muted' }, 'No changes recorded yet.'),
            h('p', {}, h('a', { href: '#/changes' }, 'All changes →')))),
      ];
    }
    const [health, traffic, hot, changes] = await Promise.all([
      api('host_health'), api('traffic', { window: state.window }), api('hot_boxes', { window: state.window }),
      api('changes', { limit: 8 })]);
    const graphs = await keyGraphs('', traffic);
    const pts = traffic.series.points;
    const total = pts.reduce((a, p) => a + p.n, 0), errs = pts.reduce((a, p) => a + p.n5, 0);
    const notes = health.startup_notes || [];
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Overview'), h('span', { class: 'muted' }, '·'), windowPicker(render)),
      notes.length ? h('div', { class: 'notice bad' }, notes.map((n) => h('div', {}, n))) : null,
      h('div', { class: 'grid' },
        card('Host', h('div', { class: 'stats' },
          stat(health.build, 'build'), stat(fmt.dur(health.uptime_ms), 'up'), stat(fmt.n(health.boxes), 'boxes'),
          stat(fmt.n(total), 'requests (' + state.window + ')'), stat(fmt.pct(errs, total), '5xx'),
          stat(health.mail, 'mail'), stat(health.backups && health.backups.enabled ? (health.backups.healthy === false ? 'failing' : 'on') : 'off', 'backups'),
          health.sandbox ? stat('yes', 'sandbox') : null)),
        card('Hot boxes', heatmap(hot.boxes))),
      ...graphs,
      h('div', { class: 'grid' },
        wideCard('Newest changes', changes.changes.length ? changesTable(changes.changes) : h('p', { class: 'muted' }, 'No changes recorded yet: the log starts with this version of the host.'), h('p', {}, h('a', { href: '#/changes' }, 'All changes →')))),
    ];
  },

  async traffic(params) {
    const tenant = params.get('tenant') || (focus() ? focus()[0] : ''), box = params.get('box') || '';
    const t = await api('traffic', { window: state.window, tenant, box });
    const tenants = state.me.tenants || [];
    const boxes = [...new Set(t.routes.map((r) => r.box).filter(Boolean))].sort();
    if (box && !boxes.includes(box)) boxes.push(box);
    const go = (k, v) => { params.set(k, v); if (!v) params.delete(k); location.hash = '#/traffic' + (params.toString() ? '?' + params : ''); };
    const pick = (label, values, value, key) => h('span', {}, h('label', {}, label + ' '),
      h('select', { onchange: (e) => go(key, e.target.value) },
        h('option', { value: '' }, 'all'), values.map((v) => h('option', { value: v, selected: v === value }, v))));
    const total = t.routes.reduce((a, r) => a + r.n, 0);
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Traffic'), windowPicker(render), pick('Tenant', tenants, tenant, 'tenant'), pick('Box', boxes, box, 'box')),
      h('div', { class: 'grid' }, wideCard('Requests', requestsChart(t.series)), wideCard('Latency', latencyChart(t.series))),
      wideCard(`Routes (${fmt.n(total)} requests)`, t.routes.length ? table([
        { label: 'Tenant', get: (r) => r.tenant || '–' },
        { label: 'Box', get: (r) => r.box ? h('a', { href: '#/box/' + encodeURIComponent(r.box) }, r.box) : h('span', { class: 'muted' }, 'host') },
        { label: 'Route', get: (r) => r.route, cls: () => 'mono' },
        { label: 'Requests', num: true, get: (r) => fmt.n(r.n) },
        { label: '4xx', num: true, get: (r) => r.n4 ? fmt.n(r.n4) : '', cls: (r) => r.n4 ? 'warn' : '' },
        { label: '5xx', num: true, get: (r) => r.n5 ? fmt.n(r.n5) + ' (' + fmt.pct(r.n5, r.n) + ')' : '', cls: (r) => r.n5 ? 'err' : '' },
        { label: 'p50', num: true, get: (r) => fmt.us(r.p50_us) },
        { label: 'p99', num: true, get: (r) => fmt.us(r.p99_us) },
        { label: 'Max', num: true, get: (r) => fmt.us(r.us_max) },
        { label: 'CPU', num: true, get: (r) => fmt.us(r.us) },
        { label: 'Fuel', num: true, get: (r) => r.fuel ? fmt.n(r.fuel) : '' },
        { label: 'SQL', num: true, get: (r) => r.sql ? fmt.n(r.sql) : '' },
        { label: 'Outbox', num: true, get: (r) => r.outbox ? fmt.n(r.outbox) : '' },
      ], t.routes) : h('p', { class: 'muted' }, 'No requests in this window.')),
    ];
  },

  // What visitors and clients got that wasn't a success (028 § 4.2), by who made it.
  async errors(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || (focus() ? focus()[0] : '');
    const e = await api('error_report', { window: state.window, tenant });
    const t = e.totals;
    const go = (v) => { if (v) params.set('tenant', v); else params.delete('tenant'); location.hash = '#/errors' + (params.toString() ? '?' + params : ''); };
    const by = { host: 'the host (no box ran)', box: 'a box answered', box_failed: 'a box failed' };
    const placeOf = (p) => p.box ? h('span', {}, h('a', { href: '#/box/' + encodeURIComponent(p.box) }, p.box), ' · ', h('span', { class: 'mono' }, p.route))
      : p.place.startsWith('(') ? h('span', { class: 'muted' }, p.place) : h('span', { class: 'mono' }, p.place);
    const logLink = (p) => h('a', { href: '#/logs?' + new URLSearchParams({ log: 'access', since: state.window === '12h' ? '24h' : state.window,
      pattern: '"status":' + p.status }) }, 'lines →');
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Errors'), windowPicker(render),
        h('select', { onchange: (ev) => go(ev.target.value) }, focus() ? null : h('option', { value: '' }, 'all tenants'),
          tenants.map((x) => h('option', { value: x, selected: x === tenant }, x)))),
      h('p', { class: 'muted small' }, 'What visitors and clients got that wasn\'t a success, from the access log. Calls between boxes and tries are left out; Box failures has why boxes failed.'),
      h('div', { class: 'stats' },
        stat(fmt.pct(t.n4 + t.n5, t.n), 'of requests failed'), stat(fmt.n(t.n4), '4xx'), stat(fmt.n(t.n5), '5xx'),
        stat(fmt.n(t.host), 'by the host'), stat(fmt.n(t.box), 'answered by boxes'), stat(fmt.n(t.box_failed), 'boxes failed')),
      wideCard('Errors over time', stackedChart(e, [
        { key: 'host', cls: 's3', label: 'by the host' }, { key: 'box', cls: 's4', label: 'answered by a box' },
        { key: 'box_failed', cls: 's5', label: 'a box failed' }], 'n', 'requests', 'Errors over time')),
      h('div', { class: 'grid' },
        card('By status', e.statuses.length ? table([
          { label: 'Status', get: (r) => String(r.status), cls: (r) => r.status >= 500 ? 'err' : 'warn' },
          { label: 'Count', num: true, get: (r) => fmt.n(r.n) },
          { label: 'Host', num: true, get: (r) => r.host ? fmt.n(r.host) : '' },
          { label: 'Box', num: true, get: (r) => r.box ? fmt.n(r.box) : '' },
          { label: 'Box failed', num: true, get: (r) => r.box_failed ? fmt.n(r.box_failed) : '' },
        ], e.statuses) : h('p', { class: 'muted' }, 'No errors in this window.')),
        card('Where visitors came from', table([
          { label: 'Came', get: (r) => r[0] },
          { label: 'Errors', num: true, get: (r) => fmt.n(r[1]) },
        ], [['by a link on this site', e.referred.same], ['from another site', e.referred.other], ['with no link (typed, a bot, an app)', e.referred.none]]),
          h('p', { class: 'muted small' }, 'An error behind a link on your own site is a broken link to fix.'))),
      wideCard('Where', e.places.length ? table([
        { label: 'Tenant', get: (p) => p.tenant || '–', cls: () => 'muted' },
        { label: 'Place', get: placeOf },
        { label: 'Status', get: (p) => String(p.status), cls: (p) => p.status >= 500 ? 'err' : 'warn' },
        { label: 'Made by', get: (p) => by[p.error_by] || p.error_by },
        { label: 'Count', num: true, get: (p) => fmt.n(p.n) },
        { label: 'From this site', num: true, get: (p) => p.same ? fmt.n(p.same) : '', cls: (p) => p.same ? 'warn' : '' },
        { label: 'Other site', num: true, get: (p) => p.other ? fmt.n(p.other) : '' },
        { label: 'No link', num: true, get: (p) => p.none ? fmt.n(p.none) : '' },
        { label: '', get: logLink },
      ], e.places) : h('p', { class: 'muted' }, 'No errors in this window.'),
        h('p', { class: 'muted small' }, '(not found) is every path nothing answers, counted together so a scan can\'t push out the rest; the access log has each one. Custom pages for these: error-message/404.html (site_guide).')),
    ];
  },

  // How boxes fail (028 § 4.3): every box call, whatever started it, and each way it went wrong.
  async failures(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || (focus() ? focus()[0] : ''), box = params.get('box') || '';
    const f = await api('box_failures', { window: state.window, tenant, box, latest: 100 });
    const go = (k, v) => { if (v) params.set(k, v); else params.delete(k); location.hash = '#/failures' + (params.toString() ? '?' + params : ''); };
    const boxes = [...new Set(f.routes.map((r) => r.box))].sort();
    if (box && !boxes.includes(box)) boxes.push(box);
    const pick = (label, values, value, key, all) => h('span', {}, h('label', {}, label + ' '),
      h('select', { onchange: (ev) => go(key, ev.target.value) }, all ? h('option', { value: '' }, 'all') : null,
        values.map((v) => h('option', { value: v, selected: v === value }, v))));
    const groupOf = (c) => f.groups[c] || '';
    const classList = (classes, group) => Object.entries(classes).filter(([c]) => groupOf(c) === group)
      .map(([c, n]) => c.replace(/_/g, ' ') + ' ' + fmt.n(n)).join(', ');
    const t = f.totals;
    const sum = (g) => Object.entries(t.classes).filter(([c]) => groupOf(c) === g).reduce((a, [, n]) => a + n, 0);
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Box failures'), windowPicker(render),
        pick('Tenant', tenants, tenant, 'tenant', !focus()), pick('Box', boxes, box, 'box', true)),
      h('p', { class: 'muted small' }, 'Every box call (requests, tools, timers, mail, jobs, calls between boxes; not tries or dry runs), from the host\'s own record. Failed calls are the call group; SQL counts statements, even in calls that went on to succeed.'),
      h('div', { class: 'stats' },
        stat(fmt.n(t.calls), 'calls'), stat(fmt.pct(t.failures, t.calls), 'failed'), stat(fmt.n(t.failures), 'failed calls'),
        stat(fmt.n(sum('sql')), 'SQL errors and refusals'), stat(fmt.n(sum('outbox')), 'deliveries gone wrong'),
        stat(fmt.n(sum('internal')), 'calls to boxes failed')),
      wideCard('Over time', stackedChart(f, [
        { key: 'failures', cls: 's5', label: 'failed calls' }, { key: 'sql', cls: 's4', label: 'SQL' },
        { key: 'outbox', cls: 'fp', label: 'deliveries' }, { key: 'internal', cls: 's3', label: 'calls to boxes' }],
        'calls', 'calls', 'Box failures over time')),
      wideCard('By route', f.routes.length ? table([
        { label: 'Tenant', get: (r) => r.tenant || '–', cls: () => 'muted' },
        { label: 'Box', get: (r) => h('a', { href: '#/box/' + encodeURIComponent(r.box) }, r.box) },
        { label: 'Route', get: (r) => r.route, cls: () => 'mono' },
        { label: 'Kind', get: (r) => r.kind, cls: () => 'muted' },
        { label: 'Calls', num: true, get: (r) => fmt.n(r.calls) },
        { label: 'Failed', num: true, get: (r) => r.failures ? fmt.n(r.failures) + ' (' + fmt.pct(r.failures, r.calls) + ')' : '', cls: (r) => r.failures ? 'err' : '' },
        { label: 'How', get: (r) => classList(r.classes, 'call') },
        { label: 'SQL', get: (r) => classList(r.classes, 'sql'), cls: () => 'warn' },
        { label: 'Other', get: (r) => ['outbox', 'internal', 'answer'].map((g) => classList(r.classes, g)).filter(Boolean).join(', '), cls: () => 'muted' },
      ], f.routes, (r) => go('box', r.box)) : h('p', { class: 'muted' }, 'No box calls in this window.')),
      wideCard('Latest', f.latest.length ? table([
        { label: 'When', get: (e) => fmt.time(e.at_ms), cls: () => 'muted' },
        { label: 'Box', get: (e) => h('a', { href: '#/box/' + encodeURIComponent(e.box) }, e.box) },
        { label: 'Version', get: (e) => e.version, cls: () => 'muted' },
        { label: 'Route', get: (e) => e.route + ' (' + e.kind + ')', cls: () => 'mono' },
        { label: 'Class', get: (e) => h('span', { class: 'pill ' + (groupOf(e.class) === 'call' ? 'failed' : '') }, e.class.replace(/_/g, ' ')) },
        { label: 'Message', get: (e) => clip(e.message, 160) + (e.n > 1 ? ' ×' + e.n : ''), cls: () => 'mono' },
        { label: '', get: (e) => e.request_id ? h('a', { href: '#/logs?' + new URLSearchParams({ log: 'boxes/' + e.box, pattern: e.request_id, since: '7d' }) }, 'log →') : '' },
      ], f.latest) : h('p', { class: 'muted' }, 'Nothing failed in this window.')),
      (f.tries || []).length ? wideCard('Tries and dry runs that failed (not counted above)', table([
        { label: 'When', get: (e) => fmt.time(e.at_ms), cls: () => 'muted' },
        { label: 'By', get: (e) => e.trial.replace(/_/g, ' '), cls: () => 'muted' },
        { label: 'Box', get: (e) => h('a', { href: '#/box/' + encodeURIComponent(e.box) }, e.box) },
        { label: 'Version', get: (e) => e.version, cls: () => 'muted' },
        { label: 'Route', get: (e) => e.route + ' (' + e.kind + ')', cls: () => 'mono' },
        { label: 'Class', get: (e) => e.class.replace(/_/g, ' ') },
        { label: 'Message', get: (e) => clip(e.message, 160) + (e.n > 1 ? ' ×' + e.n : ''), cls: () => 'mono' },
      ], f.tries)) : null,
    ];
  },

  // Per-domain controls (047 § 9): each domain's controls with their effective values and mode,
  // and for the one picked, what each control did and the verdicts. Look-only: changes go through
  // control_set and control_remove.
  async controls(params) {
    const all = await api('control_list', {});
    const domains = all.map((d) => d.domain);
    const withControls = all.filter((d) => d.controls.length || d.host_defaults);
    const domain = params.get('domain') || (withControls[0] || all[0] || {}).domain || '';
    const go = (k, v) => { if (v) params.set(k, v); else params.delete(k); location.hash = '#/controls' + (params.toString() ? '?' + params : ''); };
    const bar = h('div', { class: 'bar' }, h('h1', {}, 'Controls'), windowPicker(render),
      domains.length ? h('span', {}, h('label', {}, 'Domain '), h('select', { onchange: (ev) => go('domain', ev.target.value) },
        domains.map((d) => h('option', { value: d, selected: d === domain }, d)))) : null);
    const about = h('p', { class: 'muted small' }, 'Rate limits, concurrency, body size, timeouts, and closing, per domain, per API ' +
      '(a box, content, mcp, identity), and per endpoint, inside the host-wide per-client limit. An observed control refuses nothing ' +
      'and counts what it would have refused. Set them with control_set (start in observe), read control_report, then enforce.');
    if (!domain) return [bar, about, h('p', { class: 'muted' }, 'No domains.')];
    const row = all.find((d) => d.domain === domain) || { controls: [] };
    const report = await api('control_report', { domain, window: state.window });
    const byId = new Map(report.controls.map((c) => [c.id, c]));
    const valueText = (v) => v === null || v === undefined ? '–' : typeof v === 'object' ? JSON.stringify(v) : String(v);
    const props = ['rate', 'concurrent', 'body-max-bytes', 'timeout-ms', 'closed'];
    const effectiveTable = (c) => {
      const rows = props.filter((p) => c.effective.enforced[p] || c.effective.observed[p]).map((p) => ({ p, e: c.effective.enforced[p], o: c.effective.observed[p] }));
      return rows.length ? table([
        { label: 'Property', get: (r) => r.p, cls: () => 'mono' },
        { label: 'Enforced', get: (r) => r.e ? valueText(r.e.value) : '–', cls: () => 'mono' },
        { label: 'From', get: (r) => r.e ? r.e.from : '', cls: () => 'muted' },
        { label: 'Observed', get: (r) => r.o ? valueText(r.o.value) : '', cls: () => 'mono' },
        { label: 'From', get: (r) => r.o ? r.o.from : '', cls: () => 'muted' },
      ], rows) : h('p', { class: 'muted small' }, 'Names no property: it only watches what it matches.');
    };
    const brief = (b) => b ? fmt.n(b.requests) + ' requests, ' + fmt.n(b.refused) + ' refused, p99 ' + fmt.us(b.p99_us) +
      ', 5xx ' + fmt.pct(b.ratio_5xx, 1) : '–';
    const reportCard = (id, listed) => {
      const r = byId.get(id);
      const title = (listed && listed.match ? id + ' · ' + listed.match : id) + (listed && listed.api ? ' · ' + listed.api : '');
      const mode = listed ? listed.mode : 'removed';
      const kids = [h('p', {}, h('span', { class: 'pill' + (mode === 'enforce' ? ' ok' : '') }, mode), ' ',
        h('span', { class: 'muted small' }, listed ? listed.level + (listed.last_change ? ' · changed ' + fmt.ago(listed.last_change.at_ms) +
          ' by ' + listed.last_change.by + ': ' + listed.last_change.why : '') : 'removed'),
        listed && listed.in_flight ? h('span', { class: 'muted small' }, ' · ' + listed.in_flight + ' in flight') : null)];
      if (listed) kids.push(effectiveTable(listed));
      if (r) {
        const t = r.totals;
        kids.push(h('div', { class: 'stats' }, stat(fmt.n(t.matched), 'matched'), stat(fmt.n(t.refused), 'refused'),
          stat(fmt.pct(t.refused, t.matched), 'of matched'), stat(fmt.n(t.would), 'would refuse'),
          stat(t.clients_refused >= 1000 ? '1000+' : fmt.n(t.clients_refused), 'clients refused'),
          stat(fmt.pct(t.top_client_share, 1), 'from the busiest'), stat(fmt.n(t.peak), 'peak in flight'),
          stat(fmt.us(t.p99_us), 'p99 allowed')));
        if (r.verdicts.length) kids.push(h('div', { class: 'notice' }, r.verdicts.map((v) => h('p', {}, v))));
        const series = { ...r, points: r.points.map((p) => ({ ...p, n: p.allowed })) };
        kids.push(stackedChart(series, [
          { key: 'allowed', cls: 's2', label: 'allowed' }, { key: 'rate', cls: 's4', label: 'rate' },
          { key: 'concurrent', cls: 's5', label: 'concurrent' }, { key: 'body', cls: 's3', label: 'body' },
          { key: 'closed', cls: 'fp', label: 'closed' }], 'matched', 'matched', 'What ' + id + ' matched over time'));
        if (t.would || mode === 'observe') {
          kids.push(stackedChart(series, [{ key: 'would', cls: 's4', label: 'would refuse' }], 'matched', 'matched',
            'What ' + id + ' would have refused over time'));
        }
        kids.push(latencyChart(series));
        if (r.effect) {
          kids.push(h('p', { class: 'small' }, 'Since its last change (' + fmt.dur(r.effect.span_ms) + ' each side): before ',
            brief(r.effect.before), '; after ', brief(r.effect.after), '.', r.effect.note ? h('span', { class: 'muted' }, ' ' + r.effect.note) : null));
        }
      }
      return wideCard(title, kids);
    };
    const cards = [bar, about];
    if (row.host_defaults) {
      cards.push(h('p', { class: 'small' }, 'host.json controls.defaults (' + row.host_defaults.mode + '): ',
        h('span', { class: 'mono' }, JSON.stringify(row.host_defaults.set))));
    }
    if (!row.controls.length && !report.controls.length) cards.push(h('p', { class: 'muted' }, 'No controls on ' + domain + '.'));
    const seen = new Set();
    for (const c of row.controls) { seen.add(c.id); cards.push(reportCard(c.id, c)); }
    for (const r of report.controls) if (!seen.has(r.id)) cards.push(reportCard(r.id, null));
    // MCP's strict rate and fixed queue (047 § 13): this domain's /mcp, and the operator MCP.
    if (report.mcp_queue) cards.push(mcpQueueCard(report.mcp_queue, 'MCP queue · ' + domain + '/mcp'));
    try { cards.push(mcpQueueCard(await api('mcp_queue_report', { window: state.window }), 'MCP queue · the operator MCP')); } catch (e) { /* not this person's to see */ }
    return cards;
  },

  // What went out to services (028 § 4.4): the mailer, push, the robot, and every service boxes
  // call; one service's methods, statuses, and latest failures when one is picked.
  async services(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || (focus() ? focus()[0] : ''), service = params.get('service') || '';
    const args = { window: state.window, latest: 100 };
    if (tenant) args.tenant = tenant;
    if (service) args.service = service;
    const r = await api('service_report', args);
    const go = (k, v) => { if (v) params.set(k, v); else params.delete(k); location.hash = '#/services' + (params.toString() ? '?' + params : ''); };
    const t = r.totals, series = serviceSeries(r);
    const kinds = { mailer: 'the host\'s mailer', mail: 'a box\'s mail', web: 'the web (robot)', push: 'web push', external: 'external' };
    const fail = (x) => x.failed ? fmt.n(x.failed) + ' (' + fmt.pct(x.failed, x.calls) + ')' : '';
    const outboxNow = (r.outbox.now || []).filter((x) => !service || x.key === service);
    const logLink = (e) => e.request_id && e.service !== 'mailer'
      ? h('a', { href: '#/logs?' + new URLSearchParams({ log: 'egress', pattern: e.request_id, since: '7d' }) }, 'log →')
      : e.outbox_id ? h('a', { href: '#/logs?' + new URLSearchParams({ log: 'egress', pattern: '"outbox_id":' + e.outbox_id, since: '7d' }) }, 'log →') : '';
    const pick = h('select', { onchange: (ev) => go('tenant', ev.target.value) }, focus() ? null : h('option', { value: '' }, 'all tenants'),
      tenants.map((x) => h('option', { value: x, selected: x === tenant }, x)));
    const cards = [
      h('div', { class: 'bar' }, h('h1', {}, service ? 'Service: ' + service : 'Services'), windowPicker(render), pick,
        service ? h('a', { href: '#/services' + (tenant ? '?tenant=' + encodeURIComponent(tenant) : '') }, '← all services') : null),
      h('p', { class: 'muted small' }, 'Everything that went out: boxes\' calls through goober_call and the outbox, the host\'s push sends, ' +
        'the robot\'s fetches, token refreshes, and every mail the mailer sent. A failure is no answer, an error, or a status of 400 or more.'),
      h('div', { class: 'stats' },
        stat(fmt.n(t.calls), 'calls'), stat(fmt.pct(t.failed, t.calls), 'failed'), stat(fmt.n(t.n4), '4xx'), stat(fmt.n(t.n5), '5xx'),
        stat(fmt.n(t.neterr), 'no answer'), stat(fmt.n(t.outbox), 'outbox deliveries'), stat(fmt.us(t.p50_us), 'p50'),
        stat(fmt.us(t.p99_us), 'p99'), stat(fmt.bytes(t.bytes_out) + ' / ' + fmt.bytes(t.bytes_in), 'out / in')),
      h('div', { class: 'grid' },
        card('Calls', stackedChart(series, [{ key: 'ok', cls: 's2', label: 'answered' }, { key: 'failed', cls: 's5', label: 'failed' }],
          'calls', 'calls', 'Calls over time')),
        card('Latency', latencyChart(series))),
    ];
    if (!service) {
      cards.push(wideCard('By service', r.services.length ? table([
        { label: 'Service', get: (x) => x.service, cls: () => 'mono' },
        { label: 'Kind', get: (x) => kinds[x.kind] || x.kind, cls: () => 'muted' },
        { label: 'Calls', num: true, get: (x) => fmt.n(x.calls) },
        { label: 'Failed', num: true, get: fail, cls: (x) => x.failed ? 'err' : '' },
        { label: '4xx', num: true, get: (x) => x.n4 ? fmt.n(x.n4) : '' },
        { label: '5xx', num: true, get: (x) => x.n5 ? fmt.n(x.n5) : '' },
        { label: 'No answer', num: true, get: (x) => x.neterr ? fmt.n(x.neterr) : '' },
        { label: 'Retries', num: true, get: (x) => x.retries ? fmt.n(x.retries) : '', cls: (x) => x.retries ? 'warn' : '' },
        { label: 'Dead', num: true, get: (x) => x.dead ? fmt.n(x.dead) : '', cls: (x) => x.dead ? 'err' : '' },
        { label: 'p50', num: true, get: (x) => fmt.us(x.p50_us) },
        { label: 'p99', num: true, get: (x) => fmt.us(x.p99_us) },
        { label: 'Boxes', get: (x) => (x.boxes || []).join(', '), cls: () => 'muted' },
      ], r.services, (x) => go('service', x.service)) : h('p', { class: 'muted' }, 'Nothing went out in this window, and no box declares a service.')));
    }
    cards.push(wideCard(service ? 'By method and box' : 'By method', r.methods.length ? table([
      { label: 'Tenant', get: (m) => m.tenant || '–', cls: () => 'muted' },
      { label: 'Box', get: (m) => !m.box ? h('span', { class: 'muted' }, 'host') : m.box === 'host' || m.box === 'boxes' || m.box === 'operator'
        ? h('span', { class: 'muted' }, m.box) : h('a', { href: '#/box/' + encodeURIComponent(m.box) }, m.box) },
      { label: 'Service', get: (m) => m.service, cls: () => 'mono' },
      { label: 'Method', get: (m) => m.method, cls: () => 'mono' },
      { label: 'Calls', num: true, get: (m) => fmt.n(m.calls) },
      { label: 'Failed', num: true, get: fail, cls: (m) => m.failed ? 'err' : '' },
      { label: 'Outbox', num: true, get: (m) => m.outbox ? fmt.n(m.outbox) : '' },
      { label: 'p50', num: true, get: (m) => fmt.us(m.p50_us) },
      { label: 'p99', num: true, get: (m) => fmt.us(m.p99_us) },
    ], r.methods, service ? null : (m) => go('service', m.service)) : h('p', { class: 'muted' }, 'No calls in this window.')));
    cards.push(h('div', { class: 'grid' },
      card('Failures by status', r.statuses.length ? table([
        { label: 'Service', get: (x) => x.service, cls: () => 'mono' },
        { label: 'Status', get: (x) => x.status ? String(x.status) : 'no answer', cls: (x) => x.status >= 500 || !x.status ? 'err' : 'warn' },
        { label: 'Count', num: true, get: (x) => fmt.n(x.n) },
      ], r.statuses) : h('p', { class: 'muted' }, 'Nothing failed in this window.')),
      card('Outbox', levelChart(r.outbox, 'waiting'), outboxNow.length ? table([
        { label: 'Tenant', get: (x) => x.tenant },
        { label: 'Service', get: (x) => x.key, cls: () => 'mono' },
        { label: 'Waiting now', num: true, get: (x) => fmt.n(x.value), cls: (x) => x.value ? 'warn' : '' },
      ], outboxNow) : h('p', { class: 'muted small' }, 'Nothing is waiting.'),
        h('p', { class: 'muted small' }, 'Retries and dead letters are each box\'s: ', h('a', { href: '#/failures' }, 'Box failures →'), ' outbox_dead_letters lists what gave up.'))));
    cards.push(wideCard('Latest failures', r.latest.length ? table([
      { label: 'When', get: (e) => fmt.time(e.at_ms), cls: () => 'muted' },
      { label: 'Tenant', get: (e) => e.tenant || '–', cls: () => 'muted' },
      { label: 'Box', get: (e) => e.box || 'host' },
      { label: 'Call', get: (e) => e.service + '.' + e.method, cls: () => 'mono' },
      { label: 'Status', get: (e) => e.status ? String(e.status) : 'no answer', cls: (e) => e.status >= 500 || !e.status ? 'err' : 'warn' },
      { label: 'Error', get: (e) => clip(e.error || '', 140), cls: () => 'mono' },
      { label: 'URL', get: (e) => clip(e.url || '', 60), cls: () => 'mono muted' },
      { label: 'Request', get: (e) => e.request_id || (e.outbox_id ? 'outbox ' + e.outbox_id : ''), cls: () => 'mono muted' },
      { label: '', get: logLink },
    ], r.latest) : h('p', { class: 'muted' }, 'Nothing failed in this window.')));
    if (!service || service === 'mailer') {
      const m = r.mail;
      cards.push(h('div', { class: 'grid' },
        card('Mail', h('div', { class: 'stats' }, stat(m.mailer, 'mailer'), stat(fmt.n(m.daily_max), 'a tenant may send a day'),
          stat(fmt.n(m.per_address_daily), 'to one address a day'), stat(fmt.n(r.mail_in.calls), 'mail in'),
          stat(fmt.n(r.mail_in.n4 + r.mail_in.n5), 'mail in refused')),
          m.tenants.length ? table([
            { label: 'Tenant', get: (x) => x.tenant },
            { label: 'Sent, last 24 hours', num: true, get: (x) => fmt.n(x.sent_24h) },
            { label: 'Of the day\'s limit', num: true, get: (x) => fmt.pct(x.sent_24h, m.daily_max), cls: (x) => x.sent_24h >= 0.8 * m.daily_max ? 'warn' : '' },
          ], m.tenants) : h('p', { class: 'muted small' }, 'No box mail in the last 24 hours.'),
          h('p', { class: 'muted small' }, 'Bounces and complaints: ' + m.bounces + '.')),
        r.backups ? card('Backups', h('div', { class: 'stats' },
          stat(r.backups.enabled ? (r.backups.healthy === false ? 'failing' : 'on') : 'off', 'backups'),
          stat(r.backups.last_nightly_at || '–', 'last nightly'), stat(r.backups.last_increment_at || '–', 'last increment')),
          r.backups.last_error ? h('div', { class: 'notice bad' }, r.backups.last_error) : null,
          r.backups.note ? h('p', { class: 'muted small' }, r.backups.note) : null) : null));
    }
    return cards;
  },

  async flow() {
    if (focus()) return [h('h1', {}, 'Data flow'), focusNote('The whole host\'s graph is left out; open a box for its own.'), h('p', {}, h('a', { href: '#/boxes' }, 'Boxes →'))];
    const g = await api('box_graph', { window: state.window });
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Data flow'), windowPicker(render)),
      wideCard('', h('p', { class: 'muted small' },
        'Tenants on the left, then their pages, host routes, and boxes, then the databases, services, and boxes those reach, and ' +
        'each database\'s tables and the mailer on the right: the letters are what boxes may do there (read, insert, update, delete), and a ' +
        'dashed table edge is a grant no statement has used since the host started. Services show their calls, failures, and p99; ' +
        'the host\'s own calls out (push, the robot, token refreshes) come from its routes, and every mail goes through the mailer. ' +
        'Width is traffic; a box\'s bar is its share of CPU time; red edges fail 5% or more. Click a box or a service to open it.'),
      flowGraph(g, (n) => openNode(n))),
    ];
  },

  async boxes() {
    const [list, hot] = await Promise.all([api('box_list'), api('hot_boxes', { window: state.window }).catch(() => ({ boxes: [] }))]);
    const heat = Object.fromEntries(hot.boxes.map((b) => [b.box, b]));
    const routeCount = (b) => (b.routes || []).length + (b.tools || []).length + (b.timers || []).length +
      (b.mail || []).length + (b.internal || []).length;
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Boxes'), windowPicker(render)),
      wideCard('', table([
        { label: 'Box', get: (b) => h('a', { href: '#/box/' + encodeURIComponent(b.box) }, b.box) },
        { label: 'Version', get: (b) => h('code', {}, (b.version || '').slice(0, 12)) },
        { label: 'Domains', get: (b) => (b.domains || []).join(', ') },
        { label: 'Routes', num: true, get: (b) => routeCount(b) },
        { label: 'Requests', num: true, get: (b) => fmt.n((heat[b.box] || {}).n) },
        { label: '5xx', num: true, get: (b) => (heat[b.box] || {}).n5 ? fmt.n(heat[b.box].n5) : '', cls: (b) => (heat[b.box] || {}).n5 ? 'err' : '' },
        { label: 'CPU', num: true, get: (b) => fmt.us((heat[b.box] || {}).us) },
        { label: 'Heat', num: true, get: (b) => fmt.pct((heat[b.box] || {}).share || 0, 1) },
        { label: 'Mission', get: (b) => clip(b.mission, 90), cls: () => 'muted' },
      ], list, (b) => { location.hash = '#/box/' + encodeURIComponent(b.box); })),
    ];
  },

  async box(params, name) {
    const [graph, versions, logs, changes, sourceHistory, grants] = await Promise.all([
      api('box_graph', { window: state.window, box: name }), api('version_list', { box: name }),
      api('box_logs', { box: name, limit: 30 }).catch(() => []),
      api('changes', { kind: 'deploy', limit: 200, tenant: focus() ? focus()[0] : '' }).catch(() => ({ changes: [] })),
      api('source_box_history', { box: name, limit: 20 }).catch(() => ({ deploys: [] })),
      api('grant_usage', { box: name }).catch(() => [])]);
    const active = versions.find((v) => v.active) || versions[0] || {};
    const manifest = active.version ? await api('version_get', { box: name, version: active.version }) : null;
    const jobs = manifest && manifest.manifest.routes && manifest.manifest.routes.job ? await jobsOf(name) : [];
    const flags = manifest && manifest.manifest.flags ? await flagsOf(name) : [];
    const deploys = changes.changes.filter((c) => c.target === name);
    const deployedAt = Object.fromEntries(deploys.map((c) => [c.detail.version, c.at_ms]));
    const routes = graph.nodes.filter((n) => n.kind === 'route');
    return [
      h('div', { class: 'bar' }, h('h1', {}, name), h('code', { class: 'muted' }, (active.version || '').slice(0, 12)), windowPicker(render),
        h('a', { href: '#/traffic?box=' + encodeURIComponent(name) }, 'Traffic →')),
      manifest && manifest.manifest.mission ? h('p', { class: 'muted' }, manifest.manifest.mission) : null,
      wideCard('Its graph', flowGraph(graph, (n) => { if (n.label !== name) openNode(n); })),
      grantsCard(grants[0]),
      jobsCard(jobs),
      flagsCard(flags),
      h('div', { class: 'grid' },
        wideCard('Routes', table([
          { label: 'Protocol', get: (r) => r.protocol },
          { label: 'Route', get: (r) => r.label, cls: () => 'mono' },
          { label: 'Name', get: (r) => r.route, cls: () => 'mono muted' },
          { label: 'Requests', num: true, get: (r) => fmt.n(r.n) },
          { label: '5xx', num: true, get: (r) => r.n5 ? fmt.n(r.n5) : '', cls: (r) => r.n5 ? 'err' : '' },
          { label: 'p50', num: true, get: (r) => fmt.us(r.p50_us) },
          { label: 'p99', num: true, get: (r) => fmt.us(r.p99_us) },
          { label: 'Fuel', num: true, get: (r) => r.fuel ? fmt.n(r.fuel) : '' },
          { label: 'SQL', num: true, get: (r) => r.sql ? fmt.n(r.sql) : '' },
        ], routes))),
      h('div', { class: 'grid' },
        card('Versions', table([
          { label: 'Version', get: (v) => h('code', {}, v.version.slice(0, 16)) },
          { label: '', get: (v) => v.active ? h('span', { class: 'pill deploy' }, 'active')
            : (v.state === 'archived' || v.state === 'deleted') ? h('span', { class: 'pill' }, v.state) : '' },
          // Keeping (045 § 3.1): why it's kept, or when it will be archived or deleted.
          { label: 'Kept', get: (v) => v.kept ? 'kept' : v.archives_at ? 'archives ' + v.archives_at.slice(0, 10)
            : v.deletes_at ? 'deletes ' + v.deletes_at.slice(0, 10) : '', cls: () => 'muted' },
          { label: 'Activated', get: (v) => deployedAt[v.version] ? fmt.time(deployedAt[v.version]) : '', cls: () => 'muted' },
          { label: 'Source', get: (v) => v.source ? h('a', { href: '#/project/' + encodeURIComponent(v.source.project) + '?rev=' + v.source.rev },
            v.source.project + ' r' + v.source.rev) : '' },
        ], versions)),
        card('Deploys', deploys.length ? changesTable(deploys.slice(0, 10)) : h('p', { class: 'muted' }, 'None recorded yet.'))),
      wideCard('Newest log lines', logs.length ? table([
        { label: 'At', get: (l) => (l.at || '').replace('T', ' ').slice(0, 19), cls: () => 'muted mono' },
        { label: 'Level', get: (l) => String(l.level) },
        { label: 'Text', get: (l) => l.text, cls: () => 'mono' },
      ], logs.slice().reverse()) : h('p', { class: 'muted' }, 'No log lines.')),
      manifest ? wideCard('Manifest', h('details', {}, h('summary', {}, 'Show the active manifest'),
        h('pre', { class: 'json' }, JSON.stringify(manifest.manifest, null, 2)))) : null,
      sourceCard(sourceHistory),
    ];
  },

  // ---- source (020): projects, revisions, files, and the builds and deploys of each

  async source() {
    const [projects, jobs] = await Promise.all([api('source_list'), api('source_job', { limit: 20 })]);
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Source')),
      wideCard('Projects', projects.length ? table([
        { label: 'Project', get: (p) => h('a', { href: '#/project/' + encodeURIComponent(p.project) }, p.project) },
        { label: 'Head', num: true, get: (p) => p.head },
        { label: 'Deployed', num: true, get: (p) => p.deployed_rev ?? '–' },
        { label: 'Updated', get: (p) => fmt.time(Date.parse(p.updated_at)), cls: () => 'muted' },
        { label: 'In the bucket', get: (p) => mirrorText(p.mirror), cls: (p) => p.mirror && p.mirror.pending ? 'warn' : 'muted' },
        { label: 'About', get: (p) => clip(p.description, 90), cls: () => 'muted' },
      ], projects, (p) => { location.hash = '#/project/' + encodeURIComponent(p.project); })
        : h('p', { class: 'muted' }, 'No source projects yet: an agent starts one with source_create.')),
      wideCard('Builds and deploys', jobsTable(jobs, true)),
    ];
  },

  async project(params, name) {
    const rev = Number(params.get('rev') || 0), file = params.get('file');
    if (file) return fileView(name, rev, file);
    if (rev) return revisionView(name, rev);
    const [projects, log, jobs] = await Promise.all([
      api('source_list'), api('source_log', { project: name, limit: 100 }), api('source_job', { project: name, limit: 50 })]);
    const p = projects.find((x) => x.project === name) || {};
    const builds = {};
    for (const j of jobs) (builds[j.rev] = builds[j.rev] || []).push(j);
    return [
      h('div', { class: 'bar' }, h('h1', {}, name), h('a', { href: '#/source' }, '← Source'),
        p.head ? h('a', { href: '#/project/' + encodeURIComponent(name) + '?rev=' + p.head }, 'Files at the head →') : null),
      p.description ? h('p', { class: 'muted' }, p.description) : null,
      h('div', { class: 'grid' }, card('', h('div', { class: 'stats' },
        stat(p.head, 'revisions'), stat(p.deployed_rev ?? '–', 'deployed'), stat(mirrorText(p.mirror), 'in the backup bucket')))),
      wideCard('Revisions', log.length ? table([
        { label: 'Rev', num: true, get: (r) => h('a', { href: '#/project/' + encodeURIComponent(name) + '?rev=' + r.rev }, String(r.rev)) },
        { label: '', get: (r) => r.rev === p.deployed_rev ? h('span', { class: 'pill deploy' }, 'deployed') : '' },
        { label: 'Message', get: (r) => clip(r.message.split('\n')[0], 100) },
        { label: 'By', get: (r) => r.by, cls: () => 'mono muted' },
        { label: 'When', get: (r) => fmt.time(Date.parse(r.at)), cls: () => 'muted' },
        { label: 'Files', get: (r) => clip(r.changed.join(', '), 80), cls: () => 'mono muted' },
        { label: 'Builds', get: (r) => (builds[r.rev] || []).map((j) => jobPill(j)) },
      ], log, (r) => { location.hash = '#/project/' + encodeURIComponent(name) + '?rev=' + r.rev; })
        : h('p', { class: 'muted' }, 'Nothing committed yet.')),
      wideCard('Builds and deploys', jobsTable(jobs, false)),
    ];
  },

  async job(params, id) {
    const j = await api('source_job', { job: Number(id), verbose: true });
    const done = j.state === 'succeeded' || j.state === 'failed';
    return [
      h('div', { class: 'bar' }, h('h1', {}, (j.kind === 'deploy' ? 'Deploy ' : 'Build ') + j.job),
        jobPill(j), h('a', { href: '#/project/' + encodeURIComponent(j.project) + '?rev=' + j.rev }, j.project + ' revision ' + j.rev + ' →')),
      h('div', { class: 'grid' }, card('', h('div', { class: 'stats' },
        stat(j.by, 'by'), stat(j.queued_at ? fmt.time(Date.parse(j.queued_at)) : '–', 'queued'),
        stat(j.started_at && j.finished_at ? fmt.us(1000 * (Date.parse(j.finished_at) - Date.parse(j.started_at))) : '–', 'took')))),
      (j.boxes || []).length ? wideCard('Boxes', table([
        { label: 'Box', get: (b) => h('a', { href: '#/box/' + encodeURIComponent(b.box) }, b.box) },
        { label: 'Version', get: (b) => b.version ? h('code', {}, String(b.version).slice(0, 16)) : '' },
        { label: 'Bytes', num: true, get: (b) => b.bytes ? fmt.bytes(b.bytes) : '' },
        { label: 'Warnings', get: (b) => (b.warnings || []).join('; '), cls: () => 'muted' },
      ], j.boxes)) : null,
      wideCard(done ? 'Build log' : 'Build log (so far; this page refreshes)', h('pre', { class: 'source log' }, j.log || '(empty)')),
    ];
  },

  async changes(params) {
    const kind = params.get('kind') || '', tenant = params.get('tenant') || (focus() ? focus()[0] : ''), tab = params.get('tab') || 'changes';
    const before = Number(params.get('before') || 0);
    const go = (k, v) => { if (v) params.set(k, v); else params.delete(k); params.delete('before'); location.hash = '#/changes?' + params; };
    const tabs = focus() ? null : segmented(['changes', 'actions'], tab, (t) => go('tab', t === 'changes' ? '' : t));
    if (tab === 'actions') {
      const changedOnly = params.get('changed') === '1';
      const a = await api('operator_actions', { changed_only: changedOnly, before_id: before, limit: 100 });
      return [
        h('div', { class: 'bar' }, h('h1', {}, 'Change log'), tabs,
          h('label', {}, h('input', { type: 'checkbox', checked: changedOnly, onchange: (e) => go('changed', e.target.checked ? '1' : '') }), ' only calls that changed something')),
        wideCard('Operator actions', a.actions.length ? table([
          { label: 'When', get: (r) => fmt.time(r.at_ms), cls: () => 'muted' },
          { label: 'Operator', get: (r) => r.operator, cls: () => 'mono' },
          { label: 'Tool', get: (r) => r.tool, cls: () => 'mono' },
          { label: 'Arguments', get: (r) => Object.entries(r.args || {}).map(([k, v]) => k + '=' + v).join(' '), cls: () => 'mono muted' },
          { label: 'Result', get: (r) => r.failed ? h('span', { class: 'pill failed' }, 'failed') : '' },
          { label: 'Changes', get: (r) => r.changes.length ? h('span', {}, r.changes.map((id, i) => [i ? ' ' : '', h('a', { href: '#/change/' + id }, '#' + id)])) : '' },
        ], a.actions) : h('p', { class: 'muted' }, 'No operator calls recorded yet.'),
        pager(a.actions, before, (id) => { params.set('before', id); location.hash = '#/changes?' + params; })),
      ];
    }
    const c = await api('changes', { kind, tenant, before_id: before, limit: 100 });
    const kinds = ['deploy', 'upload', 'content', 'partial', 'template', 'schema', 'flag', 'control', 'attachment', 'other'];
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Change log'), tabs,
        h('label', {}, 'Kind '), h('select', { onchange: (e) => go('kind', e.target.value) }, h('option', { value: '' }, 'all'),
          kinds.map((k) => h('option', { value: k, selected: k === kind }, k))),
        h('label', {}, 'Tenant '), h('select', { onchange: (e) => go('tenant', e.target.value) }, h('option', { value: '' }, 'all'),
          [...(state.me.tenants || []), ...(focus() ? [] : ['_host'])].map((t) => h('option', { value: t, selected: t === tenant }, t)))),
      wideCard('', c.changes.length ? changesTable(c.changes) : h('p', { class: 'muted' }, 'No changes recorded yet. The log starts with this version of the host.'),
        pager(c.changes, before, (id) => { params.set('before', id); location.hash = '#/changes?' + params; })),
    ];
  },

  async change(params, id) {
    const c = await api('change_get', { id: Number(id) });
    const rows = [
      ['When', fmt.time(c.at_ms) + ' (' + fmt.ago(c.at_ms) + ')'], ['Kind', c.kind + ' · ' + c.action],
      ['Tenant', c.tenant || '–'], ['Target', c.target], ['Who', c.actor], ['Request', c.request_id || '–'],
      ['Size', (c.bytes_before == null ? '–' : fmt.bytes(c.bytes_before)) + ' → ' + (c.bytes_after == null ? '–' : fmt.bytes(c.bytes_after))],
    ];
    for (const [k, v] of Object.entries(c.detail || {})) rows.push([k, String(v)]);
    let body;
    if (c.kind === 'deploy' || c.kind === 'upload') {
      const d = manifestDiff(c.manifest_before, c.manifest_after);
      body = !c.manifest_after ? h('p', { class: 'muted' }, 'The manifest is no longer stored.')
        : !d ? h('p', { class: 'muted' }, 'The manifests could not be read.')
          : !c.manifest_before ? h('details', { open: true }, h('summary', {}, 'The first version: its whole manifest'), h('pre', { class: 'json' }, JSON.stringify(JSON.parse(c.manifest_after), null, 2)))
            : d.length ? table([
              { label: 'Field', get: (r) => r.path, cls: () => 'mono' },
              { label: 'Before', get: (r) => r.before ?? '(none)', cls: (r) => 'mono ' + (r.before === undefined ? 'muted' : '') },
              { label: 'After', get: (r) => r.after ?? '(removed)', cls: (r) => 'mono ' + (r.after === undefined ? 'muted' : '') },
            ], d) : h('p', { class: 'muted' }, 'The manifests are the same: only the module changed.');
    } else if (c.has_text) {
      body = diffView(c.before, c.after);
    } else {
      body = h('p', { class: 'muted' }, c.kind === 'attachment' ? 'Attachments are measured, not compared.'
        : 'The text is not kept (it is larger than 64 KiB, binary, or older than 90 days).');
    }
    return [
      h('div', { class: 'bar' }, h('a', { href: '#/changes' }, '← Change log'), h('h1', {}, 'Change #' + c.id)),
      h('div', { class: 'grid' }, card('', h('dl', { class: 'kv' }, rows.map(([k, v]) => [h('dt', {}, k), h('dd', {}, v)])))),
      wideCard(c.kind === 'deploy' || c.kind === 'upload' ? 'Manifest changes' : 'Diff', body),
    ];
  },

  async operators() {
    const o = await api('operators');
    const tenants = state.me.all_tenants || state.me.tenants || [];
    const status = h('p', { class: 'muted small' });
    const editor = (op) => {
      const editable = o.may_set_any || op.you;
      const current = op.scope === 'all' ? null : op.scope.tenants;
      const all = h('input', { type: 'checkbox', checked: !current, disabled: !o.may_set_any });
      const boxes = tenants.map((t) => h('input', { type: 'checkbox', value: t, checked: !current || current.includes(t) }));
      const save = h('button', { class: 'small', onclick: async () => {
        const picked = boxes.filter((b) => b.checked).map((b) => b.value);
        const next = all.checked ? 'all' : picked;
        const words = next === 'all' ? 'every tenant' : picked.join(', ') || 'nothing';
        if (!confirm('Focus ' + op.name + ' on ' + words + '?')) return;
        try {
          await api('write/scope', { operator: op.key, tenants: next, confirm: true });
          render(false);
        } catch (e) { status.textContent = e.message; }
      } }, 'Save');
      if (!editable) return h('span', { class: 'muted' }, current ? current.join(', ') : 'all tenants');
      return h('span', { class: 'scope' }, h('label', {}, all, ' all'), ' ',
        tenants.map((t, i) => h('label', {}, boxes[i], ' ' + t + ' ')), save);
    };
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Operators')),
      wideCard('', h('p', { class: 'muted small' },
        'Each operator acts on all tenants, or only those checked: their agents too. Only a person here changes it; ' +
        (o.may_set_any ? 'as an owner, you set anyone\'s.' : 'you may narrow your own.')),
        table([
          { label: 'Operator', get: (op) => op.name + (op.you ? ' (you)' : ''), cls: () => 'mono' },
          { label: 'Kind', get: (op) => op.kind },
          { label: 'Roles', get: (op) => op.roles.join(', '), cls: () => 'muted' },
          { label: 'Scope', get: (op) => editor(op) },
          { label: 'Set', get: (op) => op.scope === 'all' ? '' : op.scope.set_by + ' · ' + fmt.time(op.scope.at_ms), cls: () => 'muted small' },
        ], o.operators), status),
      await approvalsCard(),
      await deletionsCard(tenants),
      wideCard('Lift a mail block', unblockForm(tenants)),
    ];
  },

  // The whole site as an icicle (a flame graph laid flat): width is traffic, or structure; color is p99.
  async sitemap(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || tenants[0] || '';
    const mode = params.get('mode') || 'traffic';
    const zoom = params.get('path') || '/';
    const win = params.get('window') || '1h';
    const go = (k, v) => { params.set(k, v); location.hash = '#/sitemap?' + params; };
    const tree = await api('uri_tree', { tenant, window: win });
    const find = (n, path) => {
      if (n.path === path) return n;
      for (const k of n.children || []) { const f = find(k, path); if (f) return f; }
      return null;
    };
    const root = find(tree, zoom) || tree;
    const detail = h('div', { class: 'card' }, h('p', { class: 'muted' }, 'Click a segment to see its numbers and flow; double-click to zoom in.'));
    const eps = Math.max(1, root.total * 0.004);
    const leaves = (n) => (n.children || []).length ? n.children.reduce((a, k) => a + leaves(k), 0) : 1;
    const weight = (n) => mode === 'traffic' ? n.total + eps * leaves(n) : leaves(n);
    const color = (n) => {
      if (!n.total) return '#9aa0a6';
      const ms = n.p99_us / 1000;
      if (ms < 100) return '#3a9a5b';
      if (ms < 300) return '#9bbf3b';
      if (ms < 1000) return '#d99a1e';
      return '#c9372c';
    };
    const W = 1000, RH = 24;
    let depth = 0;
    const rects = [];
    const lay = (n, x, w, d) => {
      depth = Math.max(depth, d + 1);
      rects.push({ n, x, w, d });
      let cx = x;
      const total = (n.children || []).reduce((a, k) => a + weight(k), 0) || 1;
      for (const k of n.children || []) {
        const kw = w * weight(k) / total;
        lay(k, cx, kw, d + 1);
        cx += kw;
      }
    };
    lay(root, 0, W, 0);
    const show = async (n) => {
      clear(detail);
      append(detail, [h('h2', {}, n.path), h('div', { class: 'stats' }, stat(n.kind, 'kind'), stat(fmt.n(n.total), 'requests (' + win + ')'),
        stat(fmt.us(n.p99_us), 'p99'), stat(fmt.n(n.total_5xx), '5xx'))]);
      try {
        const f = await api('uri_flow', { tenant, path: n.path, window: win });
        const list = (title, rows, key) => rows.length ? [h('h3', {}, title), h('ul', {}, rows.slice(0, 12).map((r) =>
          h('li', {}, h('code', {}, r[key]), ' · ' + fmt.n(r.n))))] : null;
        append(detail, [list('Came from', f.in, 'from'), list('Went next', f.out, 'to'),
          !f.in.length && !f.out.length ? h('p', { class: 'muted' }, 'No same-site flow through it in this window.') : null]);
      } catch (e) { append(detail, [errorBox(e)]); }
      append(detail, [h('p', {}, h('a', { href: '#/perf?' + new URLSearchParams({ tenant, prefix: n.path }) }, 'Its paths in Performance →'))]);
    };
    const svg = s('svg', { viewBox: `0 0 ${W} ${depth * RH}`, class: 'icicle', role: 'img' },
      rects.filter((r) => r.w >= 0.6).map((r) => {
        const g = s('g', { class: 'seg-node' },
          s('rect', { x: r.x + 0.5, y: r.d * RH + 0.5, width: Math.max(r.w - 1, 0.5), height: RH - 1, rx: 3, fill: color(r.n),
            stroke: r.n.total_5xx ? '#c9372c' : 'none', 'stroke-width': r.n.total_5xx ? 2 : 0 }),
          r.w > 40 ? s('text', { x: r.x + 6, y: r.d * RH + 16, class: 'seg-label' }, clip(r.n.name, Math.floor(r.w / 7))) : null,
          s('title', {}, `${r.n.path}\n${r.n.kind} · ${r.n.total} requests · p99 ${fmt.us(r.n.p99_us)}`));
        g.addEventListener('click', () => show(r.n));
        g.addEventListener('dblclick', () => go('path', r.n.path));
        return g;
      }));
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Sitemap'),
        h('select', { onchange: (e) => { params.delete('path'); go('tenant', e.target.value); } }, tenants.map((t) => h('option', { value: t, selected: t === tenant }, t))),
        segmented(['traffic', 'structure'], mode, (m) => go('mode', m)),
        segmented(['5m', '15m', '1h'], win, (w) => go('window', w)),
        zoom !== '/' ? h('a', { href: '#/sitemap?' + new URLSearchParams({ tenant, mode, window: win }) }, '↑ the whole site') : null),
      wideCard('', h('p', { class: 'muted small' }, 'Each row is one more path segment. Width is ' +
        (mode === 'traffic' ? 'requests in the window (pages nobody visited keep a sliver)' : 'the number of pages and files under it') +
        '. Green is a p99 under 100 ms, then yellow, orange, and red past a second; gray had no traffic; a red edge had 5xx. ' +
        'Pages, files, and box routes show even unvisited.'), svg),
      detail,
    ];
  },

  async perf(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || (focus() ? focus()[0] : '');
    const win = params.get('window') || '15m', sort = params.get('sort') || 'p99', prefix = params.get('prefix') || '';
    const go = (k, v) => { if (v) params.set(k, v); else params.delete(k); location.hash = '#/perf?' + params; };
    const ssort = params.get('ssort') || 'total';
    const [rows, report, statements] = await Promise.all([
      api('uri_stats', { tenant, window: win, sort, prefix, limit: 300 }),
      api('perf_report', { tenant, window: win === '15m' || win === '5m' ? win : '1h' }).catch(() => ({ routes: [], findings: [] })),
      api('perf_statements', { tenant, sort: ssort, limit: 30 }).catch(() => [])]);
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Performance'),
        h('select', { onchange: (e) => go('tenant', e.target.value) }, focus() ? null : h('option', { value: '' }, 'all tenants'),
          tenants.map((t) => h('option', { value: t, selected: t === tenant }, t))),
        segmented(['5m', '15m', '1h'], win, (w) => go('window', w)), h('label', {}, 'Sort '), segmented(['p99', 'count', 'errors'], sort, (x) => go('sort', x)),
        prefix ? h('span', { class: 'pill' }, 'under ' + prefix, ' ', h('a', { href: '#/perf?' + new URLSearchParams({ tenant, window: win, sort }) }, '×')) : null),
      wideCard('', h('p', { class: 'muted small' }, 'Measured in memory as requests happen, so it is current to the second; it starts over when the host restarts. History is in Traffic.'),
        rows.length ? table([
          { label: 'Tenant', get: (r) => r.tenant, cls: () => 'muted' },
          { label: 'Path', get: (r) => r.path, cls: () => 'mono' },
          { label: 'Box · route', get: (r) => r.box ? r.box + ' · ' + (r.route || '') : '', cls: () => 'muted' },
          { label: 'Requests', num: true, get: (r) => fmt.n(r.n) },
          { label: '4xx', num: true, get: (r) => r.n4 ? fmt.n(r.n4) : '', cls: (r) => r.n4 ? 'warn' : '' },
          { label: '5xx', num: true, get: (r) => r.n5 ? fmt.n(r.n5) : '', cls: (r) => r.n5 ? 'err' : '' },
          { label: 'p50', num: true, get: (r) => fmt.us(r.p50_us) },
          { label: 'p95', num: true, get: (r) => fmt.us(r.p95_us) },
          { label: 'p99', num: true, get: (r) => fmt.us(r.p99_us), cls: (r) => r.p99_us > 1e6 ? 'err' : r.p99_us > 3e5 ? 'warn' : '' },
          { label: 'Max', num: true, get: (r) => fmt.us(r.max_us) },
        ], rows) : h('p', { class: 'muted' }, 'No requests in this window yet.')),
      perfFindingsCard(report.findings),
      perfRoutesCard(report.routes),
      perfStatementsCard(statements, ssort, (x) => go('ssort', x)),
    ];
  },

  async agents(params) {
    const win = params.get('window') || '24h';
    const a = await api('agent_audit', { window: win });
    const spark = (vals) => {
      const max = Math.max(1, ...vals), W = 160, H = 26, step = W / Math.max(vals.length, 1);
      return s('svg', { viewBox: `0 0 ${W} ${H}`, class: 'spark' },
        vals.map((v, i) => s('rect', { x: i * step + 0.5, y: H - (H - 2) * v / max, width: Math.max(step - 1, 1), height: (H - 2) * v / max, fill: 'var(--accent)' })));
    };
    const flagText = (f) => {
      const { flag, ...rest } = f;
      return flag + ' (' + Object.entries(rest).map(([k, v]) => k.replace(/_/g, ' ') + ' ' + v).join(', ') + ')';
    };
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Agents'), segmented(['1h', '24h', '7d'], win, (w) => { params.set('window', w); location.hash = '#/agents?' + params; })),
      wideCard('', h('p', { class: 'muted small' }, 'Every operator, token, and site agent: what it called, wrote, deleted, and failed, from the audit log. ' +
        'Flags: burst (more than 60 calls in 5 minutes or 30 writes in 10), failing (over a quarter of 20+ calls), deleting (more than 10 in an hour), looping (the same call 10+ times in 10 minutes).'),
        a.actors.length ? table([
          { label: 'Who', get: (r) => r.actor, cls: () => 'mono' },
          { label: 'Activity', get: (r) => spark(r.spark) },
          { label: 'Calls', num: true, get: (r) => fmt.n(r.calls) },
          { label: 'Writes', num: true, get: (r) => fmt.n(r.writes) },
          { label: 'Deletes', num: true, get: (r) => r.deletes ? fmt.n(r.deletes) : '', cls: (r) => r.deletes ? 'warn' : '' },
          { label: 'Failed', num: true, get: (r) => r.failed ? fmt.n(r.failed) : '', cls: (r) => r.failed ? 'err' : '' },
          { label: 'Peak 5m', num: true, get: (r) => fmt.n(r.peak_5m) },
          { label: 'Flags', get: (r) => r.flags.length ? h('span', {}, r.flags.map((f) => h('span', { class: 'pill failed', title: flagText(f) }, f.flag))) : h('span', { class: 'muted' }, 'ok') },
          { label: 'Last', get: (r) => fmt.ago(Date.parse(r.last)), cls: () => 'muted' },
        ], a.actors, (r) => { location.hash = '#/logs?' + new URLSearchParams({ log: 'audit', pattern: '"actor":"' + r.actor + '"', since: win }); })
          : h('p', { class: 'muted' }, 'No agent or operator activity in this window.')),
    ];
  },

  async content(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || tenants[0] || '';
    const tab = params.get('tab') || 'templates', item = params.get('item') || '';
    const go = (o) => { location.hash = '#/content?' + new URLSearchParams({ tenant, tab, ...o }); };
    let names = [], source = null, extra = null;
    if (tab === 'templates') {
      const t = await api('template_list', { tenant });
      names = [...new Set([...(t.templates || []), ...Object.keys(t.defaults || {})])].sort();
      if (item) { const g = await api('template_get', { tenant, name: item }); source = g.template; extra = 'from ' + g.source; }
    } else if (tab === 'partials') {
      const t = await api('partial_list', { tenant });
      names = (t.partials || []).map((p) => typeof p === 'string' ? p : p.name);
      if (item) { const g = await api('partial_get', { tenant, name: item }); source = g.html ?? g.text ?? JSON.stringify(g, null, 2); }
    } else {
      const pages = await api('page_list', { tenant });
      names = pages.map((p) => p.url);
      if (item) {
        const [g, r] = await Promise.all([api('page_get', { tenant, path: item }), api('page_render', { tenant, path: item, drafts: true })]);
        source = g.body;
        extra = [g.kind + ' · ' + g.file + ' · etag ' + g.etag + ' · renders ' + r.status + ' · ' + fmt.bytes(r.bytes),
          (r.problems || []).length ? ' · ' + r.problems.join('; ') : ' · no problems'].join('');
      }
    }
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Content'),
        h('select', { onchange: (e) => { location.hash = '#/content?' + new URLSearchParams({ tenant: e.target.value, tab }); } },
          tenants.map((t) => h('option', { value: t, selected: t === tenant }, t))),
        segmented(['templates', 'partials', 'pages'], tab, (t) => { location.hash = '#/content?' + new URLSearchParams({ tenant, tab: t }); })),
      h('div', { class: 'db' },
        h('div', { class: 'card list' }, names.length ? names.map((n) => h('a', { href: '#/content?' + new URLSearchParams({ tenant, tab, item: n }), class: n === item ? 'on' : '' }, n))
          : h('p', { class: 'muted' }, 'None.')),
        h('div', { class: 'card' }, item ? [h('h2', {}, item), extra ? h('p', { class: 'muted small' }, extra) : null,
          h('pre', { class: 'source' }, source ?? '')] : h('p', { class: 'muted' }, 'Pick one to read its source. Look-only: agents change these with template_put, partial_put, and page_put.'))),
    ];
  },

  // Heat studies (048 § 8): the list, a running study's progress, and a finished one's heat by
  // endpoint, its clients, and the controls it suggests. Look-only: studies start and stop
  // through the operator tools.
  async studies(params) {
    const id = params.get('id');
    if (!id) {
      const rows = await api('heat_study_list', {});
      return [
        h('h1', {}, 'Heat studies'),
        h('p', { class: 'muted small' }, 'A study counts one domain\'s requests by endpoint for 5 minutes to a day, then keeps a report ' +
          '30 days: where the heat is, and the controls it suggests, in observe mode. An agent or operator starts one with ' +
          'heat_study_start {domain, minutes, why}; this panel only reads them.'),
        wideCard('', rows.length ? table([
          { label: 'Study', get: (r) => '#' + r.id, cls: () => 'mono' },
          { label: 'Domain', get: (r) => r.domain },
          { label: 'State', get: (r) => h('span', { class: 'pill' + (r.state === 'running' ? ' deploy' : r.state === 'interrupted' ? ' failed' : '') },
            r.state + (r.state === 'running' ? ' · ' + fmt.dur(r.remaining_ms || 0) + ' left' : '')) },
          { label: 'Started', get: (r) => fmt.time(r.started_ms), cls: () => 'muted' },
          { label: 'Minutes', num: true, get: (r) => String(r.minutes) },
          { label: 'Requests', num: true, get: (r) => fmt.n(r.requests) },
          { label: 'Hottest', get: (r) => r.hottest ? r.hottest.endpoint + ' · ' + fmt.pct(r.hottest.heat_share, 1) : '–', cls: () => 'mono' },
          { label: 'By', get: (r) => r.by, cls: () => 'muted' },
          { label: 'Why', get: (r) => clip(r.why, 60), cls: () => 'muted' },
        ], rows, (r) => { location.hash = '#/studies?id=' + r.id; }) : h('p', { class: 'muted' }, 'No studies yet, or none in the last 30 days.')),
      ];
    }
    const r = await api('heat_study_get', { id: Number(id) });
    const by = params.get('by') || 'heat';
    const go = (k, x) => { params.set(k, x); location.hash = '#/studies?' + params; };
    const t = r.totals, eps = r.endpoints;
    const running = r.state === 'running';
    const points = r.per_minute.series.map((n, i) => ({ t: r.started_ms + i * 60e3, n }));
    const series = { points, step_ms: 60e3, from_ms: r.started_ms, to_ms: r.started_ms + points.length * 60e3 };
    const shareOf = { heat: (e) => e.heat_share, requests: (e) => e.requests_share, bytes: (e) => e.bytes_share };
    const ranked = eps.slice().sort((a, b) => shareOf[by](b) - shareOf[by](a)).slice(0, 20);
    const detailed = eps.filter((e) => e.clients);
    const suggested = detailed.filter((e) => e.suggest && e.suggest.control_set);
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Heat study #' + r.id + ' · ' + r.domain),
        h('span', { class: 'pill' + (running ? ' deploy' : '') }, r.state), h('a', { href: '#/studies' }, '← every study')),
      h('p', { class: 'muted small' }, (r.why ? 'Why: ' + r.why + '. ' : '') + 'Started ' + fmt.time(r.started_ms) + ' by ' + r.by + ' for ' +
        r.minutes + ' minutes' + (running ? '; ' + fmt.dur(r.remaining_ms || 0) + ' left, numbers so far.' :
          '; ended ' + fmt.time(r.ended_ms) + ' (' + String(r.ended_by).replace('_', ' ') + ').')),
      h('div', { class: 'stats' },
        stat(fmt.n(t.requests), 'requests'), stat(fmt.us(t.heat_us), 'heat (time serving)'),
        stat(fmt.bytes(t.bytes_in + t.bytes_out), 'bytes in and out'), stat(fmt.n(r.per_minute.peak), 'busiest minute'),
        stat((r.clients.exact ? '' : '≈') + fmt.n(r.clients.distinct), 'clients'), stat(String(r.concurrent.peak), 'most at once'),
        stat(fmt.us(t.latency_us.p99), 'p99 latency')),
      h('div', { class: 'grid' },
        card('Requests per minute', points.length ? stackedChart(series, [{ key: 'n', cls: 's2', label: 'requests' }], 'n', 'requests',
          'Requests per minute') : h('p', { class: 'muted' }, 'Nothing yet.')),
        card('Each client\'s busiest minute',
          h('div', { class: 'stats' }, stat(String(r.clients.busiest_minute.p50), 'p50'), stat(String(r.clients.busiest_minute.p99), 'p99'),
            stat(String(r.clients.busiest_minute.max), 'max'), stat(String(r.concurrent.per_client_peak), 'most at once, one client')),
          r.top_clients.length ? table([
            { label: 'Client (keyed hash)', get: (c) => c.client_h, cls: () => 'mono' },
            { label: 'Class', get: (c) => c.class, cls: () => 'muted' },
            { label: 'Requests', num: true, get: (c) => fmt.n(c.requests) },
            { label: 'Busiest minute', num: true, get: (c) => String(c.busiest_minute) },
          ], r.top_clients) : null,
          h('p', { class: 'muted small' }, 'A client is a keyed hash of its address (an IPv6 /64 is one); log_search finds its lines. ' +
            'No address or user id is kept.'))),
      wideCard('Where the heat is', h('div', { class: 'bar' }, h('label', {}, 'By '), segmented(['heat', 'requests', 'bytes'], by, (x) => go('by', x))),
        ranked.length ? shareBars(ranked, shareOf[by]) : h('p', { class: 'muted' }, 'No requests yet.'),
        r.tracked.other_requests ? h('p', { class: 'muted small' }, fmt.n(r.tracked.other_requests) + ' requests past the ' + r.tracked.cap +
          ' endpoints tracked are in "other".') : null),
      wideCard('Endpoints', detailed.length ? table([
        { label: 'Endpoint', get: (e) => e.endpoint, cls: () => 'mono' },
        { label: 'API', get: (e) => e.api, cls: () => 'muted' },
        { label: 'Requests', num: true, get: (e) => fmt.n(e.requests) },
        { label: 'Heat', num: true, get: (e) => fmt.pct(e.heat_share, 1) },
        { label: 'p99', num: true, get: (e) => fmt.us(e.latency_us.p99) },
        { label: 'Peak/min', num: true, get: (e) => String(e.per_minute.peak) },
        { label: 'Clients', num: true, get: (e) => (e.clients.exact ? '' : '≈') + fmt.n(e.clients.distinct) },
        { label: 'Client p99/min', num: true, get: (e) => String(e.clients.busiest_minute.p99) },
        { label: 'At once', num: true, get: (e) => String(e.concurrent.peak) },
        { label: 'Out', num: true, get: (e) => fmt.bytes(e.bytes.out) },
        { label: '4xx', num: true, get: (e) => e.status['4xx'] ? fmt.n(e.status['4xx']) : '', cls: (e) => e.status['4xx'] ? 'warn' : '' },
        { label: '5xx', num: true, get: (e) => e.status['5xx'] ? fmt.n(e.status['5xx']) : '', cls: (e) => e.status['5xx'] ? 'err' : '' },
        { label: '429', num: true, get: (e) => e.status['429'] ? fmt.n(e.status['429']) : '' },
        { label: 'Bots', num: true, get: (e) => e.classes.bot ? fmt.pct(e.classes.bot, e.requests) : '' },
      ], detailed) : h('p', { class: 'muted' }, 'No requests yet.')),
      wideCard('Suggested controls', h('p', { class: 'muted small' }, 'Each is ready-to-send control_set arguments in observe mode: ' +
        'send it, read control_report while it observes, and enforce once it would have refused only what it should. This panel only looks.'),
        suggested.length ? suggested.map((e) => h('div', { class: 'suggest' },
          h('h3', { class: 'mono' }, e.endpoint, ' ', h('span', { class: 'muted small' }, e.api)),
          h('pre', { class: 'source' }, JSON.stringify(e.suggest.control_set, null, 2)),
          h('ul', { class: 'small' }, Object.entries(e.suggest.reasons).map(([k, v]) => h('li', {}, h('b', {}, k), ': ', v)))))
          : h('p', { class: 'muted' }, 'No endpoint has enough requests to suggest from yet (20 at least).')),
    ];
  },

  // How fast the logs grow and what backups put in the bucket (032 § 10, 019 § 9): each log's
  // bytes over the window, the warnings, who writes the most, and uploads per day by stream.
  async volume(params) {
    if (focus()) return [h('h1', {}, 'Logs and uploads'), focusNote('Logs and backups are the whole host\'s.')];
    const log = params.get('log') || '';
    const [v, b] = await Promise.all([
      api('log_velocity', log ? { window: state.window, log } : { window: state.window }),
      api('backup_status', {}).catch(() => ({ enabled: false })),
    ]);
    const go = (k, x) => { if (x) params.set(k, x); else params.delete(k); location.hash = '#/volume' + (params.toString() ? '?' + params : ''); };
    const classes = ['s2', 's3', 's4', 's5', 'fp'];
    // The four biggest logs over the window, and the rest as one.
    const sum = (a) => a.reduce((x, y) => x + y, 0);
    const ranked = v.series.slice().sort((a, c) => sum(c.points) - sum(a.points));
    const shown = ranked.slice(0, 4), rest = ranked.slice(4);
    const parts = shown.map((x, i) => ({ key: x.log, cls: classes[i], label: x.log }));
    if (rest.length) parts.push({ key: '(others)', cls: classes[4], label: rest.length + ' more' });
    const count = ranked.length ? ranked[0].points.length : 0;
    const points = [];
    for (let i = 0; i < count; i++) {
      const p = { t: v.from_ms + i * v.step_ms, all: 0 };
      for (const x of shown) { p[x.log] = x.points[i]; p.all += x.points[i]; }
      if (rest.length) { p['(others)'] = sum(rest.map((x) => x.points[i])); p.all += p['(others)']; }
      points.push(p);
    }
    const disk = v.disk || {};
    const cards = [
      h('div', { class: 'bar' }, h('h1', {}, log ? 'Log: ' + log : 'Logs and uploads'), windowPicker(render),
        log ? h('a', { href: '#/volume' }, '← every log') : null),
      h('p', { class: 'muted small' }, 'Every line the host writes, counted by log and by the tenant, box, and route it names; five-minute ' +
        'counts for a week, a total per day for 90 days. A log warns when its last 24 hours are three times its usual day (the median ' +
        'of the seven before) and at least 1 MiB.'),
      v.warnings.length ? h('div', { class: 'notice' }, v.warnings.map((w) => h('p', {}, w.text))) : null,
      log ? null : h('div', { class: 'stats' },
        stat(fmt.bytes(disk.per_day_bytes), 'written in 24 hours'), stat(fmt.bytes(disk.on_disk_bytes), 'on the machine'),
        stat(fmt.bytes(disk.archive_queued_bytes), 'waiting to upload'), stat(fmt.bytes(disk.projected_bytes), 'in 7 days at this rate'),
        stat(fmt.pct(disk.projected_bytes, disk.budget_bytes), 'of limits.logs-bytes (' + fmt.bytes(disk.budget_bytes) + ')')),
      h('div', { class: 'grid' },
        card('Log volume', count ? stackedChart({ points, step_ms: v.step_ms, from_ms: v.from_ms, to_ms: v.to_ms }, parts, 'all', 'written',
          'Log bytes over time', fmt.bytes) : h('p', { class: 'muted' }, 'Nothing counted yet.'))),
      wideCard('By log', v.logs.length ? table([
        { label: 'Log', get: (l) => l.log, cls: () => 'mono' },
        { label: 'Last 24 h', num: true, get: (l) => fmt.bytes(l.last_24h_bytes) },
        { label: 'Lines', num: true, get: (l) => fmt.n(l.last_24h_lines) },
        { label: 'Today', num: true, get: (l) => fmt.bytes(l.today_bytes) },
        { label: 'Usual day', num: true, get: (l) => l.median_day_bytes === null ? '–' : fmt.bytes(l.median_day_bytes) },
        { label: 'Growth', num: true, get: (l) => l.growth === null ? '–' : l.growth + '×', cls: (l) => l.grew ? 'err' : '' },
        { label: 'Days known', num: true, get: (l) => String(l.days_known) },
      ], v.logs, log ? null : (l) => go('log', l.log)) : h('p', { class: 'muted' }, 'No log lines counted yet.')),
      wideCard('Who writes the most', v.top.length ? table([
        { label: 'Log', get: (t) => t.log, cls: () => 'mono' },
        { label: 'Tenant', get: (t) => t.tenant || '–', cls: () => 'muted' },
        { label: 'Box', get: (t) => t.box ? h('a', { href: '#/box/' + encodeURIComponent(t.box) }, t.box) : h('span', { class: 'muted' }, 'host') },
        { label: 'Route', get: (t) => t.route || '–', cls: () => 'mono' },
        { label: 'Bytes', num: true, get: (t) => fmt.bytes(t.bytes) },
        { label: 'Lines', num: true, get: (t) => fmt.n(t.lines) },
        { label: 'Of its log', num: true, get: (t) => fmt.pct(t.share, 1) },
      ], v.top) : h('p', { class: 'muted' }, 'Nothing in this window.')),
    ];
    // Uploads (019 § 9).
    const up = b.uploads;
    if (!b.enabled || !up) {
      cards.push(wideCard('Uploads', h('p', { class: 'muted' }, 'Backups are off, so nothing goes up. ' + (b.note || ''))));
      return cards;
    }
    const streams = ['nightly', 'increments', 'logs', 'assets', 'source'];
    const days = new Map();
    for (const d of up.days) {
      if (!days.has(d.day)) days.set(d.day, { t: Date.parse(d.day + 'T00:00:00Z'), all: 0 });
      const p = days.get(d.day);
      p[d.stream] = (p[d.stream] || 0) + d.bytes;
      p.all += d.bytes;
    }
    const today = new Date(), dayPoints = [];
    for (let i = 29; i >= 0; i--) {
      const t = Date.UTC(today.getUTCFullYear(), today.getUTCMonth(), today.getUTCDate()) - i * 86400e3;
      dayPoints.push(days.get(new Date(t).toISOString().slice(0, 10)) || { t, all: 0 });
    }
    const daySeries = { points: dayPoints, step_ms: 86400e3, from_ms: dayPoints[0].t, to_ms: dayPoints[29].t + 86400e3 };
    cards.push(h('div', { class: 'stats' },
      stat(fmt.bytes(up.today_bytes), 'uploaded today (UTC)'),
      stat(up.budget_bytes ? fmt.bytes(up.budget_bytes) : 'none', 'limits.upload-bytes-per-day'),
      stat(up.budget_bytes ? fmt.pct(up.today_bytes, up.budget_bytes) : '–', 'of the budget'),
      stat(up.deferring ? 'yes' : 'no', 'logs and increments waiting')));
    if (up.note) cards.push(h('div', { class: 'notice' }, h('p', {}, up.note)));
    cards.push(h('div', { class: 'grid' },
      card('Uploads per day', stackedChart(daySeries, streams.map((x, i) => ({ key: x, cls: classes[i], label: x })), 'all', 'uploaded',
        'Upload bytes per day', fmt.bytes),
        h('p', { class: 'muted small' }, 'Past the budget, log archives, the source mirror, and increments wait for the next UTC day ' +
          '(a log archive waiting a week goes anyway); the nightly and attachments never wait.')),
      card('In the bucket', up.bucket.length ? table([
        { label: 'Prefix', get: (x) => x.prefix + '/', cls: () => 'mono' },
        { label: 'Objects', num: true, get: (x) => fmt.n(x.objects) },
        { label: 'Bytes', num: true, get: (x) => fmt.bytes(x.bytes) },
        { label: 'Listed', get: (x) => x.at, cls: () => 'muted' },
      ], up.bucket) : h('p', { class: 'muted' }, 'Not listed yet: the first nightly\'s rotation lists it.'))));
    return cards;
  },

  async logs(params) {
    const st = await api('log_status', {}).catch(() => ({ logs: [], archive: {} }));
    const log = params.get('log') || 'all', pattern = params.get('pattern') || '', regex = params.get('regex') === '1';
    const email = params.get('email') || '', since = params.get('since') || '24h', before = params.get('before') || '';
    const q = h('input', { type: 'text', value: pattern, placeholder: regex ? 'regex, like "status":5[0-9][0-9]' : 'text to find, like a request id' });
    const em = h('input', { type: 'email', value: email, placeholder: 'an address (logs keep hashes)' });
    const pick = h('select', {}, ['all', ...st.logs.map((l) => l.log)].map((n) => h('option', { value: n, selected: n === log }, n)));
    const rx = h('input', { type: 'checkbox', checked: regex });
    const submit = (extra) => {
      const p = new URLSearchParams({ log: pick.value, pattern: q.value, since, ...(rx.checked ? { regex: '1' } : {}), ...(em.value ? { email: em.value } : {}), ...(extra || {}) });
      location.hash = '#/logs?' + p;
    };
    let out = null;
    if (params.toString()) {
      const args = { log, since, limit: 200 };
      if (pattern) args[regex ? 'regex' : 'pattern'] = pattern;
      if (email) args.email = email;
      if (before) args.before = before;
      try {
        const r = await api('log_search', args);
        out = [h('p', { class: 'muted small' }, `${r.count} line${r.count === 1 ? '' : 's'}, newest first · ${fmt.bytes(r.bytes_read)} read`),
          h('div', { class: 'loglines' }, r.lines.map((l) => {
            let o = {};
            try { o = JSON.parse(l); } catch { /* shown raw */ }
            const row = h('div', { class: 'logline' }, h('span', { class: 'muted' }, (o.at || '').replace('T', ' ').slice(0, 19)), ' ',
              h('span', { class: 'pill' }, o.log || ''), ' ', h('code', {}, l));
            if (o.request_id) row.addEventListener('click', () => submit({ log: 'all', pattern: o.request_id, regex: '' }));
            return row;
          })),
          r.next ? h('p', {}, h('a', { href: '#/logs?' + new URLSearchParams({ log, pattern, since, ...(regex ? { regex: '1' } : {}), ...(email ? { email } : {}), before: r.next }) }, 'Older →')) : null];
      } catch (e) { out = [errorBox(e)]; }
    }
    return [
      h('div', { class: 'bar' }, h('h1', {}, 'Logs'), segmented(['1h', '24h', '3d', '7d'], since, (w) => { params.set('since', w); params.delete('before'); location.hash = '#/logs?' + params; })),
      wideCard('', h('form', { class: 'logform', onsubmit: (e) => { e.preventDefault(); submit(); } },
        pick, q, h('label', {}, rx, ' regex'), em, h('button', {}, 'Search')),
        h('p', { class: 'muted small' }, 'Every line is chained to the one before it (log_verify), rotated daily, kept 7 days here, and archived sealed ' +
          (st.archive.enabled ? 'to the backup bucket for 400 days' : '— but backups are off, so nothing is archived') +
          '. Click a line to see everything with its request id. Searches are themselves audited.'),
        out),
    ];
  },

  async db(params) {
    const tenants = state.me.tenants || [];
    const tenant = params.get('tenant') || tenants[0] || '';
    const database = params.get('database') || '', tableName = params.get('table') || '';
    const offset = Number(params.get('offset') || 0);
    const go = (o) => {
      const p = new URLSearchParams();
      for (const [k, v] of Object.entries(o)) if (v || v === 0) p.set(k, v);
      location.hash = '#/db?' + p;
    };
    const list = tenant ? await api('db_list', { tenant }) : [];
    const dbs = (list[0] && list[0].databases) || [];
    const side = h('div', { class: 'card list' },
      h('div', { class: 'head' }, 'Tenant'),
      h('select', { onchange: (e) => go({ tenant: e.target.value }) }, tenants.map((t) => h('option', { value: t, selected: t === tenant }, t))),
      dbs.map((d) => [
        h('div', { class: 'head' }, d.database + (d.exists ? ' · ' + fmt.bytes(d.bytes) : ' · not created')),
        (d.tables || []).map((t) => h('a', { href: '#/db?' + new URLSearchParams({ tenant, database: d.database, table: t.table }),
          class: d.database === database && t.table === tableName ? 'on' : '' }, `${t.table} (${fmt.n(t.rows)})`)),
      ]),
      dbs.length ? null : h('p', { class: 'muted small' }, 'No databases for this tenant.'));
    const right = [];
    const sqlBox = h('textarea', { spellcheck: 'false', placeholder: 'One read-only statement, like SELECT * FROM orders WHERE status = \'open\'' });
    const sqlOut = h('div', {});
    const sqlDb = database || (dbs[0] && dbs[0].database) || '';
    if (database && tableName) {
      const desc = await api('db_describe', { tenant, database });
      const t = (desc.tables || []).find((x) => x.table === tableName);
      const quoted = '"' + tableName.replace(/"/g, '""') + '"';
      const rows = await api('db_query', { tenant, database, sql: `SELECT * FROM ${quoted} LIMIT 50 OFFSET ${offset}`, limit: 50 });
      sqlBox.value = `SELECT * FROM ${quoted} LIMIT 50`;
      right.push(card(`${database} · ${tableName}`,
        t ? h('p', { class: 'muted small' }, `${fmt.n(t.rows)} rows · schema ${(desc.schemas || []).map((x) => x.schema).join(', ')}`) : null,
        t ? h('details', {}, h('summary', {}, 'Columns'), table([
          { label: 'Column', get: (c) => c.name, cls: () => 'mono' },
          { label: 'Declared', get: (c) => c.declared || '', cls: () => 'mono' },
          { label: 'SQL', get: (c) => c.sql_type, cls: () => 'muted' },
          { label: '', get: (c) => (c.primary_key ? 'primary key ' : '') + (c.nullable ? 'nullable' : '') + (c.references ? '→ ' + c.references : '') },
          { label: 'Description', get: (c) => c.description || '' },
          { label: 'Schema', get: (c) => c.schema || '', cls: () => 'muted' },
        ], t.columns || [])) : null,
        rowsTable(rows),
        h('div', { class: 'pager' },
          h('button', { type: 'button', disabled: offset <= 0, onclick: () => go({ tenant, database, table: tableName, offset: Math.max(0, offset - 50) }) }, '← Newer'),
          h('span', { class: 'muted small' }, `rows ${offset + 1}–${offset + rows.rows.length}`),
          h('button', { type: 'button', disabled: rows.rows.length < 50, onclick: () => go({ tenant, database, table: tableName, offset: offset + 50 }) }, 'More →'))));
    }
    if (sqlDb) {
      const dbPick = h('select', {}, dbs.map((d) => h('option', { value: d.database, selected: d.database === sqlDb }, d.database)));
      const run = async () => {
        clear(sqlOut);
        try { sqlOut.appendChild(rowsTable(await api('db_query', { tenant, database: dbPick.value, sql: sqlBox.value, limit: 500 }))); }
        catch (e) { sqlOut.appendChild(errorBox(e)); }
      };
      sqlBox.addEventListener('keydown', (e) => { if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) run(); });
      right.push(card('Query', h('div', { class: 'bar' }, h('label', {}, 'Database '), dbPick,
        h('button', { type: 'button', class: 'run', onclick: run }, 'Run'), h('span', { class: 'muted small' }, 'read-only · Ctrl+Enter')), sqlBox, sqlOut));
    }
    if (tenant) {
      const [unbound, snaps] = await Promise.all([api('db_unbound', { tenant }).catch(() => null),
        api('db_snapshots', { tenant }).catch(() => ({ snapshots: [] }))]);
      if (unbound) right.push(dropsCard(tenant, unbound, snaps.snapshots || []));
    }
    return [h('h1', {}, 'Database'), h('div', { class: 'cols' }, side, h('div', {}, right.length ? right : h('p', { class: 'muted' }, 'Pick a table.')))];
  },
};

// What no box binds, the snapshots, and the drops only a person makes (008 § 11). Each drop
// asks for the name typed out; the host refuses while a box binds it and snapshots first.
function dropsCard(tenant, unbound, snapshots) {
  const owner = (state.me.roles || []).includes('host:owner');
  const out = h('p', { class: 'muted small' });
  const ask = (args, target, what, said) => async () => {
    const typed = prompt(`${what} ${target}?\n${said || 'The host takes a snapshot first.'} Type ${target} to go ahead.`);
    if (typed === null) return;
    const why = prompt('Why? (kept in the change log)') || '';
    try {
      const r = await api('write/drop', Object.assign({ tenant, confirm: typed, why }, args));
      out.textContent = r.retired ? `Retired ${r.retired}.` : r.deleted ? `Deleted ${r.deleted}.`
        : r.approved ? `Approved ${r.approved}: it applies as boxes name it, each database snapshotted first.`
        : `Dropped ${r.dropped}. Its snapshot: ${r.snapshot}.`;
      render(true);
    } catch (e) { out.textContent = e.message; }
  };
  const button = (label, onclick) => owner ? h('button', { type: 'button', class: 'small', onclick }, label) : null;
  const rows = [];
  for (const d of unbound.databases || []) {
    if (!d.bound) rows.push({ what: 'database', name: d.database, rows: '', act: button('Drop', ask({ kind: 'database', database: d.database }, d.database, 'Drop database')) });
    for (const t of d.unbound_tables || []) {
      const target = d.database + '.' + t.table;
      rows.push({ what: 'table', name: target, rows: fmt.n(t.rows), act: button('Drop', ask({ kind: 'table', database: d.database, table: t.table }, target, 'Drop table')) });
    }
  }
  for (const s of unbound.unnamed_schemas || []) {
    rows.push({ what: 'schema', name: s, rows: '', act: button('Retire', ask({ kind: 'schema', schema: s }, s, 'Retire schema')) });
  }
  // Schemas whose step drops or rewrites data wait for a person (041 § 6): what each would do.
  const held = (unbound.held_schemas || []).map((s) => {
    const says = (s.step.changes || []).filter((c) => c.kind !== 'expansion' && c.kind !== 'indexing').map((c) => c.says);
    const lines = says.concat(s.step.consequences || []);
    return h('div', { class: 'small' },
      h('p', {}, h('b', { class: 'mono' }, s.schema), ` follows ${s.step.from}. `,
        button('Approve', ask({ kind: 'schema_approve', schema: s.schema }, s.schema, 'Approve schema',
          lines.join('\n') + '\nEach database is snapshotted before it changes.')) || ''),
      h('ul', {}, lines.map((l) => h('li', {}, l))));
  });
  return card('Unbound, and snapshots',
    held.length ? h('details', { open: true }, h('summary', {}, `Schemas waiting for a person (${held.length})`), held) : null,
    h('p', { class: 'muted small' }, 'What no box binds (active or disabled), which a person may drop or retire here, and the database snapshots. ' +
      'Agents can only point at these (db_unbound); dropping is yours. ' + (owner ? '' : 'Only host:owner drops.')),
    rows.length ? table([
      { label: 'Kind', get: (r) => r.what, cls: () => 'muted' },
      { label: 'Name', get: (r) => r.name, cls: () => 'mono' },
      { label: 'Rows', num: true, get: (r) => r.rows },
      { label: '', get: (r) => r.act || '' },
    ], rows) : h('p', { class: 'muted small' }, 'Every database and table is bound by a box.'),
    snapshots.length ? h('details', {}, h('summary', {}, `Snapshots (${snapshots.length})`), table([
      { label: 'Snapshot', get: (s) => s.snapshot, cls: () => 'mono' },
      { label: 'By', get: (s) => s.by, cls: () => 'muted' },
      { label: 'Note', get: (s) => clip(s.note || '', 80) },
      { label: 'Size', num: true, get: (s) => fmt.bytes(s.bytes) },
      { label: '', get: (s) => s.before_drop ? 'before a drop' : '', cls: () => 'muted' },
      { label: '', get: (s) => button('Delete', ask({ kind: 'snapshot', snapshot: s.snapshot }, s.snapshot, 'Delete snapshot')) || '' },
    ], snapshots)) : null,
    out);
}

// Horizontal bars of each row's share (0 to 1), labeled: a heat study's endpoints (048 § 8).
function shareBars(rows, share) {
  const W = 900, row = 22, L = 330, R = 60, H = rows.length * row + 4;
  const max = Math.max(0.0001, ...rows.map(share));
  const g = s('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Share by endpoint', class: 'hb' });
  rows.forEach((e, i) => {
    const y = 2 + i * row, w = Math.max(1, (W - L - R) * share(e) / max);
    g.appendChild(s('text', { x: L - 8, y: y + 15, 'text-anchor': 'end', class: 'label' }, clip(e.endpoint, 44)));
    g.appendChild(s('rect', { class: 'track', x: L, y: y + 3, width: W - L - R, height: row - 8 }));
    const bar = s('rect', { class: 'fill h' + heatClass(share(e)), x: L, y: y + 3, width: w, height: row - 8 });
    bar.appendChild(s('title', {}, `${e.endpoint} (${e.api}): ${fmt.pct(share(e), 1)}`));
    g.appendChild(bar);
    g.appendChild(s('text', { x: L + w + 6, y: y + 15, class: 'label' }, fmt.pct(share(e), 1)));
  });
  return g;
}

function stat(v, label) { return h('div', { class: 'stat' }, h('b', {}, String(v ?? '–')), h('span', {}, label)); }

function heatmap(boxes) {
  if (!boxes.length) return h('p', { class: 'muted' }, 'No boxes.');
  return h('div', { class: 'heat' }, boxes.map((b) => h('a', { class: 'tile h' + heatClass(b.share), href: '#/box/' + encodeURIComponent(b.box),
    title: `${b.box}: ${fmt.pct(b.share, 1)} of CPU time` },
    h('b', {}, b.box), h('span', {}, `${fmt.n(b.n)} req · ${fmt.us(b.us)}` + (b.n5 ? ` · ${b.n5} 5xx` : '')))));
}

function changesTable(rows) {
  return table([
    { label: 'When', get: (c) => h('span', { title: fmt.time(c.at_ms) }, fmt.ago(c.at_ms)), cls: () => 'muted' },
    { label: 'Kind', get: (c) => h('span', { class: 'pill ' + c.kind + (c.action === 'delete' ? ' delete' : '') }, c.kind + (c.action === 'delete' ? ' · delete' : c.action === 'create' ? ' · new' : '')) },
    { label: 'Tenant', get: (c) => c.tenant || '–', cls: () => 'muted' },
    { label: 'What', get: (c) => c.kind === 'deploy' ? `${c.target} ${(c.detail.previous || '∅').slice(0, 8)} → ${(c.detail.version || '').slice(0, 8)}` : c.target, cls: () => 'mono' },
    { label: 'Who', get: (c) => c.actor, cls: () => 'mono muted' },
    { label: 'Size', num: true, get: (c) => c.bytes_after != null ? fmt.bytes(c.bytes_after) : '' },
  ], rows, (c) => { location.hash = '#/change/' + c.id; });
}

function rowsTable(r) {
  if (!r.rows.length) return h('p', { class: 'muted' }, 'No rows.' + (r.changes ? '' : ''));
  return table(r.columns.map((c, i) => ({
    label: c, get: (row) => (row[i] === null ? 'null' : typeof row[i] === 'object' ? JSON.stringify(row[i]) : row[i]),
    cls: (row) => 'cell' + (row[i] === null ? ' null' : ''),
  })), r.rows);
}

function pager(rows, before, next) {
  if (!rows.length && !before) return null;
  return h('div', { class: 'pager' },
    before ? h('button', { type: 'button', onclick: () => { history.back(); } }, '← Newer') : null,
    rows.length >= 100 ? h('button', { type: 'button', onclick: () => next(rows[rows.length - 1].id) }, 'Older →') : null);
}

// Lifting a block takes a person here, with a statement of why (029 § 6): no tool can.
// Approvals per connected agent (035): off by default; when on, the agent's changing calls wait here.
// Deleted accounts that some box hasn't acknowledged (021 § 6): each box holding the tenant's
// data must erase the person's rows within 30 days; past that it is overdue.
async function deletionsCard(tenants) {
  const reports = await Promise.all(tenants.map((t) => api('deletions', { tenant: t }).catch(() => null)));
  const rows = reports.filter(Boolean).flatMap((r) => r.deletions.filter((d) => !d.acknowledged)
    .map((d) => ({ tenant: r.tenant, ...d })));
  const waiting = (d) => d.boxes.filter((b) => b.state !== 'acknowledged');
  return wideCard('Deleted accounts not yet erased everywhere', h('p', { class: 'muted small' },
    'When a person deletes their account, every box holding the site\'s data must erase their rows within 30 days. ' +
    'A box acknowledges by answering its routes.identity "deleted" route; a box with no such route can\'t.'),
  rows.length ? table([
    { label: 'Tenant', get: (d) => d.tenant, cls: () => 'mono' },
    { label: 'User', get: (d) => d.user_id, cls: () => 'mono' },
    { label: 'Deleted', get: (d) => fmt.time(Date.parse(d.at)), cls: () => 'muted' },
    { label: 'Days', num: true, get: (d) => d.age_days, cls: (d) => d.overdue ? 'err' : '' },
    { label: 'By', get: (d) => d.how, cls: () => 'muted' },
    { label: 'Waiting on', get: (d) => waiting(d).map((b) => `${b.box} (${b.state.replace('_', ' ')})`).join(', '),
      cls: (d) => d.overdue ? 'err' : '' },
  ], rows) : h('p', { class: 'muted' }, 'None: every deletion has been acknowledged by every box.'));
}

async function approvalsCard() {
  const a = await api('approvals');
  const status = h('p', { class: 'muted small' });
  const toggle = (ag) => {
    if (!ag.may_set) return h('span', { class: 'muted' }, ag.approvals ? 'on' : 'off');
    const box = h('input', { type: 'checkbox', checked: ag.approvals, onchange: async () => {
      const words = box.checked ? 'Hold every changing call by ' + ag.name + ' for approval?' : 'Let ' + ag.name + ' act without approval?';
      if (!confirm(words)) { box.checked = !box.checked; return; }
      try { await api('write/approvals', { agent: ag.agent, on: box.checked, confirm: true }); render(false); }
      catch (e) { status.textContent = e.message; box.checked = !box.checked; }
    } });
    return h('label', {}, box, ' ', ag.approvals ? 'on' : 'off');
  };
  const decide = (r, decision) => h('button', { class: 'small' + (decision === 'deny' ? ' secondary' : ''), onclick: async () => {
    if (!confirm((decision === 'approve' ? 'Run ' : 'Deny ') + r.tool + ' for ' + r.agent + '?')) return;
    try {
      const out = await api('write/decide', { id: r.id, decision, confirm: true });
      status.textContent = '#' + r.id + ' ' + out.state + (out.is_error ? ' (the tool failed: see Recent)' : '');
      render(false);
    } catch (e) { status.textContent = e.message; }
  } }, decision === 'approve' ? 'Approve' : 'Deny');
  return wideCard('Approvals',
    h('p', { class: 'muted small' }, 'Off by default. Turned on for an agent, its changing tool calls wait here until a person ' +
      'approves (the call then runs as the agent) or denies them; looking never waits. Undecided requests expire in a day.'),
    table([
      { label: 'Agent', get: (ag) => ag.name, cls: () => 'mono' },
      { label: 'Kind', get: (ag) => ag.kind },
      { label: 'Last call', get: (ag) => ag.last_ms ? fmt.time(ag.last_ms) : '', cls: () => 'muted small' },
      { label: 'Approvals', get: (ag) => toggle(ag) },
    ], a.agents),
    h('h3', {}, 'Waiting (' + a.pending.length + ')'),
    a.pending.length ? table([
      { label: '#', get: (r) => String(r.id), cls: () => 'mono' },
      { label: 'When', get: (r) => fmt.time(r.at_ms), cls: () => 'muted small' },
      { label: 'Agent', get: (r) => r.agent, cls: () => 'mono small' },
      { label: 'Tool', get: (r) => r.tool, cls: () => 'mono' },
      { label: 'Arguments', get: (r) => {
        // Never cut silently: what's past the first 20,000 characters is named, so nothing hides there.
        // Invisible and direction-changing characters shown as \uXXXX: what's approved reads as what runs.
        const s = JSON.stringify(r.args, null, 2).replace(/[\u00ad\u200b-\u200f\u202a-\u202e\u2060-\u206f\ufeff]/g,
          (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
        return h('pre', { class: 'small' }, s.length > 20000 ? s.slice(0, 20000) + '\n… and ' + (s.length - 20000) +
          ' more characters not shown: deny it, or ask the agent to split it' : s);
      } },
      // A held upload says what it would widen against the live version (version_diff's review).
      { label: 'Widens', get: (r) => !r.review ? (r.review_error ? h('span', { class: 'err small' }, r.review_error) : '')
        : (r.widens || []).length
          ? h('ul', { class: 'findings small' }, r.widens.map((w) => h('li', { class: 'warn' }, w)))
          : h('span', { class: 'muted small' }, r.review.new_box ? 'a new box that reaches nothing' : 'nothing widens') },
      { label: '', get: (r) => h('span', {}, decide(r, 'approve'), ' ', decide(r, 'deny')) },
    ], a.pending) : h('p', { class: 'muted' }, 'Nothing waiting.'),
    a.recent.length ? [h('h3', {}, 'Recent'), table([
      { label: '#', get: (r) => String(r.id), cls: () => 'mono' },
      { label: 'Agent', get: (r) => r.agent, cls: () => 'mono small' },
      { label: 'Tool', get: (r) => r.tool, cls: () => 'mono' },
      { label: 'State', get: (r) => r.state + (r.is_error ? ' (failed)' : '') },
      { label: 'Result', get: (r) => r.result === undefined ? '' : h('pre', { class: 'small' }, JSON.stringify(r.result).slice(0, 300)) },
      { label: 'By', get: (r) => (r.decided_by || '') + (r.decided_ms ? ' · ' + fmt.time(r.decided_ms) : ''), cls: () => 'muted small' },
    ], a.recent)] : null,
    status);
}

function unblockForm(tenants) {
  const tenant = h('select', {}, tenants.map((t) => h('option', { value: t }, t)));
  const email = h('input', { type: 'email', placeholder: 'person@example.com' });
  const statement = h('textarea', { rows: 2, placeholder: 'Why: "Asked by phone on 11/2 to rejoin the digest"' });
  const out = h('p', { class: 'muted small' });
  const go = h('button', { class: 'small', onclick: async () => {
    if (!confirm('Allow ' + tenant.value + ' to email ' + email.value + ' again?')) return;
    try {
      const r = await api('write/unblock', { tenant: tenant.value, email: email.value, statement: statement.value, confirm: true });
      out.textContent = r.lifted ? 'Lifted. It was blocked ' + fmt.time(r.blocked_at) + ' (' + r.how + ').' : 'That address wasn\'t blocked.';
    } catch (e) { out.textContent = e.message; }
  } }, 'Allow mail again');
  return h('div', { class: 'unblock' },
    h('p', { class: 'muted small' }, 'Someone who declined an invite or unsubscribed is blocked. They can lift it themselves, signed in on the site; ' +
      'or you can, here, saying why. Agents can\'t.'),
    h('label', {}, 'Tenant '), tenant, ' ', h('label', {}, 'Address '), email, statement, go, out);
}

// ------------------------------------------------------------------ source

function mirrorText(m) {
  if (!m) return '–';
  if (!m.patches && !m.pending) return 'nothing yet';
  return `${m.patches} patches, ${m.snapshots} deployed trees` + (m.pending ? ` (${m.pending} to go)` : '');
}

function jobPill(j) {
  const cls = j.state === 'failed' ? 'failed' : j.state === 'succeeded' ? (j.kind === 'deploy' ? 'deploy' : 'partial') : '';
  return h('a', { href: '#/job/' + j.job, class: 'pill ' + cls, title: j.state }, (j.kind === 'deploy' ? 'deploy ' : 'build ') + j.job +
    (j.state === 'succeeded' ? '' : ' · ' + j.state));
}

function jobsTable(jobs, withProject) {
  if (!jobs.length) return h('p', { class: 'muted' }, 'No builds or deploys yet.');
  return table([
    { label: 'Job', get: (j) => jobPill(j) },
    withProject ? { label: 'Project', get: (j) => h('a', { href: '#/project/' + encodeURIComponent(j.project) }, j.project) } : null,
    { label: 'Rev', num: true, get: (j) => j.rev },
    { label: 'State', get: (j) => j.state, cls: (j) => j.state === 'failed' ? 'err' : j.state === 'succeeded' ? '' : 'warn' },
    { label: 'By', get: (j) => j.by, cls: () => 'mono muted' },
    { label: 'Queued', get: (j) => fmt.time(Date.parse(j.queued_at)), cls: () => 'muted' },
    { label: 'Boxes', get: (j) => (j.boxes || []).map((b) => b.box + (b.version ? ' ' + String(b.version).slice(0, 8) : '')).join(', '), cls: () => 'mono muted' },
  ].filter(Boolean), jobs, (j) => { location.hash = '#/job/' + j.job; });
}

// On a box's page: each table its strategies grant, what was actually used since the host
// started, and what to narrow (grant_usage, 008 § 4).
function grantsCard(report) {
  if (!report || !report.strategies || !report.strategies.length) return null;
  const cols = (v) => v === true ? 'every column' : (v || []).length ? v.join(', ') : '—';
  const rows = [];
  for (const st of report.strategies) for (const t of st.tables) rows.push({ st, t });
  const findings = rows.flatMap(({ st, t }) => t.findings.map((f) => ({ st, t, f })));
  return wideCard(`Grants (since ${fmt.time(Date.parse(report.since))})`,
    h('p', { class: 'muted small' }, 'What each table\'s grant allows, and what this version\'s statements did with it. ' +
      'Grant only what is used: narrow grants are the box\'s limits.'),
    table([
      { label: 'Strategy', get: (r) => r.st.strategy, cls: () => 'mono muted' },
      { label: 'Database', get: (r) => r.st.database, cls: () => 'mono' },
      { label: 'Table', get: (r) => r.t.table, cls: () => 'mono' },
      { label: 'Granted', get: (r) => r.t.granted.ops, cls: () => 'mono' },
      { label: 'Used', get: (r) => [r.t.used.read.length ? 'r' : '', r.t.used.insert ? 'i' : '', r.t.used.update.length ? 'u' : '',
        r.t.used.delete ? 'd' : ''].join('') || '—', cls: (r) => 'mono' + (r.t.calls ? '' : ' muted') },
      { label: 'Reads', get: (r) => cols(r.t.granted.read), cls: () => 'muted' },
      { label: 'Read', get: (r) => cols(r.t.used.read), cls: () => 'muted' },
      { label: 'Updates', get: (r) => cols(r.t.granted.update), cls: () => 'muted' },
      { label: 'Calls', num: true, get: (r) => fmt.n(r.t.calls) },
    ], rows),
    findings.length ? h('ul', { class: 'findings' }, findings.map(({ st, t, f }) => h('li', {},
      h('code', {}, `${st.strategy}.${t.table}`), ' ', f.text,
      f.suggested ? [': ', h('code', {}, JSON.stringify(f.suggested))] : null,
      f.columns ? [': ', h('code', {}, f.columns.join(', '))] : null)))
      : h('p', { class: 'muted small' }, 'Nothing to narrow: every grant has been used.'));
}

// A box's jobs (038 § 7) for every tenant on the host: job_list is per tenant.
// A box's flags (039) for every tenant on the host: flag_list is per tenant.
async function flagsOf(box) {
  const domains = await api('domain_list').catch(() => []);
  const tenants = [...new Set(domains.filter((d) => !d.redirect_to).map((d) => d.tenant))].sort();
  const lists = await Promise.all(tenants.map((t) => api('flag_list', { tenant: t, box }).catch(() => ({ boxes: [] }))));
  return lists.flatMap((l, i) => l.boxes.flatMap((b) => b.flags.map((f) => ({ ...f, tenant: tenants[i] }))));
}

// On a box's page: its flags per tenant, what the box reads now, and who flipped them. Set over
// MCP with flag_set; the panel only looks.
function flagsCard(flags) {
  if (!flags.length) return null;
  return wideCard('Flags', h('p', { class: 'muted small' },
    'Switches the manifest declares, flipped with flag_set; the box reads them on its next call, no new version.'),
  table([
    { label: 'Tenant', get: (f) => f.tenant, cls: () => 'mono' },
    { label: 'Flag', get: (f) => f.flag, cls: () => 'mono' },
    { label: 'Value', get: (f) => f.value, cls: (f) => 'mono' + (f.value === f.default ? ' muted' : '') },
    { label: 'Default', get: (f) => f.default, cls: () => 'mono muted' },
    { label: 'Set by', get: (f) => f.set_by || '', cls: () => 'mono muted' },
    { label: 'When', get: (f) => f.set_at ? fmt.time(Date.parse(f.set_at)) : '', cls: () => 'muted' },
    { label: 'Why', get: (f) => clip(f.set_why || f.about || '', 100), cls: () => 'muted' },
  ], flags));
}

async function jobsOf(box) {
  const domains = await api('domain_list').catch(() => []);
  const tenants = [...new Set(domains.filter((d) => !d.redirect_to).map((d) => d.tenant))].sort();
  const lists = await Promise.all(tenants.map((t) => api('job_list', { tenant: t }).catch(() => ({ jobs: [] }))));
  return lists.flatMap((l) => l.jobs).filter((j) => j.box === box);
}

// On a box's page: its one-time jobs, per tenant, and where each stands. Started over MCP
// (job_dry_run, then job_start); the panel only looks.
function jobsCard(jobs) {
  if (!jobs.length) return null;
  const last = (j) => j.runs[0] || {};
  return wideCard('Jobs', h('p', { class: 'muted small' },
    'One-time work an operator starts: job_dry_run on copies first, then job_start. Runs go in batches and resume after a restart.'),
  table([
    { label: 'Tenant', get: (j) => j.tenant, cls: () => 'mono' },
    { label: 'Job', get: (j) => j.job, cls: () => 'mono' },
    { label: 'Dry run', get: (j) => j.dry_run_passed ? 'passed' : '—', cls: (j) => j.dry_run_passed ? '' : 'muted' },
    { label: 'Done', get: (j) => j.done ? 'yes' : '—', cls: (j) => j.done ? '' : 'muted' },
    { label: 'Newest run', get: (j) => last(j).run ? `${last(j).dry_run ? 'dry run' : 'run'} ${last(j).state}` : 'never',
      cls: (j) => ['failed', 'paused'].includes(last(j).state) ? 'err' : 'muted' },
    { label: 'Batches', num: true, get: (j) => last(j).calls ?? '' },
    { label: 'Moved', num: true, get: (j) => last(j).moved ?? '' },
    { label: 'Cursor', get: (j) => clip(last(j).cursor || '', 24), cls: () => 'mono muted' },
    { label: 'Updated', get: (j) => last(j).updated_at ? fmt.time(Date.parse(last(j).updated_at)) : '', cls: () => 'muted' },
    { label: 'Last error', get: (j) => clip(last(j).last_error || '', 100), cls: () => 'err' },
  ], jobs));
}

// Performance (037 § 7): what to fix, with its evidence (perf_report's findings).
function perfFindingsCard(findings) {
  if (!findings || !findings.length) return null;
  return wideCard('Findings', h('ul', { class: 'findings' }, findings.map((f) => h('li', {},
    h('span', { class: 'pill' }, f.kind.replace(/_/g, ' ')), ' ',
    h('code', {}, f.box + (f.route ? ' · ' + f.route : '')), ' ', f.text,
    f.evidence && f.evidence.suggested_index ? [': ', h('code', {}, JSON.stringify({ [f.evidence.suggested_index.name]: { columns: f.evidence.suggested_index.columns } }))] : null))));
}

// Where each route's time goes: the average call split into its parts.
function perfRoutesCard(routes) {
  if (!routes || !routes.length) return null;
  const a = (r, k) => fmt.us(r.average[k] || 0);
  return wideCard('Where the time goes (average per call)', table([
    { label: 'Box · route', get: (r) => r.box + ' · ' + r.route, cls: () => 'mono' },
    { label: 'Verb', get: (r) => r.verb, cls: () => 'muted' },
    { label: 'Calls', num: true, get: (r) => fmt.n(r.calls) },
    { label: 'p99', num: true, get: (r) => fmt.us(r.p99_us) },
    { label: 'Queue', num: true, get: (r) => a(r, 'queue_us') },
    { label: 'Box', num: true, get: (r) => a(r, 'box_us') },
    { label: 'Lock wait', num: true, get: (r) => a(r, 'lock_us'), cls: (r) => r.average.lock_us > 5000 ? 'warn' : '' },
    { label: 'SQL', num: true, get: (r) => a(r, 'statement_us') },
    { label: 'Commit', num: true, get: (r) => a(r, 'commit_us') },
    { label: 'Calls out', num: true, get: (r) => a(r, 'out_us') },
    { label: 'Render', num: true, get: (r) => a(r, 'render_us') },
    { label: 'Cache hits', num: true, get: (r) => r.cache.hit + r.cache.not_modified ? Math.round(100 * r.cache.hit_rate) + '%' : '' },
    { label: 'Refused writes', num: true, get: (r) => r.refused_writes || '', cls: (r) => r.refused_writes ? 'err' : '' },
  ], routes));
}

// Each SQL statement boxes ran in the last hour, with SQLite's own counters.
function perfStatementsCard(rows, sort, onSort) {
  return wideCard('Statements (last hour)', h('div', { class: 'bar' }, h('label', {}, 'Sort '),
    segmented(['total', 'count', 'fullscan', 'max'], sort, onSort)),
  rows && rows.length ? table([
    { label: 'Box · strategy', get: (r) => r.box + ' · ' + r.strategy, cls: () => 'muted' },
    { label: 'Statement', get: (r) => clip(r.sql, 140), cls: () => 'mono' },
    { label: 'Runs', num: true, get: (r) => fmt.n(r.count) },
    { label: 'Total', num: true, get: (r) => fmt.us(r.total_us) },
    { label: 'Avg', num: true, get: (r) => fmt.us(r.avg_us) },
    { label: 'Max', num: true, get: (r) => fmt.us(r.max_us) },
    { label: 'Rows', num: true, get: (r) => fmt.n(r.rows) },
    { label: 'Full-scan steps', num: true, get: (r) => fmt.n(r.fullscan_steps), cls: (r) => r.fullscan_steps / Math.max(r.count, 1) >= 100 ? 'warn' : '' },
    { label: 'Auto index', num: true, get: (r) => r.autoindexes || '', cls: (r) => r.autoindexes ? 'warn' : '' },
  ], rows) : h('p', { class: 'muted' }, 'No statements in the last hour.'));
}

// On a box's page: the source deploys behind its versions (020 § 9).
function sourceCard(history) {
  const deploys = (history && history.deploys) || [];
  if (!deploys.length) return null;
  return wideCard('From source', table([
    { label: 'Version', get: (d) => h('code', {}, String(d.version || '').slice(0, 16)) },
    { label: 'Project', get: (d) => h('a', { href: '#/project/' + encodeURIComponent(d.project) + '?rev=' + d.rev }, d.project + ' r' + d.rev) },
    { label: 'Message', get: (d) => clip((d.message || '').split('\n')[0], 90) },
    { label: 'Deployed by', get: (d) => d.by, cls: () => 'mono muted' },
    { label: 'When', get: (d) => fmt.time(Date.parse(d.at)), cls: () => 'muted' },
    { label: 'Log', get: (d) => h('a', { href: '#/job/' + d.job }, 'job ' + d.job) },
  ], deploys));
}

// A unified diff as the host writes it, colored: file headers, hunks, additions, removals.
function unifiedView(text) {
  const box = h('div', { class: 'diff' });
  for (const line of text.split('\n')) {
    let cls = '';
    if (line.startsWith('+++ ') || line.startsWith('--- ') || line.startsWith('Binary file ')) cls = 'file';
    else if (line.startsWith('@@')) cls = 'hunk';
    else if (line.startsWith('+')) cls = 'add';
    else if (line.startsWith('-')) cls = 'del';
    box.appendChild(h('div', { class: cls }, line || ' '));
  }
  return box;
}

function fileLink(name, rev, path) {
  return h('a', { href: '#/project/' + encodeURIComponent(name) + '?rev=' + rev + '&file=' + encodeURIComponent(path) }, path);
}

async function revisionView(name, rev) {
  const [tree, diff] = await Promise.all([
    api('source_tree', { project: name, rev }),
    rev > 1 ? api('source_diff', { project: name, from: rev - 1, to: rev, verbose: true }) : Promise.resolve(null)]);
  const changed = new Set(tree.changed || []);
  const base = '#/project/' + encodeURIComponent(name);
  return [
    h('div', { class: 'bar' }, h('h1', {}, name + ' · revision ' + rev), h('a', { href: base }, '← ' + name),
      rev > 1 ? h('a', { href: base + '?rev=' + (rev - 1) }, '← ' + (rev - 1)) : null,
      rev < tree.head ? h('a', { href: base + '?rev=' + (rev + 1) }, (rev + 1) + ' →') : null),
    h('p', {}, h('b', {}, (tree.message || '').split('\n')[0]), h('span', { class: 'muted' }, ' · ' + (tree.by || '') + ' · ' + (tree.at ? fmt.time(Date.parse(tree.at)) : ''))),
    (tree.message || '').includes('\n') ? h('pre', { class: 'json' }, tree.message) : null,
    wideCard('What changed', diff ? (diff.diff && diff.diff !== '(no differences)' ? unifiedView(diff.diff) : h('p', { class: 'muted' }, 'No differences.'))
      : h('p', { class: 'muted' }, 'The first revision: every file is new.')),
    wideCard(`Files (${tree.files.length})`, table([
      { label: 'Path', get: (f) => fileLink(name, rev, f.path), cls: () => 'mono' },
      { label: '', get: (f) => changed.has(f.path) ? h('span', { class: 'pill deploy' }, 'changed') : '' },
      { label: 'Size', num: true, get: (f) => fmt.bytes(f.size) },
      { label: 'SHA-256', get: (f) => f.sha256.slice(0, 12), cls: () => 'mono muted' },
    ], tree.files)),
  ];
}

async function fileView(name, rev, path) {
  const [file] = await api('source_read', { project: name, rev, paths: [path] });
  const back = '#/project/' + encodeURIComponent(name) + '?rev=' + (rev || '');
  let body;
  if (typeof file.text === 'string') {
    // Line numbers beside the text, each line placed as text.
    const lines = file.text.split('\n');
    if (lines.length > 1 && lines[lines.length - 1] === '') lines.pop();
    body = h('div', { class: 'code' }, lines.map((l, i) => h('div', {}, h('span', { class: 'ln' }, String(i + 1)), h('span', {}, l || ' '))));
  } else {
    body = h('p', { class: 'muted' }, `Binary, ${fmt.bytes(file.size)}: not shown.`);
  }
  return [
    h('div', { class: 'bar' }, h('h1', { class: 'mono' }, path), h('a', { href: back }, '← ' + name + (rev ? ' revision ' + rev : ''))),
    wideCard(fmt.bytes(file.size), body),
  ];
}

// ------------------------------------------------------------------ router

let renderSeq = 0;
async function render(quiet) {
  const hash = location.hash.replace(/^#\/?/, '');
  const [route, query] = hash.split('?');
  const params = new URLSearchParams(query || '');
  const [view, arg] = route.split('/').map(decodeURIComponent);
  const name = view || 'overview';
  const needsArg = ['box', 'change', 'project', 'job'].includes(name);
  const fn = Object.hasOwn(views, name) && (!needsArg || arg) ? views[name] : null;
  for (const a of document.querySelectorAll('#nav a')) {
    const v = a.getAttribute('data-view');
    a.classList.toggle('active', v === name || (v === 'boxes' && name === 'box') || (v === 'changes' && name === 'change') ||
      (v === 'source' && (name === 'project' || name === 'job')));
  }
  const seq = ++renderSeq;
  if (!quiet) { clear($main); $main.appendChild(h('p', { class: 'muted' }, 'Loading…')); }
  let out;
  try {
    out = fn ? await fn(params, arg) : [h('h1', {}, 'Not found'), h('p', {}, h('a', { href: '#/' }, 'Back to the overview'))];
  } catch (e) {
    if (e instanceof ApiError && e.message === 'signed out') return;
    out = [errorBox(e)];
  }
  if (seq !== renderSeq) return;  // a newer render started
  // A quiet refresh keeps what the person is typing or reading.
  if (quiet && $main.contains(document.activeElement) && document.activeElement.matches('textarea, input, select')) return;
  const y = window.scrollY;
  clear($main);
  append($main, [out]);
  document.title = 'Goober Panel · ' + name[0].toUpperCase() + name.slice(1);
  if (quiet) window.scrollTo(0, y);
}

async function start() {
  try {
    const res = await fetch('/api/me', { credentials: 'same-origin' });
    if (res.status === 401) { location.href = '/identity/sign-in/as-host?return=%2F'; return; }
    if (!res.ok) { clear($main); $main.appendChild(errorBox(new Error((await res.json()).error || 'no access'))); return; }
    state.me = await res.json();
  } catch (e) {
    clear($main); $main.appendChild(errorBox(e)); return;
  }
  const who = document.getElementById('who');
  append(who, [focus() ? h('span', { class: 'pill focus' }, 'Focused on ' + focus().join(', ')) : null,
    h('span', { title: state.me.roles.join(', ') }, state.me.email), h('span', {}, 'build ' + state.me.build),
    h('form', { method: 'post', action: '/identity/sign-out' }, h('input', { type: 'hidden', name: 'return', value: '/' }), h('button', {}, 'Sign out'))]);
  window.addEventListener('hashchange', () => render(false));
  // Live views refresh every 15 seconds while visible; the database and change pages don't.
  setInterval(() => {
    const v = location.hash.replace(/^#\/?/, '').split(/[/?]/)[0] || 'overview';
    if (document.visibilityState === 'visible' && ['overview', 'traffic', 'services', 'controls', 'flow', 'boxes', 'box', 'perf', 'agents', 'source', 'job', 'studies'].includes(v)) render(true);
  }, 15000);
  render(false);
}

start();
