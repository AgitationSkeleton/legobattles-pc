// docs/502: SLURS, AND THE WAYS THEY ARE DISGUISED. The same matcher as the
// Python server's (services/master-server/master_server.py, slur_keys and
// has_slur): fold the text -- accents and look-alike letters from other
// alphabets to a-z, digits and symbols to the letters they stand for,
// separators dropped, runs of one letter shortened -- and look every
// substring up among salted hashes of slur stems (slur_words.js, made by
// tools/lb_slur_hash.py). A second reading also takes l for i and q for g.
// No stem is in this file or the repository.
import { SLUR_MIN, SLUR_MAX, SLUR_HASHES, SLUR_ALLOW_MIN, SLUR_ALLOW_MAX, SLUR_ALLOW_HASHES } from './slur_words.js';
import { SLUR_FOLD_EXTRA } from './slur_fold.js';
const MAX_TEXT = 512;   // characters of one text looked at (the check is not free)

const SALT = 'lbslur1:';
const MASK = (1n << 64n) - 1n;
const FOLD = {
  'ı': 'i', 'і': 'i', 'ї': 'i', 'ӏ': 'i', 'ɪ': 'i', 'ι': 'i', '¡': 'i', '|': 'i', '!': 'i', '1': 'i',
  'а': 'a', 'α': 'a', '@': 'a', '4': 'a', 'ä': 'a',
  'е': 'e', 'ё': 'e', 'є': 'e', 'ε': 'e', '3': 'e', '€': 'e',
  'о': 'o', 'ο': 'o', 'σ': 'o', 'ө': 'o', '0': 'o', 'ø': 'o',
  'р': 'p', 'ρ': 'p', 'с': 'c', 'ϲ': 'c', 'х': 'x', 'χ': 'x', 'у': 'y', 'ү': 'y', 'γ': 'y',
  'к': 'k', 'κ': 'k', 'ѕ': 's', '$': 's', '5': 's', '§': 's', 'ԁ': 'd', 'đ': 'd',
  'ɡ': 'g', 'ǥ': 'g', 'ԍ': 'g', '9': 'g', '6': 'g', 'т': 't', 'τ': 't', '7': 't', '+': 't',
  'в': 'b', 'β': 'b', '8': 'b', 'н': 'h', 'м': 'm', 'п': 'n', 'η': 'n', 'ո': 'n', 'ŋ': 'n',
  'ν': 'v', 'ω': 'w', 'ш': 'w', 'ł': 'l', 'ℓ': 'l', 'υ': 'u',
  'ß': 'ss', 'æ': 'ae', 'œ': 'oe',
};
const VOWELS = new Set('aeiouy');

function collapse(t) {
  const out = [];
  for (const c of t) {
    const n = out.length;
    if (n && out[n - 1] === c && (VOWELS.has(c) || (n > 1 && out[n - 2] === c))) continue;
    out.push(c);
  }
  return out.join('');
}

// The spans [i, j) of `v` that are ordinary words containing a stem.
function allowed(v) {
  const spans = [];
  for (let n = SLUR_ALLOW_MIN; n <= SLUR_ALLOW_MAX; ++n)
    for (let i = 0; i + n <= v.length; ++i)
      if (SLUR_ALLOW_HASHES.has(slurHash(v.slice(i, i + n)))) spans.push([i, i + n]);
  return spans;
}

// collapse(), with each kept letter's index in `v`.
function collapseMap(v) {
  const out = [], idx = [];
  for (let i = 0; i < v.length; ++i) {
    const c = v[i], n = out.length;
    if (n && out[n - 1] === c && (VOWELS.has(c) || (n > 1 && out[n - 2] === c))) continue;
    out.push(c);
    idx.push(i);
  }
  return [out.join(''), idx];
}

function readings(text) {
  let base = '';
  for (const ch of String(text == null ? '' : text).slice(0, MAX_TEXT).normalize('NFKD')) {
    if (/\p{M}/u.test(ch)) continue;
    let c = ch.toLowerCase();
    c = FOLD[c] !== undefined ? FOLD[c] : SLUR_FOLD_EXTRA[c] !== undefined ? SLUR_FOLD_EXTRA[c] : c;
    base += /^[a-z]+$/.test(c) ? c : '';
  }
  const out = [];
  const seen = new Set();
  for (const v of [base, base.replace(/l/g, 'i').replace(/q/g, 'g')]) {
    if (seen.has(v)) continue;
    seen.add(v);
    const [k, idx] = collapseMap(v);
    out.push([k, idx, allowed(v)]);
  }
  return out;
}

export function slurKeys(text) { return readings(text).map((r) => r[0]); }

function slurHash(s) {
  let h = 0xCBF29CE484222325n;
  for (const t of [SALT, s]) {
    for (let i = 0; i < t.length; ++i) {
      h ^= BigInt(t.charCodeAt(i) & 0xFF);
      h = (h * 0x100000001B3n) & MASK;
    }
  }
  return h;
}

// True when a reading holds a stem that is not wholly inside an allowed word.
export function hasSlur(text) {
  if (!SLUR_HASHES.size) return false;
  for (const [k, idx, spans] of readings(text))
    for (let n = SLUR_MIN; n <= SLUR_MAX; ++n)
      for (let i = 0; i + n <= k.length; ++i)
        if (SLUR_HASHES.has(slurHash(k.slice(i, i + n)))) {
          const a = idx[i], b = idx[i + n - 1];
          if (!spans.some(([s, e]) => s <= a && b < e)) return true;
        }
  return false;
}

export const NAME_REFUSED = 'That name is not allowed on the online servers. ' +
  'Change your profile name (or the lobby\'s) and try again.';
