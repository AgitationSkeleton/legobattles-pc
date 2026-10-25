// The server's log for its operator: one Discord message for each thing worth
// knowing -- a lobby hosted (with its settings), a player joining or leaving,
// the match starting and ending, the lobby disbanded or gone quiet. The
// webhook is the DISCORD_WEBHOOK secret (`wrangler secret put DISCORD_WEBHOOK`),
// never in the repository: whoever has it can post to the channel. Without it
// nothing is sent. A message that fails is let go; the game never waits on it.

// Names as they are, not as Discord's formatting would take them.
export function plain(s, max = 100) {
  return String(s == null ? '' : s)
    .replace(/[\r\n]+/g, ' ')
    .replace(/[\\*_~`|>#[\]()<:@-]/g, '\\$&')
    .slice(0, max);
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
    // A value may be a list of map or unit names (docs/429): longer than a name.
    lines.push('• ' + plain(k) + ': ' +
               plain(typeof v === 'object' ? JSON.stringify(v) : v, 400));
  }
  return lines.join('\n');
}

// Every channel the log goes to: the DISCORD_WEBHOOK secret and, posted
// identically, DISCORD_WEBHOOK_2 (a second server's channel; `wrangler secret
// put DISCORD_WEBHOOK_2`). Either may be absent.
export function webhooks(env) {
  return env ? [env.DISCORD_WEBHOOK, env.DISCORD_WEBHOOK_2].filter(Boolean) : [];
}

// docs/442: the sender's name; a server with SERVER_LABEL (the fallback) says
// which it is, so the channel can tell the two apart.
function sender(env) {
  return env && env.SERVER_LABEL ? 'LEGO Battles server (' + env.SERVER_LABEL + ')'
                                 : 'LEGO Battles server';
}

// docs/428: a message with a picture (a PNG's bytes), to every webhook: a
// multipart post, the file shown by an embed as attachment://match.png.
export function discordImage(env, ctx, text, png) {
  for (const url of webhooks(env)) {
    const form = new FormData();
    form.append('payload_json', JSON.stringify({
      username: sender(env),
      content: String(text).slice(0, 1900),
      embeds: [{ image: { url: 'attachment://match.png' } }],
      allowed_mentions: { parse: [] },
    }));
    form.append('files[0]', new Blob([png], { type: 'image/png' }), 'match.png');
    const send = fetch(url, { method: 'POST', body: form }).catch(() => {});
    if (ctx && ctx.waitUntil) ctx.waitUntil(send);
  }
}

// One message. ctx: whatever can keep the Durable Object running until it is
// sent -- its waitUntil.
export function discord(env, ctx, text) {
  const body = JSON.stringify({
    username: sender(env),
    content: String(text).slice(0, 1900),
    // (no one is pinged, whatever a name says)
    allowed_mentions: { parse: [] },
  });
  for (const url of webhooks(env)) {
    const send = fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body,
    }).catch(() => {});
    if (ctx && ctx.waitUntil) ctx.waitUntil(send);
  }
}
