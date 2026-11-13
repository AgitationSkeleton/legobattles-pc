// COMMUNITY UNITS (docs/448). Custom units and buildings made in the port's
// Balance > Custom units, shared like community levels: the same Durable
// Object (the Levels object, so the same settings, bans and moderator), the
// same Discord channel and buttons (custom ids "lvl:unit:..." so the bot, which
// forwards every "lvl:" press, needs no change).
//
//   POST /units/upload  {key, name, author, credits?, category, template, bundle, sheet,
//                        thumb, sounds:[{name, wav}]}      binary fields: base64
//                        (credits: optional collaborators line, SG-0070)
//   GET  /units/list?sort=new|old|downloads|likes&category=<0..19>&offset=&limit=
//   GET  /units/get?id=N[&count=1]    the bundle (count: a download)
//   GET  /units/thumb?id=N            the small picture for the catalogue
//   POST /units/like   {key, id, like}
//   POST /units/delete {key, id}
//
// SAFETY. A unit is DATA the port reads, never code. The bundle (the lobby
// format, "LBUB" 1) is parsed with a bound on every read; every path must be
// one of the shapes the importer reads (unit.json, sheets/<name>.png|.NCBR,
// frames/<action>/<frame>.png, two portraits, model/texture.png,
// model/model.obj, sounds/<name>.wav); every PNG
// must be a PNG and every NCBR an NCBR (raw or COMP); unit.json must parse as
// JSON with a template index 0..181 and numeric fields. The preview sheet, the
// thumbnail and the sounds are checked as PNG and PCM WAV and bounded. Names
// pass the allow-list and the swear filter. SQL is bound parameters only.

const MAX_BUNDLE = 1024 * 1024;
const MAX_SHEET = 768 * 1024;
const MAX_THUMB = 64 * 1024;
const MAX_WAV = 256 * 1024;
const MAX_SOUNDS = 8;
// SG-0070: the optional credits line: letters, digits, space and _ - ' . , & ! ?, 1..60.
const CREDITS_OK = /^[\w \-'.,&!?]{1,60}$/u;
export const MAX_UNIT_BODY = Math.ceil((MAX_BUNDLE + MAX_SHEET + MAX_THUMB + MAX_SOUNDS * MAX_WAV) * 4 / 3) + 16384;
const NAME_OK = /^[\w \-'.!?]{1,24}$/u;
const KEY_OK = /^[0-9a-fA-F]{32}$/;
const PATH_OK = /^(unit\.json|sheets\/[A-Za-z0-9_-]{1,48}\.(png|NCBR)|frames\/[A-Za-z0-9_-]{1,24}\/[A-Za-z0-9_-]{1,32}\.png|portraits\/card\.png|portraits\/card_top_screen\.png|portraits\/card_mask\.png|portraits\/head\.png|portraits\/hud_icon\.png|portraits\/build_icon\.png|portraits\/end_victory\.png|portraits\/end_defeat\.png|model\/texture\.png|model\/model\.obj|sounds\/[A-Za-z0-9_-]{1,48}\.wav)$/;
// The record's category byte (+0x5C), as the game's records use it.
export const CATEGORY_NAMES = ['Hero', 'Builder', 'Melee', 'Ranged', 'Mounted', 'Transport', 'Special', 'Base',
  'Lumber mill', 'Mine', 'Farm', 'Barracks', 'Factory', 'Tower', 'Tower 2', 'Tower 3', 'Shipyard', 'Bridge',
  'Gate', 'Wall'];
const MAX_CATEGORY = CATEGORY_NAMES.length - 1;
const DISCORD = 'https://discord.com/api/v10';

function json(status, payload, cache) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': cache || 'no-store' },
  });
}
async function sha256hex(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
function b64bytes(s, cap) {
  if (typeof s !== 'string' || s.length > Math.ceil(cap * 4 / 3) + 8) return null;
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(s)) return null;
  try {
    const bin = atob(s);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; ++i) out[i] = bin.charCodeAt(i);
    return out;
  } catch {
    return null;
  }
}
function bytesB64(u8) {
  let s = '';
  for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000));
  return btoa(s);
}
const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const be32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

