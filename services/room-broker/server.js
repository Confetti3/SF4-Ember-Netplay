/**
 * SF4 Netplay Launcher room broker (budget MVP).
 * Run: node server.js
 * Env: PORT=8787 RELAY_HOST=your.vps RELAY_PORT_BASE=23456 MAX_ROOMS=20 ROOM_IDLE_MS=900000
 *      FORCE_VPS_RELAY=1 RELAY_MANAGER_URL=http://127.0.0.1:8788
 */

const http = require("http");
const crypto = require("crypto");

const PORT = parseInt(process.env.PORT || "8787", 10);
const RELAY_HOST = process.env.RELAY_HOST || "127.0.0.1";
const RELAY_PORT_BASE = parseInt(process.env.RELAY_PORT_BASE || "23456", 10);
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || "20", 10);
const ROOM_IDLE_MS = parseInt(process.env.ROOM_IDLE_MS || String(15 * 60 * 1000), 10);
const QUEUE_TICK_MS = parseInt(process.env.QUEUE_TICK_MS || "3000", 10);
const FORCE_VPS_RELAY =
  process.env.FORCE_VPS_RELAY === "1" || process.env.FORCE_VPS_RELAY === "true";
const RELAY_MANAGER_URL = process.env.RELAY_MANAGER_URL || "http://127.0.0.1:8788";

/** @type {Map<string, { code: string, host: string, port: number, displayName: string, createdAt: number, lastSeenAt: number, sidecarHash?: string }>} */
const rooms = new Map();
/** @type {string[]} */
const queue = [];

function makeToken() {
  return crypto.randomBytes(3).toString("hex").toUpperCase().slice(0, 4);
}

function allocatePort() {
  const used = new Set([...rooms.values()].map((r) => r.port));
  for (let i = 0; i < MAX_ROOMS; i++) {
    const port = RELAY_PORT_BASE + i;
    if (!used.has(port)) return port;
  }
  return 0;
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
    "Access-Control-Allow-Origin": "*",
  });
  res.end(payload);
}

function readBody(req) {
  return new Promise((resolve) => {
    let data = "";
    req.on("data", (chunk) => {
      data += chunk;
    });
    req.on("end", () => {
      try {
        resolve(data ? JSON.parse(data) : {});
      } catch {
        resolve({});
      }
    });
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
  });
  return { code, host: roomHost, port, token };
}

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET,POST,DELETE,OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type",
    });
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
    });
    return;
  }

  if (req.method === "GET" && url.pathname === "/v1/rooms") {
    const list = [...rooms.values()]
      .filter((r) => Date.now() - r.lastSeenAt < ROOM_IDLE_MS)
      .map((r) => ({
        code: r.code,
        displayName: r.displayName,
        port: r.port,
      }));
    json(res, 200, { rooms: list });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/rooms") {
    const body = await readBody(req);
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
    const code = normalizeCode(roomMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
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
    const code = normalizeCode(heartbeatMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, {
        error: "not_found",
        message: "Room not found or expired.",
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
    const body = await readBody(req);
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
      });
      return;
    }
    json(res, 202, { status: "waiting", message: "Looking for an opponent…" });
    return;
  }

  json(res, 404, { error: "not_found", message: "Unknown API path." });
});

setInterval(pruneRooms, Math.min(ROOM_IDLE_MS, 60000));

server.listen(PORT, () => {
  console.log(
    `SF4 Netplay Launcher room broker on :${PORT} relay=${RELAY_HOST}:${RELAY_PORT_BASE}+ maxRooms=${MAX_ROOMS} forceVpsRelay=${FORCE_VPS_RELAY}`
  );
});
