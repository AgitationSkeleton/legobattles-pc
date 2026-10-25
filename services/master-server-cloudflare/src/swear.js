// SG-0018: the game's swear filter (port/runner/src/lb_chat.cpp, docs/415), for
// names only: hasSwear(name) is true exactly when the game would turn part of
// it into "ziff". The score tables refuse such a name altogether. The hashes
// are in swear_words.js (tools/lb_swear_js.py); no word is in either file.

import { ROOT_MIN, ROOT_MAX, ROOTS, WORDS } from './swear_words.js';

const MASK = (1n << 64n) - 1n;
const SALT = 'lbziff1:';

function wordHash(s) {
  let h = 0xCBF29CE484222325n;
  for (const t of [SALT, s]) {
    for (let i = 0; i < t.length; ++i) {
      h ^= BigInt(t.charCodeAt(i) & 0xFF);
      h = (h * 0x100000001B3n) & MASK;
    }
  }
  return h;
}

// lb_chat.cpp letter(): a letter for matching, or '' for a separator.
function letter(c) {
  if (c >= 'a' && c <= 'z') return c;
  if (c >= 'A' && c <= 'Z') return c.toLowerCase();
  switch (c) {
    case '4': case '@': return 'a';
    case '3': return 'e';
    case '1': case '!': return 'i';
    case '0': return 'o';
    case '5': case '$': return 's';
    case '7': case '+': return 't';
    default: return '';
  }
}

// SWEAR_TEST_WORDS=a,b,c (a test's `wrangler dev --var`, like the game's
// LB_SWEAR_TEST_WORDS): whole words an innocent name can stand in for.
let testWords = new Set();

function isWord(t) {
  if (!t) return false;
  const h = wordHash(t);
  return WORDS.has(h) || ROOTS.has(h) || testWords.has(t);
}

function collapse(t) {
  let o = '';
  for (const c of t) if (!o || o[o.length - 1] !== c) o += c;
  return o;
}

const ENDINGS = ['ing', 'in', 'ers', 'er', 'ed', 'es', 's', 'y', 'ies'];

function wholeWord(t) {
  for (const v of [t, collapse(t)]) {
    if (isWord(v)) return true;
    for (const e of ENDINGS)
      if (v.endsWith(e) && v.length > e.length + 1 && isWord(v.slice(0, v.length - e.length)))
        return true;
  }
  return false;
}

export function hasSwear(name, test) {
  testWords = new Set(String(test || '').toLowerCase().split(',').filter(Boolean));
  const s = String(name == null ? '' : name);
  const norm = [...s].map(letter);
  let i = 0;
  while (i < norm.length) {
    if (!norm[i]) { ++i; continue; }
    let j = i;
    while (j < norm.length && norm[j]) ++j;
    const tok = norm.slice(i, j).join('');
    if (wholeWord(tok)) return true;
    for (let a = 0; a < tok.length; ++a)
      for (let len = ROOT_MAX; len >= ROOT_MIN; --len)
        if (a + len <= tok.length && ROOTS.has(wordHash(tok.slice(a, a + len)))) return true;
    i = j;
  }
  return false;
}