export function isPng(b, maxSide) {
  const sig = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  if (!b || b.length < 33) return false;
  for (let i = 0; i < 8; ++i) if (b[i] !== sig[i]) return false;
  if (String.fromCharCode(b[12], b[13], b[14], b[15]) !== 'IHDR') return false;
  const w = be32(b, 16), h = be32(b, 20);
  return w > 0 && h > 0 && w <= maxSide && h <= maxSide;
}
export function isWav(b) {
  if (!b || b.length < 44) return false;
  const tag = (o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE' || tag(12) !== 'fmt ') return false;
  const fmt = u16(b, 20), ch = u16(b, 22), rate = u32(b, 24), bits = u16(b, 34);
  return fmt === 1 && ch >= 1 && ch <= 2 && rate >= 4000 && rate <= 96000 && (bits === 8 || bits === 16);
}
// docs/492 (SG-0083): model/model.obj is plain text (printable ASCII, tabs and
// line breaks), at most MAX_OBJ bytes; the port parses only v, vt, vn and f.
const MAX_OBJ = 512 * 1024;
export function isObjText(b) {
  if (!b || b.length < 1 || b.length > MAX_OBJ) return false;
  for (let i = 0; i < b.length; ++i) {
    const c = b[i];
    if (!(c === 9 || c === 10 || c === 13 || (c >= 32 && c < 127))) return false;
  }
  return true;
}

// The bundle: {ok, why, unit: {template, fields}} with every file checked.
export function validateBundle(b) {
  if (!b || b.length < 7 || b.length > MAX_BUNDLE) return { ok: false, why: 'size' };
  if (String.fromCharCode(b[0], b[1], b[2], b[3]) !== 'LBUB' || b[4] !== 1) return { ok: false, why: 'not a unit bundle' };
  const count = u16(b, 5);
  if (count < 1 || count > 400) return { ok: false, why: 'file count' };
  let at = 7, unitJson = null;
  const seen = new Set();
  for (let i = 0; i < count; ++i) {
    if (at + 2 > b.length) return { ok: false, why: 'truncated' };
    const pl = u16(b, at); at += 2;
    if (pl < 1 || pl > 96 || at + pl + 4 > b.length) return { ok: false, why: 'truncated' };
    const path = String.fromCharCode(...b.subarray(at, at + pl)); at += pl;
    const dl = u32(b, at); at += 4;
    if (dl > MAX_BUNDLE || at + dl > b.length) return { ok: false, why: 'truncated' };
    if (!PATH_OK.test(path) || seen.has(path)) return { ok: false, why: 'path ' + path.slice(0, 40) };
    seen.add(path);
    const d = b.subarray(at, at + dl); at += dl;
    if (path.endsWith('.png') && !isPng(d, 1024)) return { ok: false, why: 'not a PNG: ' + path };
    if (path.endsWith('.wav') && !isWav(d)) return { ok: false, why: 'not a PCM WAV: ' + path };
    if (path.endsWith('.obj') && !isObjText(d)) return { ok: false, why: 'not a text OBJ: ' + path };
    if (path.endsWith('.NCBR')) {
      const t = String.fromCharCode(d[0], d[1], d[2], d[3]);
      if (t !== 'RGCN' && t !== 'PMOC') return { ok: false, why: 'not an NCBR: ' + path };
    }
    if (path === 'unit.json') unitJson = d;
  }
  if (at !== b.length) return { ok: false, why: 'trailing bytes' };
  if (!unitJson || unitJson.length > 256 * 1024) return { ok: false, why: 'no unit.json' };
  let j;
  try { j = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(unitJson)); } catch { return { ok: false, why: 'unit.json is not JSON' }; }
  if (!j || typeof j !== 'object' || !Number.isInteger(j.index) || j.index < 0 || j.index > 181)
    return { ok: false, why: 'unit.json has no template index' };
  if (j.fields !== undefined) {
    if (!j.fields || typeof j.fields !== 'object' || Array.isArray(j.fields)) return { ok: false, why: 'fields' };
    for (const [k, v] of Object.entries(j.fields))
      if (!/^[a-z_0-9]{1,24}$/.test(k) || !Number.isInteger(v) || v < 0 || v > 0x7fffffff) return { ok: false, why: 'field ' + k };
  }
  return { ok: true, unit: { template: j.index } };
}

