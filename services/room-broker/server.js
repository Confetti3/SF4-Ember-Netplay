/**
 * SF4 Netplay Launcher room broker + match coordinator.
 * Run: node server.js
 * Env: PORT=8787 BROKER_BIND=127.0.0.1 BROKER_TRUST_PROXY=1
 *      RELAY_HOST=your.vps RELAY_PORT_BASE=23456 GGPO_UDP_PORT_BASE=24456
 *      FORCE_VPS_RELAY=1 RELAY_MANAGER_URL=http://127.0.0.1:8788
 *      BROKER_ENABLE_ROOM_LIST=0 BROKER_GGPO_TRANSPORT=legacy|auto
 *      NAT_PROBE_PORT=8790 NAT_PROBE_BIND=0.0.0.0
 */

const http = require("http");
const crypto = require("crypto");
const dgram = require("dgram");

const PORT = parseInt(process.env.PORT || "8787", 10);
const BROKER_BIND = process.env.BROKER_BIND || "127.0.0.1";
const TRUST_PROXY = process.env.BROKER_TRUST_PROXY !== "0";
const RELAY_HOST = process.env.RELAY_HOST || "127.0.0.1";
const RELAY_PORT_BASE = parseInt(process.env.RELAY_PORT_BASE || "23456", 10);
const GGPO_UDP_PORT_BASE = parseInt(process.env.GGPO_UDP_PORT_BASE || "24456", 10);
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || "20", 10);
const ROOM_IDLE_MS = parseInt(process.env.ROOM_IDLE_MS || String(15 * 60 * 1000), 10);
const MAX_QUEUE = parseInt(process.env.MAX_QUEUE || "50", 10);
const MAX_BODY_BYTES = parseInt(process.env.MAX_BODY_BYTES || String(64 * 1024), 10);
const ENABLE_ROOM_LIST =
  process.env.BROKER_ENABLE_ROOM_LIST === "1" || process.env.BROKER_ENABLE_ROOM_LIST === "true";
const FORCE_VPS_RELAY =
  process.env.FORCE_VPS_RELAY === "1" || process.env.FORCE_VPS_RELAY === "true";
const RELAY_MANAGER_URL = process.env.RELAY_MANAGER_URL || "http://127.0.0.1:8788";
const BROKER_GGPO_TRANSPORT = String(process.env.BROKER_GGPO_TRANSPORT || "legacy").toLowerCase();
const NAT_PROBE_PORT = parseInt(process.env.NAT_PROBE_PORT || "8790", 10);
const NAT_PROBE_BIND = process.env.NAT_PROBE_BIND || "0.0.0.0";
const MAX_MATCH_EVENTS = 50;
const NAT_PROBE_TTL_MS = parseInt(process.env.NAT_PROBE_TTL_MS || "120000", 10);

/** @type {Map<string, { observedIp: string, observedPort: number, ts: number }>} */
const natProbeByIp = new Map();

/** @type {Map<string, object>} */
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

function makeMatchId() {
  return crypto.randomBytes(16).toString("hex");
}

function makeRoomToken() {
  return crypto.randomBytes(16).toString("hex");
}

function ggpoPortForSessionPort(sessionPort) {
  const index = sessionPort - RELAY_PORT_BASE;
  if (index < 0 || index >= MAX_ROOMS) {
    return 0;
  }
  return GGPO_UDP_PORT_BASE + index;
}

function allocatePortPair() {
  const usedSession = new Set([...rooms.values()].map((r) => r.port));
  for (let i = 0; i < MAX_ROOMS; i++) {
    const port = RELAY_PORT_BASE + i;
    if (!usedSession.has(port)) {
      return { sessionPort: port, ggpoPort: GGPO_UDP_PORT_BASE + i };
    }
  }
  return { sessionPort: 0, ggpoPort: 0 };
}

function normalizeIp(ip) {
  if (!ip) {
    return "unknown";
  }
  if (ip.startsWith("::ffff:")) {
    return ip.slice(7);
  }
  return ip;
}

function isTrustedProxy(ip) {
  const normalized = normalizeIp(ip);
  return normalized === "127.0.0.1" || ip === "::1";
}

