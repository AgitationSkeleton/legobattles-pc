// COMMUNITY LEVELS. Players upload levels made in the port's level editor
// (Maps > Level Editor); each waits for the moderator's decision in Discord
// unless auto-approve is on, and approved ones are listed in the game's Maps
// tab. One Durable Object (SQLite) keeps them.
//
//   POST /levels/upload  {key, name, author, map, tbp}   map/tbp: base64 of the
//                        RAW (decompressed) .map and detail tiles
//   GET  /levels/list?offset=&limit=   approved levels, newest first, no data
//   GET  /levels/get?id=N[&count=1]    one approved level with its data (count:
//                                      a download, not a preview)
//   POST /levels/delete  {key, id}     an uploader removes their own level
//   POST /levels/admin                 the Discord bot, with X-Levels-Key
//
// SAFETY. A level is DATA the game parses, never code: the server accepts one
// only when every byte of it parses under the cartridge's .map grammar
// (validateMap below, the same rules as the port's lb_levelmap.cpp), every
// coordinate is on the map, every value is in its known range and the derived
// planes are consistent. Names pass an allow-list and the swear filter. SQL is
// bound parameters only. The preview image Discord shows is drawn here from the
// parsed terrain, not taken from the client. Uploaders are known by the
// sha256 of a 128-bit install key (never the key itself); bans and limits act
// on that and on a hash of the address.
//
// MODERATION. Each submission is posted to the moderation channel by the
// Alien Commander bot's token (secret LEVELS_BOT_TOKEN) as an embed with
// Approve / Reject / Ban buttons; a control panel message holds Auto-approve,
// Uploads on/off and the limits. The buttons are pressed in Discord; the bot
// (on its gateway connection) forwards each press here over /levels/admin with
// the shared secret LEVELS_KEY, and this object checks the presser is the one
// moderator (LEVELS_ADMIN_USER) before acting.

import { DurableObject } from 'cloudflare:workers';
import { hasSwear } from './swear.js';
import { unitsSetup, unitsFetch, unitsAdmin, CATEGORY_NAMES, unitEmbed, unitButtons, unitPost, unitsChannel,
         bundleFiles } from './units.js';
import { postCopies, editCopies } from './modpost.js';
import { hasSlur } from './slur.js';
import { netKey } from './auth.js';
import { verifyToken, isModerator, accountButton, SIGNIN_MESSAGE, discordWho, findAccounts, acctLine } from './auth.js';