// ------------------------------------------------------------ moderation ----
function unitEmbed(row, status) {
  const colour = status === 'approved' ? 0x3ba55d : status === 'rejected' ? 0xed4245
               : status === 'banned' ? 0x8b0000 : 0xfaa61a;
  return {
    title: row.name,
    description: `Custom unit by ${row.author || 'unknown'}${row.credits ? ` (credits: ${row.credits})` : ''}`,
    color: colour,
    fields: [
      { name: 'Kind', value: CATEGORY_NAMES[row.category] || '?', inline: true },
      { name: 'Template', value: String(row.template), inline: true },
      { name: 'Sounds', value: String(row.sounds), inline: true },
      { name: 'Status', value: status, inline: true },
      { name: 'Uploader', value: '`' + row.owner.slice(0, 10) + '`', inline: true },
      { name: 'Unit', value: '#' + row.id, inline: true },
    ],
    image: { url: 'attachment://sheet.png' },
    timestamp: new Date(row.created).toISOString(),
  };
}
function unitButtons(row, status) {
  if (status !== 'pending')
    return [{ type: 1, components: [
      { type: 2, style: 2, label: status === 'approved' ? 'Unpublish' : 'Approve after all',
        custom_id: `lvl:unit:${status === 'approved' ? 'reject' : 'approve'}:${row.id}` },
      { type: 2, style: 4, label: 'Ban uploader', custom_id: `lvl:unit:ban:${row.id}` },
    ] }];
  return [{ type: 1, components: [
    { type: 2, style: 3, label: 'Approve', custom_id: `lvl:unit:approve:${row.id}` },
    { type: 2, style: 4, label: 'Reject', custom_id: `lvl:unit:reject:${row.id}` },
    { type: 2, style: 2, label: 'Ban uploader', custom_id: `lvl:unit:ban:${row.id}` },
  ] }];
}

// --------------------------------------------------------------- routes ----
export function unitsSetup(sql) {
  sql.exec(`CREATE TABLE IF NOT EXISTS units (
      id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT UNIQUE, name TEXT, author TEXT,
      category INTEGER, template INTEGER, sounds INTEGER, size INTEGER, status TEXT, owner TEXT, addr TEXT,
      created INTEGER, decided INTEGER, message TEXT, downloads INTEGER DEFAULT 0, likes INTEGER DEFAULT 0,
      bundle BLOB, thumb BLOB)`);
  // SG-0070: older databases lack the credits column; add it once. The error
  // from a table that already has it is the expected case, so it is ignored.
  try { sql.exec('ALTER TABLE units ADD COLUMN credits TEXT'); } catch { /* already there */ }
  sql.exec('CREATE INDEX IF NOT EXISTS units_status ON units (status, id)');
  sql.exec('CREATE TABLE IF NOT EXISTS unit_likes (unit INTEGER, owner TEXT, PRIMARY KEY (unit, owner))');
}

