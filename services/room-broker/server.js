/**
 * SF4 Netplay Launcher room broker (budget MVP).
 * Run: node server.js
 * Env: PORT=8787 RELAY_HOST=your.vps RELAY_PORT_BASE=23456 MAX_ROOMS=20 ROOM_IDLE_MS=900000
 *      FORCE_VPS_RELAY=1 RELAY_MANAGER_URL=http://127.0.0.1:8788
 *      BROKER_ENABLE_ROOM_LIST=0
 */

const http = require("http");
const crypto = require("crypto");

const PORT = parseInt(process.env.PORT || "8787", 10);
const RELAY_HOST = process.env.RELAY_HOST || "127.0.0.1";
const RELAY_PORT_BASE = parseInt(process.env.RELAY_PORT_BASE || "23456", 10);
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || "20", 10);
const ROOM_IDLE_MS = parseInt(process.env.ROOM_IDLE_MS || String(15 * 60 * 1000), 10);
const QUEUE_TICK_MS = parseInt(process.env.QUEUE_TICK_MS || "3000", 10);
const MAX_QUEUE = parseInt(process.env.MAX_QUEUE || "50", 10);
const MAX_BODY_BYTES = parseInt(process.env.MAX_BODY_BYTES || String(64 * 1024), 10);
const ENABLE_ROOM_LIST =
  process.env.BROKER_ENABLE_ROOM_LIST === "1" || process.env.BROKER_ENABLE_ROOM_LIST === "true";
const FORCE_VPS_RELAY =
  process.env.FORCE_VPS_RELAY === "1" || process.env.FORCE_VPS_RELAY === "true";
const RELAY_MANAGER_URL = process.env.RELAY_MANAGER_URL || "http://127.0.0.1:8788";

/** @type {Map<string, { code: string, host: string, port: number, displayName: string, createdAt: number, lastSeenAt: number, sidecarHash?: string, hostSecret: string }>} */
const rooms = new Map();
/** @type {string[]} */
const queue = [];

/** @type {Map<string, number[]>} */
const rateBuckets = new Map();

function makeToken() {
  return crypto.randomBytes(8).toString("hex").toUpperCase();
}

function makeHostSecret() {
  return crypto.randomBytes(32).toString("hex");
}

function allocatePort() {
  const used = new Set([...rooms.values()].map((r) => r.port));
  for (let i = 0; i < MAX_ROOMS; i++) {
    const port = RELAY_PORT_BASE + i;
    if (!used.has(port)) return port;
  }
  return 0;
}

function clientIp(req) {
  const forwarded = req.headers["x-forwarded-for"];
  if (typeof forwarded === "string" && forwarded.trim()) {
    return forwarded.split(",")[0].trim();
  }
  return req.socket.remoteAddress || "unknown";
}

function allowRate(key, limit, windowMs) {
  const now = Date.now();
  const bucket = rateBuckets.get(key) || [];
  const recent = bucket.filter((ts) => now - ts < windowMs);
  if (recent.length >= limit) {
    rateBuckets.set(key, recent);
    return false;
  }
  recent.push(now);
  rateBuckets.set(key, recent);
  return true;
}

function checkRate(req, bucket, limit, windowMs) {
  const ip = clientIp(req);
  const key = `${bucket}:${ip}`;
  if (!allowRate(key, limit, windowMs)) {
    return false;
  }
  return true;
}

function verifyHostSecret(room, hostSecret) {
  if (!room || !room.hostSecret) {
    return false;
  }
  if (typeof hostSecret !== "string" || !hostSecret) {
    return false;
  }
  const a = Buffer.from(room.hostSecret, "utf8");
  const b = Buffer.from(hostSecret, "utf8");
  if (a.length !== b.length) {
    return false;
  }
  return crypto.timingSafeEqual(a, b);
}