const MAX_MAP_BYTES = 200 * 1024;
const MAX_TBP_ENTRIES = 4096;
export const MAX_UPLOAD_BODY = Math.ceil((MAX_MAP_BYTES + 2 + MAX_TBP_ENTRIES * 12) * 4 / 3) + 4096;
const NAME_OK = /^[\w \-'.!?]{1,32}$/u;
const KEY_OK = /^[0-9a-fA-F]{32}$/;
const WORLDS = ['KingTileset', 'PirateTileset', 'MarsTileset'];
const TABLE_METAS = 440;
const DISCORD = 'https://discord.com/api/v10';

const DEFAULTS = {
  auto_approve: 0,       // 1: map uploads go public without a decision
  auto_approve_units: 0, // docs/503: the same for units (unset: as auto_approve)
  uploads: 1,            // 0: uploads refused
  per_owner_day: 3,      // uploads per uploader per day (0: no limit)
  per_hour: 30,          // uploads per hour from everyone (0: no limit)
  max_pending: 50,       // pending submissions at most
  panel_message: '',     // the control panel's Discord message id
};

function json(status, payload) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
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

function cleanText(v, test) {
  const s = String(v == null ? '' : v).trim();
  if (!NAME_OK.test(s)) return null;
  if (hasSwear(s, test)) return null;
  return s;
}

// ------------------------------------------------------------- validation ----
//
// The cartridge's .map (docs/15; port/runner/src/lb_levelmap.h), parsed with a
// bound on every read. Returns {ok, why, map} with the facts the list shows.
export function validateMap(d, tbpCount) {
  let p = 0;
  const fail = (why) => ({ ok: false, why });
  const need = (k) => p + k <= d.length;
  const u8 = () => (need(1) ? d[p++] : -1);
  const tag = (t) => {
    if (!need(t.length)) return false;
    for (let i = 0; i < t.length; ++i) if (d[p + i] !== t.charCodeAt(i)) return false;
    p += t.length;
    return true;
  };
  if (d.length < 64 || d.length > MAX_MAP_BYTES) return fail('size');
  if (!tag('MAP') || !tag('TERR')) return fail('not a map');
  const w = u8(), h = u8(), u6 = u8(), u7 = u8();
  if (w < 32 || w > 128 || h < 32 || h > 128) return fail('map size');
  if (u6 !== 3 || u7 !== 2) return fail('header');
  if (!need(32)) return fail('header');
  let name = '';
  let ended = false;
  for (let i = 0; i < 32; ++i) {
    const c = d[p + i];
    if (c === 0) ended = true;
    else if (ended) return fail('tileset name');
    else name += String.fromCharCode(c);
  }
  p += 32;
  const world = WORLDS.indexOf(name);
  if (world < 0) return fail('tileset');
  const cells = w * h;
  if (!need(cells * 3 + 2)) return fail('terrain');
  const A = d.subarray(p, p + cells);
  p += cells * 2;   // A, then B (tree edges: any byte)
  const C = d.subarray(p, p + cells);
  p += cells;
  for (let i = 0; i < cells; ++i)
    if (A[i] !== 0 && A[i] !== 2 && A[i] !== 3 && A[i] !== 5) return fail('terrain class');
  const dn = d[p] | (d[p + 1] << 8);
  p += 2;
  if (!need(dn)) return fail('tree mask');
  const D = d.subarray(p, p + dn);
  p += dn;
  // The tree mask covers the map exactly.
  let total = 0;
  for (const r of D) total += r;
  if (total !== cells) return fail('tree mask length');
  if (!need(cells * 2)) return fail('tiles');
  for (let i = 0; i < cells; ++i) {
    const e = d[p + i * 2] | (d[p + i * 2 + 1] << 8);
    if (e >= TABLE_METAS + tbpCount) return fail('tile index');
  }
  p += cells * 2;
  if (!tag('RRET')) return fail('terrain end');
  // Plane C must be the area fill of plane A (the editor writes it so).
  const fill = areaFill(A, w, h);
  for (let i = 0; i < cells; ++i) if (fill[i] !== C[i]) return fail('area plane');

  const inMap = (x, y) => x >= 0 && y >= 0 && x < w && y < h;
  let starts = 0;
  const teams = new Set();
  if (!tag('EVNT')) return fail('events');
  while (need(1) && d[p] === 0x4C) {
    ++p;
    const id = u8();
    if (id < 0 || id > 46 || id % 2) return fail('event id');
    const np = u8();
    for (let i = 0; i < np; ++i) if (!inMap(u8(), u8())) return fail('patrol point');
    const area = u8();
    if (area < 0) return fail('event');
    if (area) {
      const x0 = u8(), y0 = u8(), x1 = u8(), y1 = u8();
      if (!inMap(x0, y0) || !inMap(x1, y1)) return fail('event area');
    }
    const ne = u8();
    if (ne < 0 || !need(ne * 9)) return fail('entities');
    let bases = 0;
    const ets = new Set();
    for (let i = 0; i < ne; ++i) {
      const x = d[p], y = d[p + 1], team = d[p + 2], type = d[p + 3], hp = d[p + 5];
      p += 9;
      if (!inMap(x, y) || team > 3 || type > 20 || hp > 100) return fail('entity');
      if (type === 7) ++bases;
      ets.add(team);
    }
    if (bases && ne >= 6 && ets.size === 1) {
      ++starts;
      teams.add([...ets][0]);
    }
    const nk = u8();
    if (nk < 0 || !need(nk * 8)) return fail('pickups');
    for (let i = 0; i < nk; ++i) {
      const x = d[p], y = d[p + 1], type = d[p + 3], sub = d[p + 4];
      p += 8;
      // Only golden bricks: other pickups break Free Play (the map tool's note).
      if (!inMap(x, y) || type !== 20 || sub !== 8) return fail('pickup');
    }
    const nw = u8();
    if (nw < 0 || !need(nw * 3)) return fail('walls');
    for (let i = 0; i < nw; ++i) {
      if (!inMap(d[p], d[p + 1]) || d[p + 2] > 3) return fail('wall');
      p += 3;
    }
  }
  if (u8() !== 0x21) return fail('events end');
  if (!tag('TRIG')) return fail('triggers');
  while (need(1) && d[p] === 0x4C) {
    ++p;
    const id = u8(), flag = u8();
    if (id < 0 || id > 254 || flag < 0) return fail('trigger');
    if (flag) {
      if (!need(10)) return fail('trigger');
      if (!inMap(d[p], d[p + 1]) || !inMap(d[p + 2], d[p + 3])) return fail('trigger area');
      p += 10;
    }
  }
  if (u8() !== 0x21) return fail('triggers end');
  if (!tag('MARK')) return fail('markers');
  while (need(1) && d[p] === 0x4C) {
    ++p;
    const g = u8(), n = u8();
    if (g < 0 || g > 40 || n < 0 || !need(n * 3)) return fail('markers');
    for (let i = 0; i < n; ++i) {
      if (!inMap(d[p], d[p + 1]) || d[p + 2] > 1) return fail('marker');
      p += 3;
    }
  }
  if (u8() !== 0x21) return fail('markers end');
  if (!tag('MINE')) return fail('mines');
  let groups = 0, mines = 0;
  while (need(1) && d[p] === 0x4C) {
    ++p;
    const n = u8();
    if (n < 0 || !need(n * 2)) return fail('mines');
    for (let i = 0; i < n; ++i) {
      if (!inMap(d[p], d[p + 1]) || !inMap(d[p] + 1, d[p + 1] + 1)) return fail('mine');
      p += 2;
    }
    ++groups;
    mines += n;
  }
  if (groups !== 4) return fail('mine groups');
  if (u8() !== 0x21) return fail('mines end');
  if (!tag('PAM') || p !== d.length) return fail('map end');
  if (starts < 2 || teams.size !== starts) return fail('starts');
  return { ok: true, map: { w, h, world, players: starts, mines, A, D } };
}

// Plane C: an EIGHT-connected fill of plane A, grass and stone one family
// (matches all 122 stock maps; lb_levelmap.cpp's area_fill).
export function areaFill(A, w, h) {
  const lab = new Int32Array(w * h).fill(-1);
  const fam = (a) => (a === 0 || a === 2 ? 0 : a + 1);
  let cur = 0;
  const st = [];
  for (let y = 0; y < h; ++y)
    for (let x = 0; x < w; ++x) {
      const i = y * w + x;
      if (lab[i] >= 0) continue;
      const f = fam(A[i]);
      lab[i] = cur;
      st.push(i);
      while (st.length) {
        const c = st.pop();
        const cx = c % w, cy = (c / w) | 0;
        for (let dy = -1; dy <= 1; ++dy)
          for (let dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const j = ny * w + nx;
            if (lab[j] < 0 && fam(A[j]) === f) {
              lab[j] = cur;
              st.push(j);
            }
          }
      }
      ++cur;
    }
  const out = new Uint8Array(w * h);
  for (let i = 0; i < w * h; ++i) out[i] = lab[i] & 0xff;
  return out;
}

export function validateTbp(t) {
  if (!t || t.length === 0) return { ok: true, count: 0 };
  if (t.length < 2) return { ok: false };
  const n = t[0] | (t[1] << 8);
  if (n > MAX_TBP_ENTRIES || t.length !== 2 + n * 12) return { ok: false };
  return { ok: true, count: n };
}

// ---------------------------------------------------------------- preview ----
//
// A PNG of the terrain, 2 px a cell, for the moderator: drawn from the parsed
// planes, never from anything the client drew.
// Each world's terrain in its own tileset's colours (the average of the
// level editor's material swatches, which draw the world's real tiles):
// plane A class 0 grass, 2 stone, 3 water, 5 mountain; then trees.
const WORLD_COLOURS = [
  { 0: [41, 157, 39], 2: [130, 169, 115], 3: [37, 130, 222], 5: [230, 222, 205], trees: [43, 127, 13] },   // King
  { 0: [0, 215, 33], 2: [244, 179, 67], 3: [88, 242, 211], 5: [11, 103, 39], trees: [14, 113, 8] },       // Pirate
  { 0: [150, 32, 26], 2: [184, 107, 45], 3: [123, 221, 61], 5: [98, 32, 16], trees: [36, 92, 55] },        // Mars
];
async function previewPng(A, D, w, h, world) {
  const COLOURS = WORLD_COLOURS[world] || WORLD_COLOURS[0];
  const trees = new Uint8Array(w * h);
  let at = 0, on = 0;
  for (const r of D) {
    for (let k = 0; k < r && at < trees.length; ++k) trees[at++] = on;
    on ^= 1;
  }
  const S = 2, W = w * S, H = h * S;
  const raw = new Uint8Array(H * (1 + W * 3));
  for (let y = 0; y < H; ++y) {
    const row = y * (1 + W * 3);
    raw[row] = 0;
    for (let x = 0; x < W; ++x) {
      const i = ((y / S) | 0) * w + ((x / S) | 0);
      const c = trees[i] ? COLOURS.trees : COLOURS[A[i]] || [0, 0, 0];
      raw.set(c, row + 1 + x * 3);
    }
  }
  const z = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'))).arrayBuffer());
  const crcTable = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; ++n) {
      let c = n;
      for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  const crc = (bytes) => {
    let c = 0xffffffff;
    for (const b of bytes) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const out = new Uint8Array(12 + data.length);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length);
    for (let i = 0; i < 4; ++i) out[4 + i] = type.charCodeAt(i);
    out.set(data, 8);
    dv.setUint32(8 + data.length, crc(out.subarray(4, 8 + data.length)));
    return out;
  };
  const ihdr = new Uint8Array(13);
  const dv = new DataView(ihdr.buffer);
  dv.setUint32(0, W);
  dv.setUint32(4, H);
  ihdr.set([8, 2, 0, 0, 0], 8);
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', z),
                 chunk('IEND', new Uint8Array(0))];
  const total = parts.reduce((n, a) => n + a.length, 0);
  const png = new Uint8Array(total);
  let o = 0;
  for (const a of parts) { png.set(a, o); o += a.length; }
  return png;
}

