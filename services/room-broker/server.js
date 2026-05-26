/**
 * SF4 Enhanced room broker (budget MVP).
 * Run: node server.js
 * Env: PORT=8787 RELAY_HOST=your.vps RELAY_PORT_BASE=23456 MAX_ROOMS=20 ROOM_IDLE_MS=900000
 */

const http = require("http");
const crypto = require("crypto");

const PORT = parseInt(process.env.PORT || "8787", 10);
const RELAY_HOST = process.env.RELAY_HOST || "127.0.0.1";
const RELAY_PORT_BASE = parseInt(process.env.RELAY_PORT_BASE || "23456", 10);
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || "20", 10);
const ROOM_IDLE_MS = parseInt(process.env.ROOM_IDLE_MS || String(15 * 60 * 1000), 10);
const QUEUE_TICK_MS = parseInt(process.env.QUEUE_TICK_MS || "3000", 10);

/** @type {Map<string, { code: string, host: string, port: number, displayName: string, createdAt: number, lastSeenAt: number }>} */
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

function pruneRooms() {
  const now = Date.now();
  for (const [code, room] of rooms) {
    if (now - room.lastSeenAt > ROOM_IDLE_MS) {
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

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
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
        players: 1,
      }));
    json(res, 200, { rooms: list });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/rooms") {
    if (rooms.size >= MAX_ROOMS) {
      json(res, 503, {
        error: "full",
        message: "Relay is full. Try again in a few minutes or use LAN/direct play.",
      });
      return;
    }
    const port = allocatePort();
    if (!port) {
      json(res, 503, {
        error: "full",
        message: "No relay ports available.",
      });
      return;
    }
    const body = await readBody(req);
    let token = makeToken();
    while (rooms.has(`SF4-${token}`)) token = makeToken();
    const code = `SF4-${token}`;
    const now = Date.now();
    const roomHost = (body.relayHost && String(body.relayHost).trim()) || RELAY_HOST;
    rooms.set(code, {
      code,
      host: roomHost,
      port,
      displayName: body.displayName || "Host",
      createdAt: now,
      lastSeenAt: now,
    });
    json(res, 201, { code, host: roomHost, port, token });
    return;
  }

  const roomMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)$/i);
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
    json(res, 200, { code: room.code, host: room.host, port: room.port });
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
    json(res, 200, { ok: true, code: room.code });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/queue/join") {
    const body = await readBody(req);
    const name = body.displayName || "Player";
    queue.push(name);
    if (queue.length >= 2) {
      const a = queue.shift();
      const b = queue.shift();
      const bodyCreate = { displayName: `${a} vs ${b}` };
      // Reuse room create logic inline
      if (rooms.size >= MAX_ROOMS) {
        json(res, 503, {
          error: "full",
          message: "Matchmaking queue paused — relay full.",
        });
        return;
      }
      const port = allocatePort();
      if (!port) {
        json(res, 503, { error: "full", message: "No ports for quick match." });
        return;
      }
      let token = makeToken();
      while (rooms.has(`SF4-${token}`)) token = makeToken();
      const code = `SF4-${token}`;
      const now = Date.now();
      rooms.set(code, {
        code,
        host: RELAY_HOST,
        port,
        displayName: bodyCreate.displayName,
        createdAt: now,
        lastSeenAt: now,
      });
      json(res, 200, { status: "matched", code, host: RELAY_HOST, port });
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
    `SF4 Enhanced room broker on :${PORT} relay=${RELAY_HOST}:${RELAY_PORT_BASE}+ maxRooms=${MAX_ROOMS}`
  );
});
