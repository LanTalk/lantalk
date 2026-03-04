export interface Env {
  SIGNAL_HUB: DurableObjectNamespace;
}

type PresenceRecord = {
  userId: string;
  name: string;
  avatarPayload: string;
  ip: string;
  listenPort: number;
  e2eePublic: string;
  localIps: string[];
  p2pPeers: string[];
  lastSeenMs: number;
};

type RelayMessage = {
  id: string;
  fromUserId: string;
  fromName: string;
  fromAvatar: string;
  toUserId: string;
  text: string;
  timestampMs: number;
  mode: "ws";
};

const PEER_TTL_MS = 15_000;
const MAX_PULL_MESSAGES = 200;

function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
      "access-control-allow-methods": "GET,POST,OPTIONS",
      "access-control-allow-headers": "content-type,authorization",
    },
  });
}

function buildMessageKey(toUserId: string, ts: number, id: string): string {
  return `msg:${toUserId}:${String(ts).padStart(13, "0")}:${id}`;
}

function parseTsFromMessageKey(key: string): number {
  const parts = key.split(":");
  if (parts.length < 4) return 0;
  const parsed = Number(parts[2]);
  return Number.isFinite(parsed) ? parsed : 0;
}

function decideMode(selfPeer: PresenceRecord | undefined, otherPeer: PresenceRecord): "p2p" | "ws" {
  if (!selfPeer) return "ws";
  const verified = new Set((selfPeer.p2pPeers || []).map((v) => String(v || "").trim()).filter((v) => v.length > 0));
  if (verified.has(otherPeer.userId)) return "p2p";
  return "ws";
}

export class SignalHub {
  private state: DurableObjectState;
  private env: Env;
  private sockets = new Map<string, Set<WebSocket>>();

  constructor(state: DurableObjectState, env: Env) {
    this.state = state;
    this.env = env;
  }

  async fetch(request: Request): Promise<Response> {
    if (request.method === "OPTIONS") {
      return json({ ok: true });
    }

    const url = new URL(request.url);
    const path = url.pathname;

    if (path === "/health") {
      return json({ ok: true, service: "lantalk-signal" });
    }

    if (path === "/v1/ws") {
      return this.handleWebSocket(request, url);
    }

    if (path === "/v1/presence" && request.method === "POST") {
      return this.handlePresence(request);
    }
    if (path === "/v1/peers" && request.method === "GET") {
      return this.handlePeers(url);
    }
    if (path === "/v1/messages/send" && request.method === "POST") {
      return this.handleSendMessage(request);
    }
    if (path === "/v1/messages/pull" && request.method === "GET") {
      return this.handlePullMessages(url);
    }

    return json({ ok: false, error: "not_found" }, 404);
  }

  private async handleWebSocket(request: Request, url: URL): Promise<Response> {
    const userId = (url.searchParams.get("userId") || "").trim();
    if (!userId) {
      return json({ ok: false, error: "userId_required" }, 400);
    }
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return json({ ok: false, error: "websocket_required" }, 400);
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    server.accept();

    if (!this.sockets.has(userId)) this.sockets.set(userId, new Set<WebSocket>());
    this.sockets.get(userId)!.add(server);

    server.addEventListener("close", () => {
      this.sockets.get(userId)?.delete(server);
      if ((this.sockets.get(userId)?.size || 0) === 0) this.sockets.delete(userId);
    });

    server.addEventListener("message", async (evt) => {
      try {
        const payload = JSON.parse(String(evt.data || "{}"));
        if (payload?.type === "ping") {
          server.send(JSON.stringify({ type: "pong", now: Date.now() }));
        }
      } catch {
        // ignore malformed message
      }
    });

    return new Response(null, { status: 101, webSocket: client });
  }