// ---------------------------------------------------------------- minimap ----
//
// The level's minimap NCGR, as the game's level select shows it: the same
// bytes the port's level editor writes (lb_levelmap.cpp minimap(), the map
// tool's rules), trees drawn, tiled layout. The game decodes it for the
// Community page's thumbnails exactly as it does the map archive's.
export function miniNcgr(A, D, w, h) {
  const trees = new Uint8Array(w * h);
  let at = 0, on = 0;
  for (const r of D) {
    for (let k = 0; k < r && at < trees.length; ++k) trees[at++] = on;
    on ^= 1;
  }
  const a = (x, y) => A[y * w + x];
  const quad = (cx, cy, t) => {
    const ux = cx === 0 ? 0 : cx - 1, uy = cy === 0 ? 0 : cy - 1;
    return a(cx, cy) === t && a(cx, uy) === t && a(ux, cy) === t && a(ux, uy) === t;
  };
  const pixel = (x, y) => {
    const xs = x / 96, ys = y / 96;
    if (xs >= 1 || ys >= 1) return 0;
    const mx = Math.min(w - 1, Math.max(0, Math.floor(w * xs)));
    const my = Math.min(h - 1, Math.max(0, Math.floor(h * ys)));
    if (trees[my * w + mx]) {
      const off = mx + Math.floor(my / 2) * 2;
      return my % 2 === 0 ? (off % 3 !== 0 ? 5 : 4) : (off % 3 === 1 ? 5 : 4);
    }
    const adj = (t, centre, border) => {
      if (!quad(mx, my, t)) return 3;
      const inner = quad(mx, my === 0 ? 0 : my - 1, t) && quad(mx === 0 ? 0 : mx - 1, my, t) &&
                    quad(mx === w - 1 ? w - 1 : mx + 1, my, t) && quad(mx, my === h - 1 ? h - 1 : my + 1, t);
      return inner ? centre : border;
    };
    switch (a(mx, my)) {
      case 0: return quad(mx, my, 0) ? 4 : 3;
      case 3: return adj(3, 7, 6);
      case 5: return adj(5, 2, 1);
      default: return 3;
    }
  };
  const out = new Uint8Array(0x30 + 0x2000 + 0x10);
  const dv = new DataView(out.buffer);
  const tag = (o, s) => { for (let i = 0; i < 4; ++i) out[o + i] = s.charCodeAt(i); };
  tag(0, 'RGCN'); dv.setUint16(4, 0xfeff, true); dv.setUint16(6, 0x0101, true);
  dv.setUint32(8, out.length, true); dv.setUint16(12, 0x10, true); dv.setUint16(14, 2, true);
  tag(0x10, 'RAHC'); dv.setUint32(0x14, 0x2020, true); dv.setUint16(0x18, 0x10, true); dv.setUint16(0x1a, 0x10, true);
  dv.setUint32(0x1c, 3, true); dv.setUint32(0x20, 0, true); dv.setUint32(0x24, 0, true);
  dv.setUint32(0x28, 0x2000, true); dv.setUint32(0x2c, 0x18, true);
  let p = 0x30;
  for (let t = 0; t < 256; ++t)
    for (let r = 0; r < 8; ++r)
      for (let c = 0; c < 8; c += 2) {
        const x = (t % 16) * 8 + c, y = ((t / 16) | 0) * 8 + r;
        out[p++] = pixel(x, y) | (pixel(x + 1, y) << 4);
      }
  tag(p, 'SOPC'); dv.setUint32(p + 4, 0x10, true); dv.setUint32(p + 8, 0, true); dv.setUint32(p + 12, 0x100010, true);
  return out;
}

// ---------------------------------------------------------------- Discord ----
const WORLD_NAMES = ['King', 'Pirate', 'Mars'];

function levelEmbed(row, status) {
  const colour = status === 'approved' ? 0x3ba55d : status === 'rejected' ? 0xed4245
               : status === 'banned' ? 0x8b0000 : 0xfaa61a;
  return {
    title: row.name,
    description: `by ${row.author || 'unknown'}`,
    color: colour,
    fields: [
      { name: 'Players', value: String(row.players), inline: true },
      { name: 'World', value: WORLD_NAMES[row.world] || '?', inline: true },
      { name: 'Size', value: `${row.width} x ${row.height}`, inline: true },
      { name: 'Status', value: status, inline: true },
      { name: 'Uploader', value: uploaderField(row), inline: true },
      { name: 'Level', value: '#' + row.id, inline: true },
    ],
    image: { url: 'attachment://preview.png' },
    timestamp: new Date(row.created).toISOString(),
  };
}
function levelButtons(row, status) {
  if (status !== 'pending')
    return [{ type: 1, components: [
      { type: 2, style: 2, label: status === 'approved' ? 'Unpublish' : 'Approve after all',
        custom_id: `lvl:${status === 'approved' ? 'reject' : 'approve'}:${row.id}` },
      { type: 2, style: 4, label: 'Ban uploader', custom_id: `lvl:ban:${row.id}` },
    ] }];
  return [{ type: 1, components: [
    { type: 2, style: 3, label: 'Approve', custom_id: `lvl:approve:${row.id}` },
    { type: 2, style: 4, label: 'Reject', custom_id: `lvl:reject:${row.id}` },
    { type: 2, style: 2, label: 'Ban uploader', custom_id: `lvl:ban:${row.id}` },
  ] }];
}
function panelMessage(cfg, counts) {
  const onoff = (v) => (v ? 'On' : 'Off');
  return {
    embeds: [{
      title: 'Community levels: settings',
      color: 0x5865f2,
      description:
        `Auto-approve maps: **${onoff(cfg.auto_approve)}**, units: **${onoff(cfg.auto_approve_units)}**\n` +
        `Uploads: **${onoff(cfg.uploads)}**\n` +
        `Per uploader per day: **${cfg.per_owner_day || 'no limit'}**\n` +
        `Per hour from everyone: **${cfg.per_hour || 'no limit'}**\n` +
        `Pending at most: **${cfg.max_pending}**\n\n` +
        `Levels: pending ${counts.pending}, published ${counts.approved}.\n` +
        `Units: pending ${counts.unitsPending}, published ${counts.unitsApproved}.\n` +
        `Banned uploaders ${counts.bans}.`,
    }],
    components: [
      { type: 1, components: [
        { type: 2, style: cfg.auto_approve ? 3 : 2, label: `Auto-approve maps: ${onoff(cfg.auto_approve)}`,
          custom_id: 'lvl:cfg:auto' },
        { type: 2, style: cfg.auto_approve_units ? 3 : 2, label: `Auto-approve units: ${onoff(cfg.auto_approve_units)}`,
          custom_id: 'lvl:cfg:uauto' },
        { type: 2, style: cfg.uploads ? 3 : 4, label: `Uploads: ${onoff(cfg.uploads)}`, custom_id: 'lvl:cfg:uploads' },
        { type: 2, style: 2, label: 'Refresh', custom_id: 'lvl:cfg:refresh' },
      ] },
      { type: 1, components: [
        { type: 2, style: 2, label: 'Per day -', custom_id: 'lvl:cfg:day-' },
        { type: 2, style: 2, label: 'Per day +', custom_id: 'lvl:cfg:day+' },
        { type: 2, style: 2, label: 'Per day: no limit', custom_id: 'lvl:cfg:day0' },
      ] },
      { type: 1, components: [
        { type: 2, style: 2, label: 'Per hour -', custom_id: 'lvl:cfg:hour-' },
        { type: 2, style: 2, label: 'Per hour +', custom_id: 'lvl:cfg:hour+' },
        { type: 2, style: 2, label: 'Per hour: no limit', custom_id: 'lvl:cfg:hour0' },
      ] },
    ],
  };
}

// ------------------------------------------------------------------ object ----
// SG-0149: an upload's account as moderators see it (Discord + LB id).
export function uploaderField(row) {
  if (!row.sub) return '`' + String(row.owner || '').slice(0, 10) + '` (legacy)';
  return discordWho(row.sub, row.uname, '') + (row.uuid ? `\n\`${row.uuid}\`` : '');
}