// `obj` is the Levels object: its sql, env, cfg(), counts().
export async function unitsFetch(obj, request, url, path) {
  const sql = obj.sql;
  if (request.method === 'GET' && path === '/units/list') {
    const offset = Math.max(0, Math.min(100000, Number.parseInt(url.searchParams.get('offset') || '0', 10) || 0));
    const limit = Math.max(1, Math.min(100, Number.parseInt(url.searchParams.get('limit') || '50', 10) || 50));
    const ORDERS = { new: 'id DESC', old: 'id ASC', downloads: 'downloads DESC, id DESC', likes: 'likes DESC, id DESC' };
    const order = ORDERS[url.searchParams.get('sort') || 'new'] || ORDERS.new;
    const cat = Number.parseInt(url.searchParams.get('category') ?? '', 10);
    const byCat = Number.isInteger(cat) && cat >= 0 && cat <= MAX_CATEGORY;
    const rows = sql.exec(
      `SELECT id, name, author, credits, category, template, sounds, size, downloads, likes, created, hash FROM units
         WHERE status = 'approved' ${byCat ? 'AND category = ?' : ''} ORDER BY ${order} LIMIT ? OFFSET ?`,
      ...(byCat ? [cat, limit, offset] : [limit, offset])).toArray();
    const total = sql.exec(`SELECT COUNT(*) AS n FROM units WHERE status = 'approved' ${byCat ? 'AND category = ?' : ''}`,
      ...(byCat ? [cat] : [])).toArray()[0].n;
    return json(200, { ok: true, total, units: rows });
  }
  if (request.method === 'GET' && (path === '/units/get' || path === '/units/thumb')) {
    const id = Number.parseInt(url.searchParams.get('id') || '', 10);
    if (!Number.isFinite(id) || id < 1) return json(400, { ok: false, error: 'id' });
    const r = sql.exec("SELECT id, name, author, credits, category, template, hash, bundle, thumb FROM units WHERE id = ? AND status = 'approved'", id).toArray()[0];
    if (!r) return json(404, { ok: false, error: 'no such unit' });
    if (path === '/units/thumb')
      return json(200, { ok: true, id: r.id, png: r.thumb ? bytesB64(new Uint8Array(r.thumb)) : '' }, 'public, max-age=86400');
    if (url.searchParams.get('count') === '1') sql.exec('UPDATE units SET downloads = downloads + 1 WHERE id = ?', id);
    return json(200, { ok: true, id: r.id, name: r.name, author: r.author, credits: r.credits || '', category: r.category, template: r.template,
      hash: r.hash, bundle: bytesB64(new Uint8Array(r.bundle)) });
  }
  if (request.method === 'POST' && path === '/units/like') {
    let b;
    try { b = JSON.parse(await request.text()); } catch { return json(400, { ok: false, error: 'bad request' }); }
    if (!b || typeof b !== 'object' || !KEY_OK.test(String(b.key || ''))) return json(400, { ok: false, error: 'bad request' });
    const owner = await sha256hex(new TextEncoder().encode(String(b.key).toLowerCase()));
    const id = Number.parseInt(b.id, 10);
    if (!Number.isFinite(id) || id < 1) return json(400, { ok: false, error: 'id' });
    if (!sql.exec("SELECT 1 FROM units WHERE id = ? AND status = 'approved'", id).toArray().length)
      return json(404, { ok: false, error: 'no such unit' });
    if (b.like === false) sql.exec('DELETE FROM unit_likes WHERE unit = ? AND owner = ?', id, owner);
    else sql.exec('INSERT OR IGNORE INTO unit_likes (unit, owner) VALUES (?, ?)', id, owner);
    const n = sql.exec('SELECT COUNT(*) AS n FROM unit_likes WHERE unit = ?', id).toArray()[0].n;
    sql.exec('UPDATE units SET likes = ? WHERE id = ?', n, id);
    return json(200, { ok: true, id, likes: n, liked: b.like !== false });
  }
  if (request.method === 'POST' && path === '/units/delete') {
    let b;
    try { b = JSON.parse(await request.text()); } catch { return json(400, { ok: false, error: 'bad request' }); }
    if (!b || !KEY_OK.test(String(b.key || ''))) return json(400, { ok: false, error: 'bad request' });
    const owner = await sha256hex(new TextEncoder().encode(String(b.key).toLowerCase()));
    const id = Number.parseInt(b.id, 10);
    if (!sql.exec('SELECT id FROM units WHERE id = ? AND owner = ?', id, owner).toArray()[0])
      return json(404, { ok: false, error: 'no such unit of yours' });
    sql.exec('DELETE FROM units WHERE id = ?', id);
    return json(200, { ok: true });
  }
  if (request.method === 'POST' && path === '/units/upload') return upload(obj, request);
  return json(404, { ok: false, error: 'no such endpoint' });
}