  private async handlePresence(request: Request): Promise<Response> {
    const body = (await request.json().catch(() => ({}))) as Record<string, unknown>;
    const userId = String(body.userId || "").trim();
    if (!userId) return json({ ok: false, error: "userId_required" }, 400);

    const ip = (request.headers.get("CF-Connecting-IP") || "").trim();
    const listenPort = Number(body.listenPort || 0);
    const e2eePublic = String(body.e2eePublic || "").trim();
    const localIps = Array.isArray(body.localIps)
      ? body.localIps.map((v) => String(v)).filter((v) => v.length > 0).slice(0, 16)
      : [];
    const p2pPeers = Array.isArray(body.p2pPeers)
      ? body.p2pPeers
          .map((v) => String(v || "").trim())
          .filter((v) => v.length > 0 && v.length <= 128)
          .slice(0, 256)
      : [];

    const peer: PresenceRecord = {
      userId,
      name: String(body.name || userId).trim().slice(0, 128) || userId,
      avatarPayload: String(body.avatarPayload || "").slice(0, 8192),
      ip,
      listenPort: Number.isFinite(listenPort) ? listenPort : 0,
      e2eePublic,
      localIps,
      p2pPeers,
      lastSeenMs: Date.now(),
    };

    await this.state.storage.put(`peer:${userId}`, peer);
    await this.prunePeers();
    return json({ ok: true, now: peer.lastSeenMs });
  }

  private async handlePeers(url: URL): Promise<Response> {
    const selfId = (url.searchParams.get("userId") || "").trim();
    await this.prunePeers();

    const list = await this.state.storage.list<PresenceRecord>({ prefix: "peer:" });
    const peers = Array.from(list.values());
    const self = peers.find((p) => p.userId === selfId);

    const out = peers
      .filter((p) => p.userId !== selfId)
      .map((peer) => ({
        userId: peer.userId,
        name: peer.name,
        avatarPayload: peer.avatarPayload,
        ip: peer.ip,
        port: peer.listenPort,
        e2eePublic: peer.e2eePublic,
        mode: decideMode(self, peer),
        lastSeenMs: peer.lastSeenMs,
      }));

    return json({ ok: true, peers: out, now: Date.now() });
  }

  private async handleSendMessage(request: Request): Promise<Response> {
    const body = (await request.json().catch(() => ({}))) as Record<string, unknown>;
    const fromUserId = String(body.fromUserId || "").trim();
    const toUserId = String(body.toUserId || "").trim();
    const text = String(body.text || "").trim();
    if (!fromUserId || !toUserId || !text) {
      return json({ ok: false, error: "invalid_payload" }, 400);
    }

    const timestampMs = Number(body.timestampMs || Date.now());
    const msg: RelayMessage = {
      id: crypto.randomUUID(),
      fromUserId,
      fromName: String(body.fromName || fromUserId).trim().slice(0, 128),
      fromAvatar: String(body.fromAvatar || "").slice(0, 8192),
      toUserId,
      text: text.slice(0, 16 * 1024),
      timestampMs: Number.isFinite(timestampMs) ? timestampMs : Date.now(),
      mode: "ws",
    };

    await this.state.storage.put(buildMessageKey(toUserId, msg.timestampMs, msg.id), msg);

    const targets = this.sockets.get(toUserId);
    if (targets && targets.size > 0) {
      const wire = JSON.stringify({ type: "message", message: msg });
      for (const socket of targets) {
        try {
          socket.send(wire);
        } catch {
          // ignore send failure, polling still works
        }
      }
    }

    return json({ ok: true, id: msg.id, mode: "ws" });
  }

  private async handlePullMessages(url: URL): Promise<Response> {
    const userId = (url.searchParams.get("userId") || "").trim();
    const after = Number(url.searchParams.get("after") || "0");
    if (!userId) return json({ ok: false, error: "userId_required" }, 400);

    const list = await this.state.storage.list<RelayMessage>({ prefix: `msg:${userId}:` });
    const out: RelayMessage[] = [];
    const removeKeys: string[] = [];

    for (const [key, value] of list) {
      const ts = parseTsFromMessageKey(key);
      if (ts <= after) continue;
      out.push(value);
      removeKeys.push(key);
      if (out.length >= MAX_PULL_MESSAGES) break;
    }

    out.sort((a, b) => a.timestampMs - b.timestampMs);
    if (removeKeys.length > 0) {
      await this.state.storage.delete(removeKeys);
    }

    return json({ ok: true, messages: out, now: Date.now() });
  }

  private async prunePeers(): Promise<void> {
    const now = Date.now();
    const list = await this.state.storage.list<PresenceRecord>({ prefix: "peer:" });
    const removeKeys: string[] = [];
    for (const [key, value] of list) {
      if (!value || now - value.lastSeenMs > PEER_TTL_MS) {
        removeKeys.push(key);
      }
    }
    if (removeKeys.length > 0) {
      await this.state.storage.delete(removeKeys);
    }
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const id = env.SIGNAL_HUB.idFromName("global");
    const stub = env.SIGNAL_HUB.get(id);
    return stub.fetch(request);
  },
};