export class Levels extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.env = env;
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS levels (
        id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT UNIQUE, name TEXT, author TEXT,
        players INTEGER, world INTEGER, width INTEGER, height INTEGER, size INTEGER,
        status TEXT, owner TEXT, addr TEXT, created INTEGER, decided INTEGER,
        message TEXT, downloads INTEGER DEFAULT 0, map BLOB, tbp BLOB)`);
    this.sql.exec('CREATE INDEX IF NOT EXISTS levels_status ON levels (status, id)');
    this.sql.exec('CREATE INDEX IF NOT EXISTS levels_owner ON levels (owner, created)');
    this.sql.exec('CREATE TABLE IF NOT EXISTS bans (who TEXT PRIMARY KEY, at INTEGER, level INTEGER)');
    this.sql.exec('CREATE TABLE IF NOT EXISTS config (k TEXT PRIMARY KEY, v TEXT)');
    // Likes: one per player (install-key hash) per level.
    this.sql.exec('CREATE TABLE IF NOT EXISTS likes (level INTEGER, owner TEXT, PRIMARY KEY (level, owner))');
    for (const col of ['likes INTEGER DEFAULT 0', 'mini BLOB', 'relayed TEXT',
                       "sub TEXT DEFAULT ''", 'legacy INTEGER DEFAULT 0', 'hidden INTEGER DEFAULT 0',
                       "uname TEXT DEFAULT ''", "uuid TEXT DEFAULT ''", "mirror TEXT DEFAULT ''"]) {
      try { this.sql.exec('ALTER TABLE levels ADD COLUMN ' + col); } catch { /* already there */ }
    }
    unitsSetup(this.sql);
    for (const col of ['relayed TEXT', "sub TEXT DEFAULT ''", 'legacy INTEGER DEFAULT 0', 'hidden INTEGER DEFAULT 0',
                       "uname TEXT DEFAULT ''", "uuid TEXT DEFAULT ''", "mirror TEXT DEFAULT ''"]) {
      try { this.sql.exec('ALTER TABLE units ADD COLUMN ' + col); } catch { /* already there */ }
    }
    // docs/501: everything uploaded before Discord sign-in is LEGACY: kept and
    // listed, owned by no account. Done once, when this code first runs.
    if (!this.sql.exec("SELECT v FROM config WHERE k = 'legacy_done'").toArray().length) {
      this.sql.exec("UPDATE levels SET legacy = 1 WHERE sub IS NULL OR sub = ''");
      this.sql.exec("UPDATE units SET legacy = 1 WHERE sub IS NULL OR sub = ''");
      this.sql.exec("INSERT INTO config (k, v) VALUES ('legacy_done', '1')");
    }
  }

  // docs/501: the signed-in player of a request body ({auth}), or null. Their
  // uploads, likes and bans are keyed by owner "d:<discord id>".
  // docs/502: a Discord name with a slur publishes nothing (it was refused
  // at sign-in too; this catches a name changed since the token was made).
  async player(b) {
    // docs/501: publishing takes the ACCOUNT token ("main"), never a play token.
    const c = await verifyToken(this.env, b && b.auth, ['main']);
    if (c && (hasSlur(c.name) || hasSlur(c.un))) return null;
    return c ? { sub: String(c.sub), owner: 'd:' + String(c.sub), name: String(c.name || ''),
                 un: String(c.un || ''), uid: String(c.uid || '') } : null;
  }
  swear(s) { return hasSwear(s, this.env.SWEAR_TEST_WORDS); }

  cfg() {
    const c = { ...DEFAULTS };
    const rows = this.sql.exec('SELECT k, v FROM config').toArray();
    for (const r of rows)
      if (r.k in c) c[r.k] = typeof DEFAULTS[r.k] === 'number' ? Number.parseInt(r.v, 10) || 0 : String(r.v);
    if (!rows.some((r) => r.k === 'auto_approve_units')) c.auto_approve_units = c.auto_approve;   // docs/503
    return c;
  }
  setCfg(k, v) {
    this.sql.exec('INSERT INTO config (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', k, String(v));
  }
  counts() {
    const one = (q, ...a) => this.sql.exec(q, ...a).toArray()[0].n;
    return {
      pending: one("SELECT COUNT(*) AS n FROM levels WHERE status = 'pending'"),
      approved: one("SELECT COUNT(*) AS n FROM levels WHERE status = 'approved'"),
      unitsPending: one("SELECT COUNT(*) AS n FROM units WHERE status = 'pending'"),
      unitsApproved: one("SELECT COUNT(*) AS n FROM units WHERE status = 'approved'"),
      bans: one('SELECT COUNT(*) AS n FROM bans'),
    };
  }

  // security review: fixed windows in memory (per address, per account).
  tooMany(kind, key, count, ms) {
    const now = Date.now();
    if (!this.windows || this.windows.size > 20000) this.windows = new Map();
    const k = kind + ' ' + netKey(key);
    const r = this.windows.get(k);
    if (!r || now - r.at >= ms) { this.windows.set(k, { at: now, n: 1 }); return false; }
    return ++r.n > count;
  }

  // One download counted per address per item per day.
  countOnce(kind, id, request) {
    const day = Math.floor(Date.now() / 86400000);
    if (this.countDay !== day || !this.counted || this.counted.size > 50000) { this.counted = new Set(); this.countDay = day; }
    const k = kind + id + ' ' + netKey(request.headers.get('X-Levels-Addr') || '');
    if (this.counted.has(k)) return false;
    this.counted.add(k);
    return true;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, '');
    const addr = request.headers.get('X-Levels-Addr') || '';
    if (addr && path !== '/levels/admin' && !path.startsWith('/internal/') && this.tooMany('req', addr, 300, 60000))
      return json(429, { ok: false, error: 'slow down' });
    this.req = request;
    try { await this.armShowcase(); } catch { /* never blocks a request */ }
    try {
      if (request.method === 'GET' && path === '/levels/list') return this.list(url);
      if (request.method === 'GET' && path === '/levels/get') return this.get(url);
      if (request.method === 'GET' && path === '/levels/mini') return this.mini(url);
      if (request.method === 'POST' && path === '/levels/like') return await this.like(request);
      if (request.method === 'POST' && path === '/levels/upload') return await this.upload(request);
      if (request.method === 'POST' && path === '/levels/delete') return await this.remove(request);
      if (request.method === 'POST' && path === '/levels/hide') return await this.hide(request);
      if (request.method === 'POST' && path === '/levels/edit') return await this.edit(request);
      if (request.method === 'POST' && path === '/levels/mine') return await this.mine(request);
      if (request.method === 'POST' && path === '/internal/purge') return await this.purge(request);
      if (request.method === 'POST' && path === '/internal/unpurge') return await this.unpurge(request);
      if (request.method === 'POST' && path === '/levels/admin') return await this.admin(request);
      if (path.startsWith('/units/')) return await unitsFetch(this, request, url, path);
    } catch (e) {
      return json(500, { ok: false, error: 'server error' });
    }
    return json(404, { ok: false, error: 'no such endpoint' });
  }

  list(url) {
    const offset = Math.max(0, Math.min(100000, Number.parseInt(url.searchParams.get('offset') || '0', 10) || 0));
    const limit = Math.max(1, Math.min(100, Number.parseInt(url.searchParams.get('limit') || '50', 10) || 50));
    // The order is one of four fixed clauses, never text from the request.
    const ORDERS = {
      new: 'id DESC', old: 'id ASC', downloads: 'downloads DESC, id DESC', likes: 'likes DESC, id DESC',
    };
    const order = ORDERS[url.searchParams.get('sort') || 'new'] || ORDERS.new;
    const rows = this.sql.exec(
      `SELECT id, name, author, players, world, width, height, size, downloads, likes, created, hash
         FROM levels WHERE status = 'approved' AND hidden = 0 ORDER BY ${order} LIMIT ? OFFSET ?`, limit, offset).toArray();
    const total = this.sql.exec("SELECT COUNT(*) AS n FROM levels WHERE status = 'approved' AND hidden = 0").toArray()[0].n;
    return json(200, { ok: true, total, levels: rows });
  }

  // GET /levels/mini?id=N: the level's minimap (NCGR, base64), for the
  // game's thumbnails. Made at upload; made now for a row from before.
  mini(url) {
    const id = Number.parseInt(url.searchParams.get('id') || '', 10);
    if (!Number.isFinite(id) || id < 1) return json(400, { ok: false, error: 'id' });
    const r = this.sql.exec(
      "SELECT id, hash, world, mini, map, tbp FROM levels WHERE id = ? AND status = 'approved'", id).toArray()[0];
    if (!r) return json(404, { ok: false, error: 'no such level' });
    let mini = r.mini ? new Uint8Array(r.mini) : null;
    if (!mini) {
      const tbp = r.tbp ? new Uint8Array(r.tbp) : new Uint8Array(0);
      const v = validateMap(new Uint8Array(r.map), validateTbp(tbp).count || 0);
      if (!v.ok) return json(500, { ok: false, error: 'server error' });
      mini = miniNcgr(v.map.A, v.map.D, v.map.w, v.map.h);
      this.sql.exec('UPDATE levels SET mini = ? WHERE id = ?', mini, id);
    }
    return new Response(JSON.stringify({ ok: true, id: r.id, hash: r.hash, world: r.world, ncgr: bytesB64(mini) }), {
      status: 200,
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'public, max-age=86400' },
    });
  }

  // POST /levels/like {key, id, like}: a player's like on an approved level,
  // set or taken back. One per install key.
  async like(request) {
    let b;
    try { b = JSON.parse(await request.text()); } catch { return json(400, { ok: false, error: 'bad request' }); }
    if (!b || typeof b !== 'object') return json(400, { ok: false, error: 'bad request' });
    const me = await this.player(b);
    if (!me) return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    const owner = me.owner;
    const id = Number.parseInt(b.id, 10);
    if (!Number.isFinite(id) || id < 1) return json(400, { ok: false, error: 'id' });
    if (!this.sql.exec("SELECT 1 FROM levels WHERE id = ? AND status = 'approved' AND hidden = 0", id).toArray().length)
      return json(404, { ok: false, error: 'no such level' });
    if (b.like === false) this.sql.exec('DELETE FROM likes WHERE level = ? AND owner = ?', id, owner);
    else this.sql.exec('INSERT OR IGNORE INTO likes (level, owner) VALUES (?, ?)', id, owner);
    const n = this.sql.exec('SELECT COUNT(*) AS n FROM likes WHERE level = ?', id).toArray()[0].n;
    this.sql.exec('UPDATE levels SET likes = ? WHERE id = ?', n, id);
    return json(200, { ok: true, id, likes: n, liked: b.like !== false });
  }

  get(url) {
    const id = Number.parseInt(url.searchParams.get('id') || '', 10);
    if (!Number.isFinite(id) || id < 1) return json(400, { ok: false, error: 'id' });
    const r = this.sql.exec(
      "SELECT id, name, author, players, world, width, height, hash, map, tbp FROM levels WHERE id = ? AND status = 'approved' AND hidden = 0",
      id).toArray()[0];
    if (!r) return json(404, { ok: false, error: 'no such level' });
    // A download counts; a preview (the game showing a picked level) does not.
    if (url.searchParams.get('count') === '1' && this.countOnce('l', id, this.req))
      this.sql.exec('UPDATE levels SET downloads = downloads + 1 WHERE id = ?', id);
    return json(200, {
      ok: true, id: r.id, name: r.name, author: r.author, players: r.players, world: r.world,
      width: r.width, height: r.height, hash: r.hash,
      map: bytesB64(new Uint8Array(r.map)), tbp: r.tbp ? bytesB64(new Uint8Array(r.tbp)) : '',
    });
  }

  async upload(request) {
    const env = this.env;
    const text = await request.text();
    if (text.length > MAX_UPLOAD_BODY) return json(413, { ok: false, error: 'too large' });
    let b;
    try { b = JSON.parse(text); } catch { return json(400, { ok: false, error: 'bad request' }); }
    if (!b || typeof b !== 'object' || Array.isArray(b)) return json(400, { ok: false, error: 'bad request' });
    const cfg = this.cfg();
    if (!cfg.uploads) return json(403, { ok: false, error: 'uploads are closed for now' });
    const me = await this.player(b);
    if (!me) return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    // security review: attempts per account, counted before any work.
    if (this.tooMany('upload', me.sub, 20, 3600000)) return json(429, { ok: false, error: 'too many uploads; try later' });
    const owner = me.owner;
    const ip = request.headers.get('X-Levels-Addr') || '';
    const addr = ip ? (await sha256hex(new TextEncoder().encode('lvl:' + ip))).slice(0, 32) : '';
    if (this.sql.exec('SELECT 1 FROM bans WHERE who = ? OR who = ?', owner, addr || '-').toArray().length)
      return json(403, { ok: false, error: 'you cannot upload levels' });
    const name = cleanText(b.name, env.SWEAR_TEST_WORDS);
    if (!name) return json(400, { ok: false, error: 'the level name is not allowed' });
    const author = b.author ? cleanText(b.author, env.SWEAR_TEST_WORDS) : '';
    if (author === null) return json(400, { ok: false, error: 'the author name is not allowed' });
    const map = b64bytes(b.map, MAX_MAP_BYTES);
    const tbp = b.tbp ? b64bytes(b.tbp, 2 + MAX_TBP_ENTRIES * 12) : new Uint8Array(0);
    if (!map || !tbp) return json(400, { ok: false, error: 'bad level data' });
    const t = validateTbp(tbp);
    if (!t.ok) return json(400, { ok: false, error: 'bad level data (detail tiles)' });
    const v = validateMap(map, t.count);
    if (!v.ok) return json(400, { ok: false, error: 'bad level data (' + v.why + ')' });
    // Limits.
    const now = Date.now();
    if (cfg.per_owner_day) {
      const n = this.sql.exec('SELECT COUNT(*) AS n FROM levels WHERE owner = ? AND created > ?', owner,
                              now - 86400000).toArray()[0].n;
      if (n >= cfg.per_owner_day) return json(429, { ok: false, error: 'upload limit reached for today' });
      // A new install key is free to make; one address gets three uploaders' worth.
      if (addr) {
        const a = this.sql.exec('SELECT COUNT(*) AS n FROM levels WHERE addr = ? AND created > ?', addr,
                                now - 86400000).toArray()[0].n;
        if (a >= cfg.per_owner_day * 3) return json(429, { ok: false, error: 'upload limit reached for today' });
      }
    }
    if (cfg.per_hour) {
      const n = this.sql.exec('SELECT COUNT(*) AS n FROM levels WHERE created > ?', now - 3600000).toArray()[0].n;
      if (n >= cfg.per_hour) return json(429, { ok: false, error: 'too many uploads right now; try later' });
    }
    if (this.counts().pending >= cfg.max_pending && !cfg.auto_approve)
      return json(429, { ok: false, error: 'the review queue is full; try later' });
    const both = new Uint8Array(map.length + tbp.length);
    both.set(map);
    both.set(tbp, map.length);
    const hash = await sha256hex(both);
    if (this.sql.exec('SELECT 1 FROM levels WHERE hash = ?', hash).toArray().length)
      return json(409, { ok: false, error: 'this level was uploaded before' });
    const status = cfg.auto_approve ? 'approved' : 'pending';
    this.sql.exec(
      `INSERT INTO levels (hash, name, author, players, world, width, height, size, status, owner, addr,
                           created, map, tbp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      hash, name, author || '', v.map.players, v.map.world, v.map.w, v.map.h, map.length + tbp.length, status,
      owner, addr, now, map, tbp.length ? tbp : null);
    this.sql.exec('UPDATE levels SET sub = ?, uname = ?, uuid = ? WHERE hash = ?', me.sub, me.un, me.uid, hash);
    const row = this.sql.exec('SELECT * FROM levels WHERE hash = ?', hash).toArray()[0];
    this.sql.exec('UPDATE levels SET mini = ? WHERE id = ?', miniNcgr(v.map.A, v.map.D, v.map.w, v.map.h), row.id);
    const png = await previewPng(v.map.A, v.map.D, v.map.w, v.map.h, v.map.world);
    const msg = await this.post(levelEmbed(row, status), levelButtons(row, status), png);
    if (msg.id || msg.mirror) this.sql.exec('UPDATE levels SET message = ?, mirror = ? WHERE id = ?', msg.id, msg.mirror, row.id);
    return json(200, { ok: true, id: row.id, status });
  }

  // docs/501: a signed-in player's own uploads (legacy ones belong to nobody).
  async mine(request) {
    let b;
    try { b = JSON.parse(await request.text()); } catch { return json(400, { ok: false, error: 'bad request' }); }
    const me = await this.player(b);
    if (!me) return json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE });
    const levels = this.sql.exec(
      `SELECT id, name, author, players, world, width, height, status, hidden, downloads, likes, created
         FROM levels WHERE sub = ? AND status != 'deleted' ORDER BY id DESC LIMIT 200`, me.sub).toArray();
    const units = this.sql.exec(
      `SELECT id, name, author, credits, category, status, hidden, downloads, likes, created
         FROM units WHERE sub = ? AND status != 'deleted' ORDER BY id DESC LIMIT 200`, me.sub).toArray();
    return json(200, { ok: true, levels, units });
  }

  async ownRow(request, table) {
    let b;
    try { b = JSON.parse(await request.text()); } catch { return { err: json(400, { ok: false, error: 'bad request' }) }; }
    const me = await this.player(b);
    if (!me) return { err: json(401, { ok: false, error: SIGNIN_MESSAGE, reason: 'signin', message: SIGNIN_MESSAGE }) };
    const id = Number.parseInt(b && b.id, 10);
    const row = this.sql.exec(`SELECT * FROM ${table} WHERE id = ? AND sub = ? AND status != 'deleted'`, id, me.sub).toArray()[0];
    if (!row) return { err: json(404, { ok: false, error: 'no such upload of yours' }) };
    return { b, me, row };
  }

  // Deleting keeps the row (its showcase post is taken down by the sync) but
  // drops the data.
  async remove(request) {
    const { err, row } = await this.ownRow(request, 'levels');
    if (err) return err;
    this.sql.exec("UPDATE levels SET status = 'deleted', hidden = 1, decided = ? WHERE id = ?", Date.now(), row.id);
    return json(200, { ok: true });
  }

  async hide(request) {
    const { err, b, row } = await this.ownRow(request, 'levels');
    if (err) return err;
    this.sql.exec('UPDATE levels SET hidden = ? WHERE id = ?', b.hidden === false ? 0 : 1, row.id);
    return json(200, { ok: true, hidden: b.hidden !== false });
  }

  // Name and author; the map itself is changed by uploading it again.
  async edit(request) {
    const { err, b, row } = await this.ownRow(request, 'levels');
    if (err) return err;
    const name = b.name !== undefined ? cleanText(b.name, this.env.SWEAR_TEST_WORDS) : row.name;
    if (!name) return json(400, { ok: false, error: 'the level name is not allowed' });
    const author = b.author !== undefined ? (b.author ? cleanText(b.author, this.env.SWEAR_TEST_WORDS) : '') : row.author;
    if (author === null) return json(400, { ok: false, error: 'the author name is not allowed' });
    this.sql.exec('UPDATE levels SET name = ?, author = ? WHERE id = ?', name, author || '', row.id);
    // security review: a renamed published level goes back to the moderators.
    if (row.status === 'approved' && (name !== row.name || (author || '') !== (row.author || '')) && !this.cfg().auto_approve) {
      this.sql.exec("UPDATE levels SET status = 'pending' WHERE id = ?", row.id);
      this.ctx.waitUntil(this.syncMessage('map', { ...row, name, author: author || '', status: 'pending' }, 'pending'));
    }
    // A published level's showcase post is made again with the new name.
    if (row.status === 'approved' && row.relayed && row.relayed !== 'err') {
      const old = row.relayed;
      this.sql.exec('UPDATE levels SET relayed = NULL WHERE id = ?', row.id);
      this.ctx.waitUntil(fetch(`${this.env.DISCORD_API_BASE || DISCORD}/channels/${this.env.SHOWCASE_CHANNEL}/messages/${old}`,
        { method: 'DELETE', headers: { Authorization: 'Bot ' + this.env.LEVELS_BOT_TOKEN } }).catch(() => null));
    }
    return json(200, { ok: true, name, author: author || '' });
  }

  // A banned account's uploads come down (from the Accounts object).
  async purge(request) {
    let b = {};
    try { b = JSON.parse(await request.text()); } catch { /* empty */ }
    const sub = String(b.sub || '');
    if (!/^[0-9]{5,25}$/.test(sub)) return json(400, { ok: false });
    const now = Date.now();
    this.sql.exec("UPDATE levels SET status = 'banned', decided = ? WHERE sub = ? AND status != 'deleted'", now, sub);
    this.sql.exec("UPDATE units SET status = 'banned', decided = ? WHERE sub = ? AND status != 'deleted'", now, sub);
    this.sql.exec('INSERT OR REPLACE INTO bans (who, at, level) VALUES (?, ?, 0)', 'd:' + sub, now);
    return json(200, { ok: true });
  }

  // An unban lifts the upload ban; what the ban took down goes back to the
  // moderators as pending (an approve puts it up again).
  async unpurge(request) {
    let b = {};
    try { b = JSON.parse(await request.text()); } catch { /* empty */ }
    const sub = String(b.sub || '');
    if (!/^[0-9]{5,25}$/.test(sub)) return json(400, { ok: false });
    this.sql.exec('DELETE FROM bans WHERE who = ?', 'd:' + sub);
    this.sql.exec("UPDATE levels SET status = 'pending' WHERE sub = ? AND status = 'banned'", sub);
    this.sql.exec("UPDATE units SET status = 'pending' WHERE sub = ? AND status = 'banned'", sub);
    return json(200, { ok: true });
  }

  // ------------------------------------------------------------ showcase --
  // Every APPROVED level and unit is also posted,
  // without buttons, to a public channel (SHOWCASE_CHANNEL); one that stops
  // being approved (unpublished, banned) has that post deleted. An alarm keeps
  // the two in step a few items at a time, so Discord's per-channel rate limit
  // is respected, and the first runs after this was deployed forwarded every
  // level and unit that was already approved (their `relayed` was empty).
  // `relayed` holds the showcase message id, or 'err' after a refused post
  // (not retried).
  showcaseDue() {
    const q = (t) => this.sql.exec(
      `SELECT COUNT(*) AS n FROM ${t} WHERE (status = 'approved' AND hidden = 0 AND relayed IS NULL)
         OR (NOT (status = 'approved' AND hidden = 0) AND relayed IS NOT NULL AND relayed != 'err')`).toArray()[0].n;
    return q('levels') + q('units');
  }

  async armShowcase() {
    const env = this.env;
    if (!env.LEVELS_BOT_TOKEN || !env.SHOWCASE_CHANNEL) return;
    if (!this.showcaseDue()) return;
    if ((await this.ctx.storage.getAlarm()) == null) await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  async alarm() {
    const env = this.env;
    if (!env.LEVELS_BOT_TOKEN || !env.SHOWCASE_CHANNEL) return;
    let wait;
    try { wait = await this.syncShowcase(4); } catch { wait = 30000; }
    if (wait >= 0) await this.ctx.storage.setAlarm(Date.now() + Math.max(1500, wait));
  }

  // Up to `n` steps; returns ms to wait before the next run, or -1 when done.
  async syncShowcase(n) {
    const env = this.env;
    for (let i = 0; i < n; ++i) {
      // Deletions first, so an unpublished item leaves the channel promptly.
      let deleted = false;
      for (const table of ['levels', 'units']) {
        const gone = this.sql.exec(`SELECT id, relayed FROM ${table}
            WHERE NOT (status = 'approved' AND hidden = 0) AND relayed IS NOT NULL AND relayed != 'err' LIMIT 1`).toArray()[0];
        if (!gone) continue;
        const r = await fetch(`${env.DISCORD_API_BASE || DISCORD}/channels/${env.SHOWCASE_CHANNEL}/messages/${gone.relayed}`, {
          method: 'DELETE', headers: { Authorization: 'Bot ' + env.LEVELS_BOT_TOKEN } });
        if (r.status === 429) return retryAfter(r);
        this.sql.exec(`UPDATE ${table} SET relayed = NULL WHERE id = ?`, gone.id);
        deleted = true;
        break;
      }
      if (deleted) continue;
      // Then the oldest approved item without a post, levels and units by age.
      const lv = this.sql.exec("SELECT * FROM levels WHERE status = 'approved' AND hidden = 0 AND relayed IS NULL ORDER BY created LIMIT 1").toArray()[0];
      const un = this.sql.exec("SELECT * FROM units WHERE status = 'approved' AND hidden = 0 AND relayed IS NULL ORDER BY created LIMIT 1").toArray()[0];
      if (!lv && !un) return -1;
      const table = lv && (!un || lv.created <= un.created) ? 'levels' : 'units';
      const row = table === 'levels' ? lv : un;
      const r = table === 'levels' ? await this.showcaseLevel(row) : await this.showcaseUnit(row);
      if (r.status === 429) return retryAfter(r);
      let id = 'err';
      if (r.ok) { const m = await r.json().catch(() => null); if (m && m.id) id = String(m.id); }
      this.sql.exec(`UPDATE ${table} SET relayed = ? WHERE id = ?`, id, row.id);
    }
    return this.showcaseDue() ? 1500 : -1;
  }

  async showcaseLevel(row) {
    const tbp = row.tbp ? new Uint8Array(row.tbp) : new Uint8Array(0);
    const v = validateMap(new Uint8Array(row.map), validateTbp(tbp).count || 0);
    const png = v.ok ? await previewPng(v.map.A, v.map.D, v.map.w, v.map.h, v.map.world) : null;
    return this.showcasePost({
      title: row.name,
      description: `Community level by ${row.author || 'unknown'}`,
      color: 0x3ba55d,
      fields: [
        { name: 'Players', value: String(row.players), inline: true },
        { name: 'World', value: WORLD_NAMES[row.world] || '?', inline: true },
        { name: 'Size', value: `${row.width} x ${row.height}`, inline: true },
      ],
      timestamp: new Date(row.created).toISOString(),
    }, png);
  }

  showcaseUnit(row) {
    return this.showcasePost({
      title: row.name,
      description: `Custom unit by ${row.author || 'unknown'}${row.credits ? ` (credits: ${row.credits})` : ''}`,
      color: 0x3ba55d,
      fields: [
        { name: 'Kind', value: CATEGORY_NAMES[row.category] || '?', inline: true },
        { name: 'Sounds', value: String(row.sounds), inline: true },
      ],
      timestamp: new Date(row.created).toISOString(),
    }, row.thumb ? new Uint8Array(row.thumb) : null);
  }

  // No components: the showcase is for looking, not moderating.
  showcasePost(embed, png) {
    const env = this.env;
    const form = new FormData();
    if (png) embed.image = { url: 'attachment://preview.png' };
    form.append('payload_json', JSON.stringify({
      embeds: [embed], components: [],
      attachments: png ? [{ id: 0, filename: 'preview.png' }] : [],
      allowed_mentions: { parse: [] },
    }));
    if (png) form.append('files[0]', new Blob([png], { type: 'image/png' }), 'preview.png');
    return fetch(`${env.DISCORD_API_BASE || DISCORD}/channels/${env.SHOWCASE_CHANNEL}/messages`, {
      method: 'POST', headers: { Authorization: 'Bot ' + env.LEVELS_BOT_TOKEN }, body: form,
    });
  }

  // A map's moderation message, in the maps channel and the mirror
  // (docs/503): { id, mirror }, '' for a copy not posted.
  async post(embed, components, png) {
    return postCopies(this.env, this.env.LEVELS_CHANNEL, () => {
      const form = new FormData();
      form.append('payload_json', JSON.stringify({
        embeds: [embed], components, attachments: [{ id: 0, filename: 'preview.png' }],
        allowed_mentions: { parse: [] },
      }));
      form.append('files[0]', new Blob([png], { type: 'image/png' }), 'preview.png');
      return { body: form, json: false };
    });
  }

  // ------------------------------------------------- docs/503: commands --
  // The bot's moderation slash commands: {user_id, channel_id, command, args}.
  // Map commands work in the maps channel, unit commands in the units
  // channel, account commands in the accounts channel; the mirror channel
  // takes them all. The presser was checked to be a moderator already.
  sectionOf(channelId) {
    const c = String(channelId || ''), e = this.env;
    if (!c) return '';
    if (c === String(e.MIRROR_CHANNEL || '')) return 'all';
    if (c === String(e.LEVELS_CHANNEL || '')) return 'map';
    if (c === String(e.UNITS_CHANNEL || '')) return 'unit';
    if (c === String(e.ACCOUNTS_CHANNEL || '')) return 'account';
    return '';
  }

  async syncMessage(kind, row, status) {
    const env = this.env;
    if (kind === 'map')
      await editCopies(env, env.LEVELS_CHANNEL, row.message, row.mirror,
                       { embeds: [levelEmbed(row, status)], components: levelButtons(row, status) });
    else
      await editCopies(env, unitsChannel(env), row.message, row.mirror,
                       { embeds: [unitEmbed(row, status)], components: unitButtons(row, status) });
  }

  // A waiting upload posted again (into the channels as they are now).
  async repost(kind, row) {
    let ids = { id: '', mirror: '' };
    if (kind === 'map') {
      const tbp = row.tbp ? new Uint8Array(row.tbp) : new Uint8Array(0);
      const v = validateMap(new Uint8Array(row.map), validateTbp(tbp).count || 0);
      if (!v.ok) return false;
      const png = await previewPng(v.map.A, v.map.D, v.map.w, v.map.h, v.map.world);
      ids = await this.post(levelEmbed(row, row.status), levelButtons(row, row.status), png);
    } else {
      const files = bundleFiles(row.bundle);
      const sheet = (files.find(([p]) => p.startsWith('sheets/') && p.endsWith('.png')) || [])[1];
      if (!sheet) return false;
      const sounds = files.filter(([p]) => p.startsWith('sounds/') && p.endsWith('.wav')).slice(0, 9)
        .map(([p, d]) => ({ name: p.slice(7, -4).replace(/[^A-Za-z0-9_]/g, '_').slice(0, 40) || 'sound', wav: d }));
      ids = await unitPost(this.env, unitEmbed(row, row.status), unitButtons(row, row.status), sheet, sounds);
    }
    if (!ids.id && !ids.mirror) return false;
    this.sql.exec(`UPDATE ${kind === 'map' ? 'levels' : 'units'} SET message = ?, mirror = ? WHERE id = ?`,
                  ids.id, ids.mirror, row.id);
    return true;
  }

  async resolveAccount(who) {
    const w = String(who || '').trim().replace(/^<@!?(\d+)>$/, '$1');
    if (!w) return { error: 'Say who: a Discord id, a mention, an LB- account id or a name.' };
    const found = await findAccounts(this.env, w);
    if (/^[0-9]{5,25}$/.test(w)) return { sub: w, acct: found.find((a) => a.sub === w) || null };
    const exact = found.filter((a) => a.uid.toLowerCase() === w.toLowerCase() ||
                                      String(a.username || '').toLowerCase() === w.toLowerCase());
    if (exact.length === 1) return { sub: exact[0].sub, acct: exact[0] };
    if (found.length === 1) return { sub: found[0].sub, acct: found[0] };
    if (!found.length) return { error: `No account matches "${w}".` };
    return { error: `"${w}" matches ${found.length} accounts; use the Discord id:\n` +
                    found.slice(0, 10).map(acctLine).join('\n') };
  }

  async command(b) {
    const env = this.env;
    const at = this.sectionOf(b.channel_id);
    const a = b.args && typeof b.args === 'object' ? b.args : {};
    const say = (t) => json(200, { ok: true, reply: String(t).slice(0, 1900) });
    if (!at) return say('These commands work only in the moderation channels.');
    const cmd = String(b.command || '');
    const here = (s) => at === 'all' || at === s;

    if (['users', 'user', 'ban', 'unban', 'purge'].includes(cmd)) {
      if (!here('account')) return say('Account commands work in the accounts channel.');
      if (cmd === 'users') {
        const found = await findAccounts(env, a.query);
        return say(found.length ? found.slice(0, 15).map(acctLine).join('\n') : 'No account matches.');
      }
      const who = await this.resolveAccount(a.who);
      if (who.error) return say(who.error);
      if (cmd === 'ban' || cmd === 'unban') {
        const r = await accountButton(env, cmd, who.sub, b.user_id, String(a.reason || '').slice(0, 200));
        return say(r.message);
      }
      const nl = this.sql.exec("SELECT COUNT(*) AS n FROM levels WHERE sub = ? AND status != 'deleted'", who.sub).toArray()[0].n;
      const nu = this.sql.exec("SELECT COUNT(*) AS n FROM units WHERE sub = ? AND status != 'deleted'", who.sub).toArray()[0].n;
      if (cmd === 'user')
        return say((who.acct ? acctLine(who.acct) : `<@${who.sub}> \`${who.sub}\` (never signed in)`) +
                   `\nUploads: ${nl} map(s), ${nu} unit(s).`);
      // purge: every upload of theirs deleted, no ban
      for (const [kind, table] of [['map', 'levels'], ['unit', 'units']]) {
        const rows = this.sql.exec(`SELECT * FROM ${table} WHERE sub = ? AND status != 'deleted'`, who.sub).toArray();
        this.sql.exec(`UPDATE ${table} SET status = 'deleted', hidden = 1, decided = ? WHERE sub = ? AND status != 'deleted'`,
                      Date.now(), who.sub);
        for (const r of rows.slice(0, 20)) this.ctx.waitUntil(this.syncMessage(kind, r, 'deleted'));
      }
      return say(`Deleted ${nl} map(s) and ${nu} unit(s) of <@${who.sub}>.`);
    }

    let kind = String(a.kind || '');
    if (kind !== 'map' && kind !== 'unit') kind = at === 'map' || at === 'unit' ? at : '';
    if (!kind) return say('Say which: map or unit.');
    if (!here(kind)) return say(`${kind === 'map' ? 'Map' : 'Unit'} commands work in the ${kind}s channel.`);
    const table = kind === 'map' ? 'levels' : 'units';
    const Kind = kind === 'map' ? 'Map' : 'Unit';

    if (cmd === 'autoapprove') {
      const on = a.on === true || a.on === 1 || a.on === 'on';
      this.setCfg(kind === 'map' ? 'auto_approve' : 'auto_approve_units', on ? 1 : 0);
      return say(`Auto-approve for ${kind}s is now **${on ? 'on' : 'off'}**.`);
    }
    if (['approve', 'unapprove', 'reject', 'delete'].includes(cmd)) {
      const id = Number.parseInt(a.id, 10);
      const row = this.sql.exec(`SELECT * FROM ${table} WHERE id = ?`, Number.isFinite(id) ? id : -1).toArray()[0];
      if (!row) return say(`No ${kind} #${a.id}.`);
      const status = { approve: 'approved', unapprove: 'pending', reject: 'rejected', delete: 'deleted' }[cmd];
      this.sql.exec(`UPDATE ${table} SET status = ?, decided = ?${cmd === 'delete' ? ', hidden = 1' : ''} WHERE id = ?`,
                    status, Date.now(), row.id);
      this.ctx.waitUntil(this.syncMessage(kind, { ...row, status }, status));
      const word = { approve: 'approved (public)', unapprove: 'back in the queue (not public)',
                     reject: 'rejected', delete: 'deleted' }[cmd];
      return say(`${Kind} #${row.id} "${row.name}" is ${word}.`);
    }
    if (cmd === 'queue') {
      const rows = this.sql.exec(`SELECT * FROM ${table} WHERE status = 'pending' ORDER BY id LIMIT 25`).toArray();
      if (!rows.length) return say(`No ${kind}s are waiting.`);
      let posted = 0;
      if (a.repost) for (const r of rows) if (await this.repost(kind, r)) ++posted;
      return say(`${rows.length} ${kind}(s) waiting` + (a.repost ? `; posted again: ${posted}` : '') + ':\n' +
                 rows.map((r) => `#${r.id} "${r.name}"` + (r.sub ? ` by <@${r.sub}>` : ' (legacy)')).join('\n'));
    }
    return say('Unknown command.');
  }


  // The bot forwards a button press: {user_id, custom_id}. The reply tells the
  // bot how to update the message ({update: {embeds, components}}) or what to
  // say ({reply}).
  async admin(request) {
    const env = this.env;
    const key = request.headers.get('X-Levels-Key') || '';
    if (!env.LEVELS_KEY || !timingSafeEqual(key, env.LEVELS_KEY)) return json(403, { ok: false, error: 'forbidden' });
    let b;
    try { b = JSON.parse(await request.text()); } catch { return json(400, { ok: false, error: 'bad request' }); }
    // docs/501: the presser must be one of the moderators (var MODERATORS);
    // an unset list matches nobody.
    if (!b || !isModerator(env, b.user_id))
      return json(200, { ok: true, reply: 'Only a moderator can use these buttons.' });
    if (b.command) return this.command(b);   // docs/503: a slash command
    const id = String(b.custom_id || '');
    const acct = /^lvl:acct:(ban|unban):([0-9]{5,25})$/.exec(id);
    if (acct) {
      const r = await accountButton(env, acct[1], acct[2], b.user_id);
      return json(200, { ok: true, reply: r.message });
    }
    const cfg = this.cfg();
    if (id === 'lvl:panel') return json(200, { ok: true, post: panelMessage(cfg, this.counts()) });
    let m = /^lvl:cfg:(auto|uauto|uploads|refresh|day[-+0]|hour[-+0])$/.exec(id);
    if (m) {
      const k = m[1];
      if (k === 'auto') this.setCfg('auto_approve', cfg.auto_approve ? 0 : 1);
      if (k === 'uauto') this.setCfg('auto_approve_units', cfg.auto_approve_units ? 0 : 1);
      if (k === 'uploads') this.setCfg('uploads', cfg.uploads ? 0 : 1);
      if (k === 'day-') this.setCfg('per_owner_day', Math.max(1, (cfg.per_owner_day || 4) - 1));
      if (k === 'day+') this.setCfg('per_owner_day', Math.min(50, (cfg.per_owner_day || 0) + 1));
      if (k === 'day0') this.setCfg('per_owner_day', 0);
      if (k === 'hour-') this.setCfg('per_hour', Math.max(1, (cfg.per_hour || 11) - 5));
      if (k === 'hour+') this.setCfg('per_hour', Math.min(500, (cfg.per_hour || 0) + 5));
      if (k === 'hour0') this.setCfg('per_hour', 0);
      return json(200, { ok: true, update: panelMessage(this.cfg(), this.counts()) });
    }
    if (id.startsWith('lvl:unit:')) return unitsAdmin(this, id, b.user_id);
    m = /^lvl:(approve|reject|ban):(\d{1,9})$/.exec(id);
    if (!m) return json(200, { ok: true, reply: 'Unknown button.' });
    const row = this.sql.exec('SELECT * FROM levels WHERE id = ?', Number.parseInt(m[2], 10)).toArray()[0];
    if (!row) return json(200, { ok: true, reply: 'That level is gone.' });
    let status = row.status;
    if (m[1] === 'approve') status = 'approved';
    if (m[1] === 'reject') status = 'rejected';
    if (m[1] === 'ban' && row.sub) {
      // A signed-in uploader: the account is banned, which takes down
      // everything of theirs (Accounts -> purge).
      await accountButton(env, 'ban', row.sub, b.user_id);
      status = 'banned';
      this.sql.exec("UPDATE levels SET status = 'banned', decided = ? WHERE id = ?", Date.now(), row.id);
    } else if (m[1] === 'ban') {
      status = 'banned';
      this.sql.exec('INSERT OR REPLACE INTO bans (who, at, level) VALUES (?, ?, ?)', row.owner, Date.now(), row.id);
      if (row.addr) this.sql.exec('INSERT OR REPLACE INTO bans (who, at, level) VALUES (?, ?, ?)', row.addr, Date.now(), row.id);
      // Everything that uploader sent comes down.
      this.sql.exec("UPDATE levels SET status = 'banned', decided = ? WHERE owner = ?", Date.now(), row.owner);
      this.sql.exec("UPDATE units SET status = 'banned', decided = ? WHERE owner = ?", Date.now(), row.owner);
    } else {
      this.sql.exec('UPDATE levels SET status = ?, decided = ? WHERE id = ?', status, Date.now(), row.id);
    }
    const update = { embeds: [levelEmbed(row, status)], components: levelButtons(row, status) };
    this.ctx.waitUntil(editCopies(env, env.LEVELS_CHANNEL, row.message, row.mirror, update));   // docs/503
    return json(200, { ok: true, update });
  }
}

async function retryAfter(r) {
  const b = await r.json().catch(() => null);
  const s = b && typeof b.retry_after === 'number' ? b.retry_after : 5;
  return Math.min(60000, Math.ceil(s * 1000) + 250);
}

function timingSafeEqual(a, b) {
  const x = new TextEncoder().encode(String(a)), y = new TextEncoder().encode(String(b));
  if (x.length !== y.length) return false;
  let d = 0;
  for (let i = 0; i < x.length; ++i) d |= x[i] ^ y[i];
  return d === 0;
}