async function relayManagerRequest(method, path, body) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL) {
    return { ok: true, skipped: true };
  }
  const url = `${RELAY_MANAGER_URL}${path}`;
  const opts = {
    method,
    headers: { "Content-Type": "application/json" },
  };
  if (body !== undefined) {
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(url, opts);
  let data = {};
  try {
    data = await res.json();
  } catch {
    data = {};
  }
  return { status: res.status, ...data };
}

async function startRelaySession(port, sidecarHash) {
  const result = await relayManagerRequest("POST", "/v1/sessions", { port, sidecarHash });
  if (result.skipped) {
    return { ok: true };
  }
  if (result.status === 201 && result.ok) {
    return { ok: true };
  }
  return {
    ok: false,
    message: result.message || `Relay manager returned HTTP ${result.status}`,
  };
}

function stopRelaySession(port) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL || !port) {
    return;
  }
  void relayManagerRequest("DELETE", `/v1/sessions/${port}`);
}

async function getRelaySessionHealth(port) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL || !port) {
    return { ok: true, listening: true, running: true, skipped: true };
  }
  const result = await relayManagerRequest("GET", `/v1/sessions/${port}/health`);
  if (result.skipped) {
    return { ok: true, listening: true, running: true, skipped: true };
  }
  return {
    ok: result.ok === true,
    listening: result.listening === true,
    running: result.running === true,
  };
}

async function ensureRelaySession(room) {
  const health = await getRelaySessionHealth(room.port);
  if (health.ok && health.listening) {
    return { ok: true, relay: health };
  }
  if (!room.sidecarHash) {
    return {
      ok: false,
      relay: health,
      message: "Relay session is not running. Create a new room.",
    };
  }
  const started = await startRelaySession(room.port, room.sidecarHash);
  if (!started.ok) {
    return {
      ok: false,
      relay: health,
      message: started.message || "Could not restart relay session.",
    };
  }
  const after = await getRelaySessionHealth(room.port);
  if (after.ok && after.listening) {
    return { ok: true, relay: after, restarted: true };
  }
  return {
    ok: false,
    relay: after,
    message: "Relay session failed to start. Create a new room.",
  };
}

function pruneRooms() {
  const now = Date.now();
  for (const [code, room] of rooms) {
    if (now - room.lastSeenAt > ROOM_IDLE_MS) {
      stopRelaySession(room.port);
      rooms.delete(code);
    }
  }
}

function json(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    "Content-Type": "application/json",
  });
  res.end(payload);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let data = "";
    let total = 0;
    req.on("data", (chunk) => {
      total += chunk.length;
      if (total > MAX_BODY_BYTES) {
        reject(new Error("body_too_large"));
        req.destroy();
        return;
      }
      data += chunk;
    });
    req.on("end", () => {
      try {
        resolve(data ? JSON.parse(data) : {});
      } catch {
        resolve({});
      }
    });
    req.on("error", () => resolve({}));
  });
}

function normalizeCode(raw) {
  if (!raw) return "";
  const s = String(raw).trim().toUpperCase();
  if (s.startsWith("SF4-")) return s;
  return `SF4-${s}`;
}

function isValidSidecarHash(hash) {
  return typeof hash === "string" && /^[a-f0-9]{64}$/i.test(hash);
}