function clientIp(req) {
  const socketIp = normalizeIp(req.socket.remoteAddress);
  if (TRUST_PROXY && isTrustedProxy(socketIp)) {
    const forwarded = req.headers["x-forwarded-for"];
    if (typeof forwarded === "string" && forwarded.trim()) {
      return normalizeIp(forwarded.split(",")[0].trim());
    }
  }
  return socketIp || "unknown";
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
  return allowRate(key, limit, windowMs);
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

function pushMatchEvent(room, type, detail) {
  if (!room.events) {
    room.events = [];
  }
  room.events.push({ ts: Date.now(), type, detail: detail || {} });
  if (room.events.length > MAX_MATCH_EVENTS) {
    room.events.splice(0, room.events.length - MAX_MATCH_EVENTS);
  }
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

async function startGgpoRelaySession(sessionPort, ggpoPort, roomToken) {
  const result = await relayManagerRequest("POST", "/v1/ggpo-sessions", {
    sessionPort,
    ggpoPort,
    roomToken,
  });
  if (result.skipped) {
    return { ok: true };
  }
  if (result.status === 201 && result.ok) {
    return { ok: true };
  }
  return {
    ok: false,
    message: result.message || `GGPO relay manager returned HTTP ${result.status}`,
  };
}

function stopRelaySession(port, ggpoPort) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL || !port) {
    return;
  }
  void relayManagerRequest("DELETE", `/v1/sessions/${port}`);
  if (ggpoPort) {
    void relayManagerRequest("DELETE", `/v1/ggpo-sessions/${ggpoPort}`);
  }
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

async function getGgpoRelayHealth(ggpoPort) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL || !ggpoPort) {
    return { ok: true, listening: true, running: true, skipped: true };
  }
  const result = await relayManagerRequest("GET", `/v1/ggpo-sessions/${ggpoPort}/health`);
  if (result.skipped) {
    return { ok: true, listening: true, running: true, skipped: true };
  }
  return {
    ok: result.ok === true,
    listening: result.listening === true,
    running: result.running === true,
  };
}

async function ensureGgpoRelaySession(room) {
  if (!FORCE_VPS_RELAY || !RELAY_MANAGER_URL || !room.ggpoPort) {
    return { ok: true, skipped: true };
  }
  if (!room.roomToken) {
    return {
      ok: false,
      message: "GGPO relay token missing. Create a new room.",
    };
  }

  const health = await getGgpoRelayHealth(room.ggpoPort);
  if (health.ok && health.listening) {
    return { ok: true, relay: health };
  }

  const started = await startGgpoRelaySession(room.port, room.ggpoPort, room.roomToken);
  if (!started.ok) {
    return {
      ok: false,
      relay: health,
      message: started.message || "Could not restart GGPO UDP relay.",
    };
  }

  const after = await getGgpoRelayHealth(room.ggpoPort);
  if (after.ok && after.listening) {
    return { ok: true, relay: after, restarted: true };
  }
  return {
    ok: false,
    relay: after,
    message: "GGPO UDP relay failed to start. Create a new room.",
  };
}

