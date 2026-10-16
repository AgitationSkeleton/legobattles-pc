// The server's log for its operator: one Discord message for each thing worth
// knowing -- a lobby hosted (with its settings), a player joining or leaving,
// the match starting and ending, the lobby disbanded or gone quiet. The
// webhook is the DISCORD_WEBHOOK secret (`wrangler secret put DISCORD_WEBHOOK`),
// never in the repository: whoever has it can post to the channel. Without it
// nothing is sent. A message that fails is let go; the game never waits on it.

// Names as they are, not as Discord's formatting would take them.
export function plain(s) {
  return String(s == null ? '' : s)
    .replace(/[\r\n]+/g, ' ')
    .replace(/[\\*_~`|>#[\]()<:@-]/g, '\\$&')
    .slice(0, 100);
}

// "12 min", "1 h 5 min", "40 s".
export function lasted(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return s + ' s';
  const m = Math.round(s / 60);
  return m < 60 ? m + ' min' : Math.floor(m / 60) + ' h ' + (m % 60) + ' min';
}

// The host's settings, as the port's own panel labels them (the client sends
// label: value pairs, so the server need not know what the numbers mean).
export function settingsText(options) {
  if (!options || typeof options !== 'object') return '';
  const lines = [];
  for (const [k, v] of Object.entries(options).slice(0, 24)) {
    if (v === '' || v == null) continue;
    lines.push('• ' + plain(k) + ': ' + plain(typeof v === 'object' ? JSON.stringify(v) : v));
  }
  return lines.join('\n');
}

// One message. ctx: whatever can keep the Durable Object running until it is
// sent -- its waitUntil.
export function discord(env, ctx, text) {
  if (!env || !env.DISCORD_WEBHOOK) return;
  const send = fetch(env.DISCORD_WEBHOOK, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    // (no one is pinged, whatever a name says)
    body: JSON.stringify({
      username: 'LEGO Battles server',
      content: String(text).slice(0, 1900),
      allowed_mentions: { parse: [] },
    }),
  }).catch(() => {});
  if (ctx && ctx.waitUntil) ctx.waitUntil(send);
}
