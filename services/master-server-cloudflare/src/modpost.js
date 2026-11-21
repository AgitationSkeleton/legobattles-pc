// docs/503: MODERATION MESSAGES, TWICE. Every map, unit and account message
// goes to its own channel (LEVELS_CHANNEL, UNITS_CHANNEL, ACCOUNTS_CHANNEL)
// and to the mirror channel (MIRROR_CHANNEL, the original submissions channel
// in the other server), and every later change edits both copies. A row keeps
// both ids: `message` (its own channel) and `mirror`. A message posted before
// the mirror existed has no `mirror` and may live in the mirror channel (where
// everything went at first), so an edit that misses in the own channel is
// tried there.
const DISCORD = 'https://discord.com/api/v10';
const api = (env) => env.DISCORD_API_BASE || DISCORD;

export function mirrorChannel(env, channel) {
  const m = String(env.MIRROR_CHANNEL || '');
  return m && m !== String(channel || '') ? m : '';
}

// make() returns a fresh request body each time: { body, json } (a FormData
// cannot be sent twice). Returns { id, mirror } ('' for a copy that failed).
export async function postCopies(env, channel, make) {
  const res = { id: '', mirror: '' };
  if (!env.LEVELS_BOT_TOKEN) return res;
  const targets = [];
  if (channel) targets.push(['id', String(channel)]);
  const m = mirrorChannel(env, channel);
  if (m) targets.push(['mirror', m]);
  for (const [slot, ch] of targets) {
    const { body, json } = make();
    const headers = { Authorization: 'Bot ' + env.LEVELS_BOT_TOKEN };
    if (json) headers['Content-Type'] = 'application/json';
    const r = await fetch(`${api(env)}/channels/${ch}/messages`, { method: 'POST', headers, body }).catch(() => null);
    const d = r && r.ok ? await r.json().catch(() => null) : null;
    res[slot] = d && d.id ? String(d.id) : '';
  }
  return res;
}

async function patch(env, channel, id, msg) {
  const r = await fetch(`${api(env)}/channels/${channel}/messages/${id}`, {
    method: 'PATCH',
    headers: { Authorization: 'Bot ' + env.LEVELS_BOT_TOKEN, 'Content-Type': 'application/json' },
    body: JSON.stringify(msg),
  }).catch(() => null);
  return !!(r && r.ok);
}

export async function editCopies(env, channel, id, mirror, msg) {
  if (!env.LEVELS_BOT_TOKEN) return;
  const m = mirrorChannel(env, channel);
  const jobs = [];
  if (id && channel)
    jobs.push(patch(env, channel, id, msg).then((ok) => (ok || mirror || !m ? ok : patch(env, m, id, msg))));
  if (mirror && m) jobs.push(patch(env, m, mirror, msg));
  await Promise.allSettled(jobs);
}
