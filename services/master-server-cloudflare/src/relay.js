// THE RELAY (docs/424): online play when the direct UDP route cannot connect
// -- a VPN, carrier-grade or symmetric NAT, a router that drops everything
// unasked. Both players dial OUT to this server, over the same plain HTTP the
// lobby list uses, and this room joins them.
//
// It follows lsw2ds-pc's relay (its services/master-server/src/relay.js), with
// one difference: what crosses it is the port's bridge DATAGRAMS, not a byte
// stream, so a message carries whole datagrams and the host can address every
// joiner at once.
//
// ONE ROOM PER LOBBY, a Durable Object named by the lobby's room code.
//
//   host    GET /relay/<code>?role=host   (X-Token: the lobby's token)
//   joiner  GET /relay/<code>?role=join
//
// THE WIRE, binary WebSocket messages. A BATCH is one or more datagrams, each
// [u16 length, little-endian][bytes].
//   host -> room   [0][peer][batch]  for that joiner; peer 255: every joiner
//                  [2][peer]         close that joiner
//   room -> host   [0][peer][batch]  from that joiner
//                  [1][peer]         a joiner connected
//                  [2][peer]         a joiner went away
//   joiner <-> room                  the batch alone
// Text: the room tells a joiner "ready" once it is paired with a live host;
// either side may send "ping" (the room answers "pong").
//
// The room never parses a datagram. Its sockets are accepted with
// acceptWebSocket and tagged, so a room whose host waits in an empty lobby can
// hibernate between pings.

import { DurableObject } from 'cloudflare:workers';

const MAX_PEERS = 8;
const ALL = 255;

export class RelayRoom extends DurableObject {
  host() {
    return this.ctx.getWebSockets('host')[0] || null;
  }

  peer(n) {
    return this.ctx.getWebSockets('p' + n)[0] || null;
  }

  peers() {
    const out = [];
    for (let i = 1; i <= MAX_PEERS; ++i) {
      const ws = this.peer(i);
      if (ws) out.push(ws);
    }
    return out;
  }

  peerOf(ws) {
    for (const tag of this.ctx.getTags(ws)) {
      if (tag[0] === 'p') return Number(tag.slice(1));
    }
    return -1;
  }

  // A refusal is an HTTP answer, before any upgrade: a socket closed in the
  // same breath as it was accepted can reach the client as nothing at all.
  refuse(status, error) {
    return new Response(JSON.stringify({ ok: false, error }), {
      status, headers: { 'Content-Type': 'application/json' },
    });
  }

  async fetch(request) {
    const role = new URL(request.url).searchParams.get('role');

    if (role === 'host') {
      const [client, server] = Object.values(new WebSocketPair());
      // A reconnecting host replaces the old socket; the joiners it carried
      // reconnect too.
      for (const old of this.ctx.getWebSockets()) {
        try { old.close(4000, 'host reconnected'); } catch { /* gone */ }
      }
      this.ctx.acceptWebSocket(server, ['host']);
      return new Response(null, { status: 101, webSocket: client });
    }

    const host = this.host();
    if (!host) return this.refuse(404, 'the host is not on the relay');
    let n = -1;
    for (let i = 1; i <= MAX_PEERS; ++i) {
      if (!this.peer(i)) { n = i; break; }
    }
    if (n < 0) return this.refuse(409, 'the relay room is full');
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ['p' + n]);
    host.send(new Uint8Array([1, n]));
    server.send('ready');
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    if (typeof message === 'string') {
      if (message === 'ping') ws.send('pong');
      return;
    }
    const bytes = new Uint8Array(message);
    const n = this.peerOf(ws);
    if (n > 0) {
      // From a joiner: to the host, tagged.
      const host = this.host();
      if (!host) { ws.close(4004, 'the host left'); return; }
      const out = new Uint8Array(bytes.length + 2);
      out[0] = 0;
      out[1] = n;
      out.set(bytes, 2);
      host.send(out);
      return;
    }
    if (!this.ctx.getTags(ws).includes('host') || bytes.length < 2) return;
    if (bytes[0] === 0) {
      const batch = bytes.subarray(2);
      if (bytes[1] === ALL) {
        for (const to of this.peers()) {
          try { to.send(batch); } catch { /* gone */ }
        }
        return;
      }
      const to = this.peer(bytes[1]);
      if (to) to.send(batch);
      else ws.send(new Uint8Array([2, bytes[1]]));
    } else if (bytes[0] === 2) {
      const to = this.peer(bytes[1]);
      if (to) to.close(1000, 'closed by the host');
    }
  }

  async webSocketClose(ws, code) {
    try { ws.close(code === 1005 ? 1000 : code, 'bye'); } catch { /* answered */ }
    this.gone(ws);
  }

  async webSocketError(ws) {
    this.gone(ws);
  }

  gone(ws) {
    const n = this.peerOf(ws);
    if (n > 0) {
      const host = this.host();
      if (host) {
        try { host.send(new Uint8Array([2, n])); } catch { /* gone */ }
      }
      return;
    }
    if (this.ctx.getTags(ws).includes('host')) {
      for (const other of this.ctx.getWebSockets()) {
        if (other !== ws) {
          try { other.close(4004, 'the host left'); } catch { /* gone */ }
        }
      }
    }
  }
}