async function ensureRelaySession(room) {
  const health = await getRelaySessionHealth(room.port);
  if (health.ok && health.listening) {
    const ggpo = await ensureGgpoRelaySession(room);
    return {
      ok: ggpo.ok,
      relay: health,
      ggpoRelay: ggpo.relay,
      ggpoRestarted: ggpo.restarted === true,
      message: ggpo.ok ? "" : ggpo.message || "",
    };
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
  if (!(after.ok && after.listening)) {
    return {
      ok: false,
      relay: after,
      message: "Relay session failed to start. Create a new room.",
    };
  }
  const ggpo = await ensureGgpoRelaySession(room);
  return {
    ok: ggpo.ok,
    relay: after,
    restarted: true,
    ggpoRelay: ggpo.relay,
    ggpoRestarted: ggpo.restarted === true,
    message: ggpo.ok ? "" : ggpo.message || "",
  };
}

function endRoom(code, room, reason) {
  pushMatchEvent(room, "match_ended", { reason: reason || "prune" });
  room.endedAt = Date.now();
  stopRelaySession(room.port, room.ggpoPort);
  rooms.delete(code);
}

function pruneRooms() {
  const now = Date.now();
  for (const [code, room] of rooms) {
    if (now - room.lastSeenAt > ROOM_IDLE_MS) {
      endRoom(code, room, "idle_timeout");
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

function isValidPublicIpv4(ip) {
  if (typeof ip !== "string" || !/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)) {
    return false;
  }
  const parts = ip.split(".").map((p) => parseInt(p, 10));
  if (parts.some((p) => p < 0 || p > 255)) {
    return false;
  }
  const [a, b] = parts;
  if (a === 10 || a === 127 || a === 0) {
    return false;
  }
  if (a === 172 && b >= 16 && b <= 31) {
    return false;
  }
  if (a === 192 && b === 168) {
    return false;
  }
  if (a === 169 && b === 254) {
    return false;
  }
  if (a === 100 && b >= 64 && b <= 127) {
    return false;
  }
  return true;
}

function lookupNatProbe(ip) {
  const entry = natProbeByIp.get(ip);
  if (!entry) {
    return null;
  }
  if (Date.now() - entry.ts > NAT_PROBE_TTL_MS) {
    natProbeByIp.delete(ip);
    return null;
  }
  return entry;
}

const ALLOWED_MATCH_EVENTS = new Set([
  "room_created",
  "endpoint_registered",
  "battle_start",
  "transport_fallback",
  "desync",
  "disconnect",
  "match_ended",
]);

function publicRoomFields(room) {
  return {
    code: room.code,
    host: FORCE_VPS_RELAY ? RELAY_HOST : room.host,
    port: room.port,
    ggpoPort: room.ggpoPort || 0,
    matchId: room.matchId,
    ggpoTransport: room.transportRequested || "legacy",
    transportActive: room.transportActive || null,
    displayName: room.displayName,
    createdAt: room.createdAt,
    lastSeenAt: room.lastSeenAt,
    startedAt: room.startedAt || null,
    endedAt: room.endedAt || null,
  };
}

function predictP2pPossible(room) {
  const host = room.endpoints?.host;
  const guest = room.endpoints?.guest;
  if (!host?.observedPublic || !guest?.observedPublic) {
    return false;
  }
  if (host.observedPublic === guest.observedPublic) {
    return true;
  }
  if (host.natType === "full_cone" && guest.natType === "full_cone") {
    return true;
  }
  return false;
}

function buildConnectPlan(room, role, includeSecrets = false) {
  const host = FORCE_VPS_RELAY ? RELAY_HOST : room.host;
  const base = {
    matchId: room.matchId,
    host,
    sessionPort: room.port,
    ggpoPort: room.ggpoPort || 0,
    natProbePort: NAT_PROBE_PORT,
  };
  if (includeSecrets) {
    base.roomToken = room.roomToken;
  }

  if (BROKER_GGPO_TRANSPORT === "legacy" || !room.ggpoPort) {
    return { ...base, transport: "legacy_session_tunnel" };
  }

  if (predictP2pPossible(room)) {
    const peer = role === "host" ? room.endpoints?.guest : room.endpoints?.host;
    if (peer?.observedPublic && peer?.ggpoPort) {
      return {
        ...base,
        transport: "p2p",
        peerHost: peer.observedPublic,
        peerPort: peer.ggpoPort,
      };
    }
  }

  return {
    ...base,
    transport: "udp_relay",
    ggpoRemoteHost: host,
    ggpoRemotePort: room.ggpoPort,
  };
}

async function createRoomRecord(displayName, relayHost, sidecarHash) {
  if (rooms.size >= MAX_ROOMS) {
    return { error: "full", message: "Relay is full. Try again in a few minutes or use LAN/direct play." };
  }
  const { sessionPort: port, ggpoPort } = allocatePortPair();
  if (!port) {
    return { error: "full", message: "No relay ports available." };
  }

  const matchId = makeMatchId();
  const roomToken = makeRoomToken();
  const transportRequested = BROKER_GGPO_TRANSPORT === "auto" ? "auto" : "legacy";
  let ggpoRelayOk = false;

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
    if (transportRequested === "auto") {
      const ggpoStarted = await startGgpoRelaySession(port, ggpoPort, roomToken);
      ggpoRelayOk = ggpoStarted.ok;
      if (!ggpoStarted.ok) {
        console.warn(`GGPO relay start failed for port ${ggpoPort}: ${ggpoStarted.message}`);
      }
    }
  }

  let token = makeToken();
  while (rooms.has(`SF4-${token}`)) token = makeToken();
  const code = `SF4-${token}`;
  const hostSecret = makeHostSecret();
  const now = Date.now();
  const roomHost = FORCE_VPS_RELAY ? RELAY_HOST : relayHost || RELAY_HOST;

  const room = {
    code,
    host: roomHost,
    port,
    ggpoPort: transportRequested === "auto" && ggpoRelayOk ? ggpoPort : 0,
    displayName: displayName || "Host",
    createdAt: now,
    lastSeenAt: now,
    sidecarHash: sidecarHash || undefined,
    hostSecret,
    matchId,
    roomToken,
    transportRequested,
    transportActive: null,
    startedAt: null,
    endedAt: null,
    endpoints: { host: null, guest: null },
    events: [],
  };
  pushMatchEvent(room, "room_created", { transportRequested });
  rooms.set(code, room);

  return {
    code,
    host: roomHost,
    port,
    ggpoPort: room.ggpoPort,
    matchId,
    roomToken,
    ggpoTransport: transportRequested,
    token,
    hostSecret,
  };
}

function startNatProbeServer() {
  const socket = dgram.createSocket("udp4");
  socket.on("message", (msg, rinfo) => {
    if (msg.length < 10 || msg.slice(0, 10).toString("ascii") !== "SF4E_PROBE") {
      return;
    }
    if (!allowRate(`nat:${rinfo.address}`, 30, 60_000)) {
      return;
    }
    natProbeByIp.set(rinfo.address, {
      observedIp: rinfo.address,
      observedPort: rinfo.port,
      ts: Date.now(),
    });
    const response = JSON.stringify({
      observedIp: rinfo.address,
      observedPort: rinfo.port,
    });
    socket.send(Buffer.from(response, "utf8"), rinfo.port, rinfo.address);
  });
  socket.on("error", (err) => {
    console.error(`NAT probe socket error: ${err.message}`);
  });
  socket.bind(NAT_PROBE_PORT, NAT_PROBE_BIND, () => {
    console.log(`NAT probe listening on UDP ${NAT_PROBE_BIND}:${NAT_PROBE_PORT}`);
  });
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
      ggpoUdpPortBase: GGPO_UDP_PORT_BASE,
      brokerGgpoTransport: BROKER_GGPO_TRANSPORT,
      natProbePort: NAT_PROBE_PORT,
      roomIdleMs: ROOM_IDLE_MS,
    });
    return;
  }

  if (req.method === "GET" && url.pathname === "/v1/matches") {
    if (!ENABLE_ROOM_LIST) {
      json(res, 404, { error: "not_found", message: "Match listing requires BROKER_ENABLE_ROOM_LIST=1." });
      return;
    }
    const list = [...rooms.values()].map((room) => ({
      ...publicRoomFields(room),
      events: room.events || [],
    }));
    json(res, 200, { ok: true, matches: list });
    return;
  }

  const matchMatch = url.pathname.match(/^\/v1\/matches\/([a-f0-9]{32})$/i);
  if (req.method === "GET" && matchMatch) {
    if (!ENABLE_ROOM_LIST) {
      json(res, 404, { error: "not_found", message: "Match lookup requires BROKER_ENABLE_ROOM_LIST=1." });
      return;
    }
    const matchId = matchMatch[1].toLowerCase();
    const room = [...rooms.values()].find((r) => r.matchId === matchId);
    if (!room) {
      json(res, 404, { error: "not_found", message: "Match not found or expired." });
      return;
    }
    json(res, 200, {
      ok: true,
      match: {
        ...publicRoomFields(room),
        events: room.events || [],
        endpoints: {
          host: room.endpoints?.host
            ? { ggpoPort: room.endpoints.host.ggpoPort, natType: room.endpoints.host.natType }
            : null,
          guest: room.endpoints?.guest
            ? { ggpoPort: room.endpoints.guest.ggpoPort, natType: room.endpoints.guest.natType }
            : null,
        },
      },
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
      .map((r) => publicRoomFields(r));
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
  const connectPlanMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/connect-plan$/i);
  const registerEndpointMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/register-endpoint$/i);
  const roomEventsMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/events$/i);
  const heartbeatMatch = url.pathname.match(/^\/v1\/rooms\/(SF4-[A-Z0-9]+)\/heartbeat$/i);

  if (req.method === "GET" && connectPlanMatch) {
    if (!checkRate(req, "resolve", 30, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many connect-plan requests." });
      return;
    }
    const code = normalizeCode(connectPlanMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, { error: "not_found", message: "Room not found or expired." });
      return;
    }
    room.lastSeenAt = Date.now();
    const role = String(url.searchParams.get("role") || "guest").toLowerCase();
    const relay = await ensureRelaySession(room);
    const planRoom = { ...room };
    if (planRoom.ggpoPort && !relay.ok) {
      planRoom.ggpoPort = 0;
    }
    // Room code is the join secret; include roomToken so clients can register on the GGPO UDP relay.
    const plan = buildConnectPlan(planRoom, role, true);
    json(res, 200, {
      ok: true,
      code,
      relayOk: relay.ok,
      ggpoRelayOk: relay.ggpoRelay?.listening === true || !planRoom.ggpoPort,
      ...plan,
    });
    return;
  }

  if (req.method === "POST" && registerEndpointMatch) {
    if (!checkRate(req, "register", 20, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many endpoint registrations." });
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
    const code = normalizeCode(registerEndpointMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, { error: "not_found", message: "Room not found or expired." });
      return;
    }
    const role = String(body.role || "").toLowerCase();
    if (role !== "host" && role !== "guest") {
      json(res, 400, { error: "invalid_role", message: "role must be host or guest" });
      return;
    }
    if (role === "host" && !verifyHostSecret(room, body.hostSecret)) {
      json(res, 401, { error: "unauthorized", message: "Host secret required for host role." });
      return;
    }
    const ggpoPort = parseInt(body.ggpoPort, 10);
    if (!ggpoPort || ggpoPort < 1 || ggpoPort > 65535) {
      json(res, 400, { error: "invalid_port", message: "ggpoPort required" });
      return;
    }
    const requestIp = clientIp(req);
    const probe = lookupNatProbe(requestIp);
    if (!probe) {
      json(res, 428, {
        error: "nat_probe_required",
        message: "Send SF4E_PROBE to the broker NAT port before registering an endpoint.",
        natProbePort: NAT_PROBE_PORT,
      });
      return;
    }
    const observedPublic = probe.observedIp;
    const endpoint = {
      ggpoPort,
      observedPublic,
      observedPort: probe.observedPort || ggpoPort,
      natType: String(body.natType || "unknown").slice(0, 32),
      registeredAt: Date.now(),
    };
    room.endpoints[role] = endpoint;
    room.lastSeenAt = Date.now();
    pushMatchEvent(room, "endpoint_registered", { role });
    json(res, 200, { ok: true, code, role, connectPlan: buildConnectPlan(room, role, true) });
    return;
  }

  if (req.method === "POST" && roomEventsMatch) {
    if (!checkRate(req, "events", 60, 60_000)) {
      json(res, 429, { error: "rate_limited", message: "Too many event posts." });
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
    const code = normalizeCode(roomEventsMatch[1]);
    const room = rooms.get(code);
    if (!room) {
      json(res, 404, { error: "not_found", message: "Room not found or expired." });
      return;
    }
    const eventType = String(body.type || "").trim();
    if (!eventType) {
      json(res, 400, { error: "invalid_event", message: "type required" });
      return;
    }
    if (!ALLOWED_MATCH_EVENTS.has(eventType)) {
      json(res, 400, { error: "invalid_event", message: "Unknown event type." });
      return;
    }
    const hostOnlyEvents = new Set(["battle_start", "transport_fallback", "desync", "disconnect"]);
    if (hostOnlyEvents.has(eventType) && !verifyHostSecret(room, body.hostSecret)) {
      json(res, 401, { error: "unauthorized", message: "Host secret required." });
      return;
    }
    if (eventType === "battle_start" && !room.startedAt) {
      room.startedAt = Date.now();
    }
    if (body.transportActive) {
      room.transportActive = String(body.transportActive);
    }
    pushMatchEvent(room, eventType, body.detail || {});
    room.lastSeenAt = Date.now();
    json(res, 200, { ok: true, code, matchId: room.matchId });
    return;
  }

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
    const ggpoRelay = room.ggpoPort ? await getGgpoRelayHealth(room.ggpoPort) : { ok: true };
    json(res, 200, {
      ok: relay.ok,
      code: room.code,
      port: room.port,
      ggpoPort: room.ggpoPort,
      relayOk: relay.ok,
      relayListening: relay.relay?.listening === true,
      relayRunning: relay.relay?.running === true,
      ggpoRelayOk: ggpoRelay.ok === true,
      ggpoRelayListening: ggpoRelay.listening === true,
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
    const ggpoRelay = room.ggpoPort ? await getGgpoRelayHealth(room.ggpoPort) : { ok: true };
    json(res, 200, {
      ...publicRoomFields(room),
      relayOk: relay.ok === true && relay.listening === true,
      ggpoRelayOk: ggpoRelay.ok === true && ggpoRelay.listening === true,
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
    endRoom(code, room, "host_delete");
    json(res, 200, { ok: true, code, stopped: true });
    return;
  }

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
      ggpoRelayListening: relay.ggpoRelay?.listening === true || !room.ggpoPort,
      ggpoRelayRestarted: relay.ggpoRestarted === true,
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
        json(res, 413, { error: "payload_too_large", message: "Request body too large" });
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
        ggpoPort: created.ggpoPort,
        matchId: created.matchId,
        roomToken: created.roomToken,
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

startNatProbeServer();

server.listen(PORT, BROKER_BIND, () => {
  console.log(
    `SF4 room broker on ${BROKER_BIND}:${PORT} trustProxy=${TRUST_PROXY} relay=${RELAY_HOST}:${RELAY_PORT_BASE}+ ggpo=${GGPO_UDP_PORT_BASE}+ transport=${BROKER_GGPO_TRANSPORT} forceVpsRelay=${FORCE_VPS_RELAY}`
  );
});