async function createRoomRecord(displayName, relayHost, sidecarHash) {
  if (rooms.size >= MAX_ROOMS) {
    return { error: "full", message: "Relay is full. Try again in a few minutes or use LAN/direct play." };
  }
  const port = allocatePort();
  if (!port) {
    return { error: "full", message: "No relay ports available." };
  }

  if (FORCE_VPS_RELAY) {
    if (!sidecarHash) {
      return {
        error: "invalid_hash",
        message: "sidecarHash required for VPS relay.",
      };
    }
    if (!isValidSidecarHash(sidecarHash)) {
      return {
        error: "invalid_hash",
        message: "sidecarHash must be a 64-character SHA-256 hex string.",
      };
    }
    const started = await startRelaySession(port, sidecarHash);
    if (!started.ok) {
      return {
        error: "relay_start_failed",
        message: started.message || "Could not start VPS session relay.",
      };
    }
  }

  let token = makeToken();
  while (rooms.has(`SF4-${token}`)) token = makeToken();
  const code = `SF4-${token}`;
  const hostSecret = makeHostSecret();
  const now = Date.now();
  const roomHost = FORCE_VPS_RELAY ? RELAY_HOST : relayHost || RELAY_HOST;
  rooms.set(code, {
    code,
    host: roomHost,
    port,
    displayName: displayName || "Host",
    createdAt: now,
    lastSeenAt: now,
    sidecarHash: sidecarHash || undefined,
    hostSecret,
  });
  return { code, host: roomHost, port, token, hostSecret };
}

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204);
    res.end();
    return;
  }

  pruneRooms();
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`);

  if (req.method === "GET" && url.pathname === "/v1/health") {
    json(res, 200, {
      ok: true,
      rooms: rooms.size,
      maxRooms: MAX_ROOMS,
      relayHost: RELAY_HOST,
      forceVpsRelay: FORCE_VPS_RELAY,
      queueSize: queue.length,
      relayPortBase: RELAY_PORT_BASE,
      roomIdleMs: ROOM_IDLE_MS,
    });
    return;
  }

  if (req.method === "GET" && url.pathname === "/v1/rooms") {
    if (!ENABLE_ROOM_LIST) {
      json(res, 404, { error: "not_found", message: "Room listing is disabled." });
      return;
    }
    const now = Date.now();
    const list = [...rooms.values()]
      .filter((r) => now - r.lastSeenAt < ROOM_IDLE_MS)
      .map((r) => ({
        code: r.code,
        displayName: r.displayName,
        port: r.port,
        host: r.host,
        createdAt: r.createdAt,
        lastSeenAt: r.lastSeenAt,
      }));
    json(res, 200, { rooms: list });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/rooms") {
    if (!checkRate(req, "create", 10, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many room create requests." });
      return;
    }
    let body = {};
    try {
      body = await readBody(req);
    } catch (err) {
      if (err && err.message === "body_too_large") {
        json(res, 413, { error: "payload_too_large", message: "Request body too large." });
        return;
      }
      body = {};
    }
    const sidecarHash = String(body.sidecarHash || "").trim();
    const relayHost =
      !FORCE_VPS_RELAY && body.relayHost ? String(body.relayHost).trim() : "";
    const created = await createRoomRecord(body.displayName, relayHost, sidecarHash);
    if (created.error) {
      const status =
        created.error === "full" || created.error === "relay_start_failed" ? 503 : 400;
      json(res, status, created);
      return;
    }
    json(res, 201, created);
    return;
  }

  const roomMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)$/i);
  const roomHealthMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/health$/i);

  if (req.method === "GET" && roomHealthMatch) {
    const code = normalizeCode(roomHealthMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
      });
      return;
    }
    const relay = await ensureRelaySession(room);
    json(res, 200, {
      ok: relay.ok,
      code: room.code,
      port: room.port,
      relayOk: relay.ok,
      relayListening: relay.relay?.listening === true,
      relayRunning: relay.relay?.running === true,
      restarted: relay.restarted === true,
      message: relay.message || "",
    });
    return;
  }

  if (req.method === "GET" && roomMatch) {
    if (!checkRate(req, "resolve", 30, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many room lookup requests." });
      return;
    }
    const code = normalizeCode(roomMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
      });
      return;
    }
    room.lastSeenAt = Date.now();
    const relay = FORCE_VPS_RELAY ? await getRelaySessionHealth(room.port) : { ok: true, listening: true };
    json(res, 200, {
      code: room.code,
      host: FORCE_VPS_RELAY ? RELAY_HOST : room.host,
      port: room.port,
      relayOk: relay.ok === true && relay.listening === true,
    });
    return;
  }

  if (req.method === "DELETE" && roomMatch) {
    if (!checkRate(req, "mutate", 60, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many room mutation requests." });
      return;
    }
    let body = {};
    try {
      body = await readBody(req);
    } catch (err) {
      if (err && err.message === "body_too_large") {
        json(res, 413, { error: "payload_too_large", message: "Request body too large." });
        return;
      }
      body = {};
    }
    const code = normalizeCode(roomMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
      });
      return;
    }
    if (!verifyHostSecret(room, body.hostSecret)) {
      json(res, 401, {
        error: "unauthorized",
        message: "Host secret required.",
      });
      return;
    }
    stopRelaySession(room.port);
    rooms.delete(code);
    json(res, 200, { ok: true, code, stopped: true });
    return;
  }

  const heartbeatMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/heartbeat$/i);
  if (req.method === "POST" && heartbeatMatch) {
    if (!checkRate(req, "mutate", 60, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many room mutation requests." });
      return;
    }
    let body = {};
    try {
      body = await readBody(req);
    } catch (err) {
      if (err && err.message === "body_too_large") {
        json(res, 413, { error: "payload_too_large", message: "Request body too large." });
        return;
      }
      body = {};
    }
    const code = normalizeCode(heartbeatMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
      });
      return;
    }
    if (!verifyHostSecret(room, body.hostSecret)) {
      json(res, 401, {
        error: "unauthorized",
        message: "Host secret required.",
      });
      return;
    }
    room.lastSeenAt = Date.now();
    const relay = await ensureRelaySession(room);
    json(res, 200, {
      ok: relay.ok,
      heartbeatOk: relay.ok,
      code: room.code,
      relayListening: relay.relay?.listening === true,
      restarted: relay.restarted === true,
      message: relay.message || "",
    });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/queue/join") {
    let body = {};
    try {
      body = await readBody(req);
    } catch (err) {
      if (err && err.message === "body_too_large") {
        json(res, 413, { error: "payload_too_large", message: "Request body too large." });
        return;
      }
      body = {};
    }
    if (queue.length >= MAX_QUEUE) {
      json(res, 503, {
        error: "queue_full",
        message: "Matchmaking queue is full. Try again shortly.",
      });
      return;
    }
    const name = body.displayName || "Player";
    queue.push(name);
    if (queue.length >= 2) {
      const a = queue.shift();
      const b = queue.shift();
      const sidecarHash = String(body.sidecarHash || "").trim();
      const created = await createRoomRecord(`${a} vs ${b}`, RELAY_HOST, sidecarHash);
      if (created.error) {
        json(res, 503, {
          error: created.error,
          message: created.message || "Matchmaking queue paused — relay full.",
        });
        return;
      }
      json(res, 200, {
        status: "matched",
        code: created.code,
        host: created.host,
        port: created.port,
        hostSecret: created.hostSecret,
      });
      return;
    }
    json(res, 202, { status: "waiting", message: "Looking for an opponent…" });
    return;
  }

  json(res, 404, { error: "not_found", message: "Unknown API path." });
});

setInterval(pruneRooms, Math.min(ROOM_IDLE_MS, 60000));

setInterval(() => {
  const now = Date.now();
  for (const [key, bucket] of rateBuckets.entries()) {
    const recent = bucket.filter((ts) => now - ts < 300_000);
    if (recent.length) {
      rateBuckets.set(key, recent);
    } else {
      rateBuckets.delete(key);
    }
  }
}, 60_000);

server.listen(PORT, () => {
  console.log(
    `SF4 Netplay Launcher room broker on :${PORT} relay=${RELAY_HOST}:${RELAY_PORT_BASE}+ maxRooms=${MAX_ROOMS} forceVpsRelay=${FORCE_VPS_RELAY}`
  );
});