async function upload(obj, request) {
  const env = obj.env, sql = obj.sql;
  const text = await request.text();
  if (text.length > MAX_UNIT_BODY) return json(413, { ok: false, error: 'too large' });
  let b;
  try { b = JSON.parse(text); } catch { return json(400, { ok: false, error: 'bad request' }); }
  if (!b || typeof b !== 'object' || Array.isArray(b)) return json(400, { ok: false, error: 'bad request' });
  const cfg = obj.cfg();
  if (!cfg.uploads) return json(403, { ok: false, error: 'uploads are closed for now' });
  if (!KEY_OK.test(String(b.key || ''))) return json(400, { ok: false, error: 'bad request' });
  const owner = await sha256hex(new TextEncoder().encode(String(b.key).toLowerCase()));
  const ip = request.headers.get('X-Levels-Addr') || '';
  const addr = ip ? (await sha256hex(new TextEncoder().encode('lvl:' + ip))).slice(0, 32) : '';
  if (sql.exec('SELECT 1 FROM bans WHERE who = ? OR who = ?', owner, addr || '-').toArray().length)
    return json(403, { ok: false, error: 'you cannot upload' });
  const clean = (v) => {
    const s = String(v == null ? '' : v).trim();
    return NAME_OK.test(s) && !obj.swear(s) ? s : null;
  };
  const name = clean(b.name);
  if (!name) return json(400, { ok: false, error: 'the unit name is not allowed' });
  const author = b.author ? clean(b.author) : '';
  if (author === null) return json(400, { ok: false, error: 'the author name is not allowed' });
  // SG-0070: optional; absent from older clients. Same swear filter as the names.
  const credits = b.credits ? String(b.credits).trim() : '';
  if (credits && (!CREDITS_OK.test(credits) || obj.swear(credits))) return json(400, { ok: false, error: 'the credits are not allowed' });
  const category = Number.parseInt(b.category, 10);
  if (!Number.isInteger(category) || category < 0 || category > MAX_CATEGORY) return json(400, { ok: false, error: 'category' });
  const bundle = b64bytes(b.bundle, MAX_BUNDLE);
  const v = validateBundle(bundle);
  if (!v.ok) return json(400, { ok: false, error: 'bad unit data (' + v.why + ')' });
  if (Number.parseInt(b.template, 10) !== v.unit.template) return json(400, { ok: false, error: 'template' });
  const sheet = b64bytes(b.sheet, MAX_SHEET);
  if (!isPng(sheet, 4096)) return json(400, { ok: false, error: 'bad preview sheet' });
  const thumb = b64bytes(b.thumb, MAX_THUMB);
  if (!isPng(thumb, 256)) return json(400, { ok: false, error: 'bad thumbnail' });
  const sounds = [];
  if (Array.isArray(b.sounds)) {
    if (b.sounds.length > MAX_SOUNDS) return json(400, { ok: false, error: 'too many sounds' });
    for (const s of b.sounds) {
      const nm = String((s && s.name) || '');
      const wav = b64bytes(s && s.wav, MAX_WAV);
      if (!/^[A-Za-z0-9_]{1,40}$/.test(nm) || !isWav(wav)) return json(400, { ok: false, error: 'bad sound' });
      sounds.push({ name: nm, wav });
    }
  }
  const now = Date.now();
  if (cfg.per_owner_day) {
    const n = sql.exec('SELECT COUNT(*) AS n FROM units WHERE owner = ? AND created > ?', owner, now - 86400000).toArray()[0].n;
    if (n >= cfg.per_owner_day) return json(429, { ok: false, error: 'upload limit reached for today' });
  }
  if (cfg.per_hour) {
    const n = sql.exec('SELECT COUNT(*) AS n FROM units WHERE created > ?', now - 3600000).toArray()[0].n;
    if (n >= cfg.per_hour) return json(429, { ok: false, error: 'too many uploads right now; try later' });
  }
  const pending = sql.exec("SELECT COUNT(*) AS n FROM units WHERE status = 'pending'").toArray()[0].n;
  if (pending >= cfg.max_pending && !cfg.auto_approve) return json(429, { ok: false, error: 'the review queue is full; try later' });
  const hash = await sha256hex(bundle);
  if (sql.exec('SELECT 1 FROM units WHERE hash = ?', hash).toArray().length)
    return json(409, { ok: false, error: 'this unit was uploaded before' });
  const status = cfg.auto_approve ? 'approved' : 'pending';
  sql.exec(`INSERT INTO units (hash, name, author, credits, category, template, sounds, size, status, owner, addr, created, bundle, thumb)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    hash, name, author || '', credits, category, v.unit.template, sounds.length, bundle.length, status, owner, addr, now, bundle, thumb);
  const row = sql.exec('SELECT * FROM units WHERE hash = ?', hash).toArray()[0];
  const msg = await post(env, unitEmbed(row, status), unitButtons(row, status), sheet, sounds);
  if (msg) sql.exec('UPDATE units SET message = ? WHERE id = ?', msg, row.id);
  return json(200, { ok: true, id: row.id, status });
}

// The moderation post: the embed, the full spritesheet and every sound.
async function post(env, embed, components, sheet, sounds) {
  if (!env.LEVELS_BOT_TOKEN || !env.LEVELS_CHANNEL) return '';
  const form = new FormData();
  const attachments = [{ id: 0, filename: 'sheet.png' }];
  sounds.forEach((s, i) => attachments.push({ id: i + 1, filename: s.name + '.wav' }));
  form.append('payload_json', JSON.stringify({ embeds: [embed], components, attachments, allowed_mentions: { parse: [] } }));
  form.append('files[0]', new Blob([sheet], { type: 'image/png' }), 'sheet.png');
  sounds.forEach((s, i) => form.append(`files[${i + 1}]`, new Blob([s.wav], { type: 'audio/wav' }), s.name + '.wav'));
  const r = await fetch(`${DISCORD}/channels/${env.LEVELS_CHANNEL}/messages`, {
    method: 'POST', headers: { Authorization: 'Bot ' + env.LEVELS_BOT_TOKEN }, body: form,
  });
  if (!r.ok) return '';
  const m = await r.json().catch(() => null);
  return m && m.id ? String(m.id) : '';
}

// A moderator's press on a unit's message ("lvl:unit:<what>:<id>"), already
// checked to be the moderator by the Levels object.
export function unitsAdmin(obj, customId) {
  const m = /^lvl:unit:(approve|reject|ban):(\d{1,9})$/.exec(customId);
  if (!m) return json(200, { ok: true, reply: 'Unknown button.' });
  const sql = obj.sql;
  const row = sql.exec('SELECT * FROM units WHERE id = ?', Number.parseInt(m[2], 10)).toArray()[0];
  if (!row) return json(200, { ok: true, reply: 'That unit is gone.' });
  let status = row.status;
  if (m[1] === 'approve') status = 'approved';
  if (m[1] === 'reject') status = 'rejected';
  if (m[1] === 'ban') {
    status = 'banned';
    sql.exec('INSERT OR REPLACE INTO bans (who, at, level) VALUES (?, ?, ?)', row.owner, Date.now(), 0);
    if (row.addr) sql.exec('INSERT OR REPLACE INTO bans (who, at, level) VALUES (?, ?, ?)', row.addr, Date.now(), 0);
    sql.exec("UPDATE units SET status = 'banned', decided = ? WHERE owner = ?", Date.now(), row.owner);
    sql.exec("UPDATE levels SET status = 'banned', decided = ? WHERE owner = ?", Date.now(), row.owner);
  } else {
    sql.exec('UPDATE units SET status = ?, decided = ? WHERE id = ?', status, Date.now(), row.id);
  }
  return json(200, { ok: true, update: { embeds: [unitEmbed(row, status)], components: unitButtons(row, status) } });
}
