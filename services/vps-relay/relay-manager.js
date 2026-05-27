/**
 * VPS relay manager — starts/stops sf4e-session-relay and sf4e-ggpo-udp-relay per broker-allocated port.
 * Bind locally; broker calls http://127.0.0.1:8788
 */
const http = require("http");
const { spawn } = require("child_process");
const net = require("net");
const path = require("path");
const dgram = require("dgram");

const PORT = parseInt(process.env.RELAY_MANAGER_PORT || "8788", 10);
const BIND = process.env.RELAY_MANAGER_BIND || "127.0.0.1";
const RELAY_BIN =
  process.env.SF4E_SESSION_RELAY_BIN ||
  path.join(__dirname, "bin", "sf4e-session-relay");
const GGPO_RELAY_BIN =
  process.env.SF4E_GGPO_UDP_RELAY_BIN ||
  path.join(__dirname, "bin", "sf4e-ggpo-udp-relay");
const RELAY_IDENTITY = process.env.RELAY_IDENTITY || "relay-vps";
const MAX_BODY_BYTES = parseInt(process.env.RELAY_MANAGER_MAX_BODY_BYTES || String(64 * 1024), 10);

/** @type {Map<number, { proc: import('child_process').ChildProcess, sidecarHash: string, startedAt: number }>} */
const sessions = new Map();
/** @type {Map<number, { proc: import('child_process').ChildProcess, sessionPort: number, roomToken: string, startedAt: number }>} */
const ggpoSessions = new Map();

function json(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, { "Content-Type": "application/json" });
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

function udpProbe(port, timeoutMs = 1500) {
  return new Promise((resolve) => {
    const socket = dgram.createSocket("udp4");
    let settled = false;
    const finish = (ok) => {
      if (settled) {
        return;
      }
      settled = true;
      try {
        socket.close();
      } catch {
        /* ignore */
      }
      resolve(ok);
    };
    socket.once("error", () => finish(false));
    socket.send(Buffer.alloc(1), port, "127.0.0.1", (err) => {
      finish(!err);
    });
    setTimeout(() => finish(false), timeoutMs);
  });
}

function ggpoHealthProbe(port, timeoutMs = 1500) {
  return new Promise((resolve) => {
    const socket = dgram.createSocket("udp4");
    let settled = false;
    const finish = (ok) => {
      if (settled) {
        return;
      }
      settled = true;
      try {
        socket.close();
      } catch {
        /* ignore */
      }
      resolve(ok);
    };
    socket.once("error", () => finish(false));
    socket.once("message", (msg) => {
      finish(msg.length >= 4 && msg.slice(0, 4).toString("ascii") === "SF4OK");
    });
    socket.send(Buffer.from("SF4H", "ascii"), port, "127.0.0.1", (err) => {
      if (err) {
        finish(false);
      }
    });
    setTimeout(() => finish(false), timeoutMs);
  });
}

function tcpProbe(port, timeoutMs = 1500) {
  return new Promise((resolve) => {
    const socket = net.connect({ host: "127.0.0.1", port, timeout: timeoutMs }, () => {
      socket.end();
      resolve(true);
    });
    socket.on("error", () => resolve(false));
    socket.on("timeout", () => {
      socket.destroy();
      resolve(false);
    });
  });
}

function stopSession(port) {
  const entry = sessions.get(port);
  if (!entry) {
    return false;
  }
  try {
    entry.proc.kill("SIGTERM");
  } catch {
    /* ignore */
  }
  sessions.delete(port);
  return true;
}

function stopGgpoSession(ggpoPort) {
  const entry = ggpoSessions.get(ggpoPort);
  if (!entry) {
    return false;
  }
  try {
    entry.proc.kill("SIGTERM");
  } catch {
    /* ignore */
  }
  ggpoSessions.delete(ggpoPort);
  return true;
}

function stopPairedSession(sessionPort, ggpoPort) {
  const stoppedSession = stopSession(sessionPort);
  const stoppedGgpo = ggpoPort ? stopGgpoSession(ggpoPort) : false;
  return { stoppedSession, stoppedGgpo };
}

function startSession(port, sidecarHash) {
  if (sessions.has(port)) {
    stopSession(port);
  }

  const args = [
    "--port",
    String(port),
    "--sidecar-hash",
    sidecarHash,
    "--identity",
    RELAY_IDENTITY,
    "--idle-exit-sec",
    "0",
  ];

  const proc = spawn(RELAY_BIN, args, {
    stdio: ["ignore", "pipe", "pipe"],
    detached: false,
    env: {
      ...process.env,
      LD_LIBRARY_PATH: [
        path.join(__dirname, "lib"),
        process.env.LD_LIBRARY_PATH || "",
      ]
        .filter(Boolean)
        .join(":"),
    },
  });

  proc.stdout.on("data", (chunk) => {
    process.stdout.write(`[relay:${port}] ${chunk}`);
  });
  proc.stderr.on("data", (chunk) => {
    process.stderr.write(`[relay:${port}] ${chunk}`);
  });
  proc.on("exit", (code, signal) => {
    if (sessions.get(port)?.proc === proc) {
      sessions.delete(port);
    }
    console.log(`[relay:${port}] exited code=${code} signal=${signal || ""}`);
  });

  sessions.set(port, { proc, sidecarHash, startedAt: Date.now() });
  return proc;
}

function startGgpoSession(ggpoPort, sessionPort, roomToken) {
  if (ggpoSessions.has(ggpoPort)) {
    stopGgpoSession(ggpoPort);
  }

  const args = [
    "--port",
    String(ggpoPort),
    "--room-token",
    roomToken,
    "--idle-exit-sec",
    "900",
  ];

  const proc = spawn(GGPO_RELAY_BIN, args, {
    stdio: ["ignore", "pipe", "pipe"],
    detached: false,
  });

  proc.stdout.on("data", (chunk) => {
    process.stdout.write(`[ggpo:${ggpoPort}] ${chunk}`);
  });
  proc.stderr.on("data", (chunk) => {
    process.stderr.write(`[ggpo:${ggpoPort}] ${chunk}`);
  });
  proc.on("exit", (code, signal) => {
    if (ggpoSessions.get(ggpoPort)?.proc === proc) {
      ggpoSessions.delete(ggpoPort);
    }
    console.log(`[ggpo:${ggpoPort}] exited code=${code} signal=${signal || ""}`);
  });

  ggpoSessions.set(ggpoPort, { proc, sessionPort, roomToken, startedAt: Date.now() });
  return proc;
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${BIND}:${PORT}`);

  if (req.method === "GET" && url.pathname === "/v1/health") {
    json(res, 200, {
      ok: true,
      sessions: sessions.size,
      ggpoSessions: ggpoSessions.size,
      relayBin: RELAY_BIN,
      ggpoRelayBin: GGPO_RELAY_BIN,
      identity: RELAY_IDENTITY,
      bind: BIND,
      port: PORT,
    });
    return;
  }

  if (req.method === "GET" && url.pathname === "/v1/sessions") {
    const list = [...sessions.entries()].map(([port, s]) => ({
      port,
      sidecarHash: s.sidecarHash,
      startedAt: s.startedAt,
      pid: s.proc.pid,
    }));
    json(res, 200, { ok: true, sessions: list });
    return;
  }

  if (req.method === "GET" && url.pathname === "/v1/ggpo-sessions") {
    const list = [...ggpoSessions.entries()].map(([port, s]) => ({
      port,
      sessionPort: s.sessionPort,
      roomToken: `${s.roomToken.slice(0, 8)}...`,
      startedAt: s.startedAt,
      pid: s.proc.pid,
    }));
    json(res, 200, { ok: true, sessions: list });
    return;
  }

  const healthMatch = url.pathname.match(/^\/v1\/sessions\/(\d+)\/health$/);
  if (req.method === "GET" && healthMatch) {
    const port = parseInt(healthMatch[1], 10);
    const running = sessions.has(port);
    const listening = running ? await udpProbe(port) : false;
    json(res, 200, { ok: running && listening, port, running, listening });
    return;
  }

  const ggpoHealthMatch = url.pathname.match(/^\/v1\/ggpo-sessions\/(\d+)\/health$/);
  if (req.method === "GET" && ggpoHealthMatch) {
    const port = parseInt(ggpoHealthMatch[1], 10);
    const running = ggpoSessions.has(port);
    const listening = running ? await ggpoHealthProbe(port) : false;
    json(res, 200, { ok: running && listening, port, running, listening });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/sessions") {
    const body = await readBody(req);
    const port = parseInt(body.port, 10);
    const sidecarHash = String(body.sidecarHash || "").trim();
    if (!port || port < 1 || port > 65535) {
      json(res, 400, { error: "invalid_port", message: "port required" });
      return;
    }
    if (!sidecarHash) {
      json(res, 400, { error: "invalid_hash", message: "sidecarHash required" });
      return;
    }

    startSession(port, sidecarHash);

    let ready = false;
    for (let i = 0; i < 30; i++) {
      // eslint-disable-next-line no-await-in-loop
      if (await udpProbe(port, 500)) {
        ready = true;
        break;
      }
      // eslint-disable-next-line no-await-in-loop
      await new Promise((r) => setTimeout(r, 100));
    }

    if (!ready) {
      stopSession(port);
      json(res, 503, {
        error: "start_failed",
        message: `sf4e-session-relay did not listen on port ${port}`,
      });
      return;
    }

    json(res, 201, { ok: true, port, sidecarHash });
    return;
  }

  if (req.method === "POST" && url.pathname === "/v1/ggpo-sessions") {
    const body = await readBody(req);
    const ggpoPort = parseInt(body.ggpoPort, 10);
    const sessionPort = parseInt(body.sessionPort, 10);
    const roomToken = String(body.roomToken || "").trim();
    if (!ggpoPort || ggpoPort < 1 || ggpoPort > 65535) {
      json(res, 400, { error: "invalid_port", message: "ggpoPort required" });
      return;
    }
    if (!roomToken || !/^[a-f0-9]{32}$/i.test(roomToken)) {
      json(res, 400, { error: "invalid_token", message: "roomToken must be 32 hex chars" });
      return;
    }

    startGgpoSession(ggpoPort, sessionPort, roomToken);

    let ready = false;
    for (let i = 0; i < 30; i++) {
      // eslint-disable-next-line no-await-in-loop
      if (await ggpoHealthProbe(ggpoPort, 500)) {
        ready = true;
        break;
      }
      // eslint-disable-next-line no-await-in-loop
      await new Promise((r) => setTimeout(r, 100));
    }

    if (!ready) {
      stopGgpoSession(ggpoPort);
      json(res, 503, {
        error: "start_failed",
        message: `sf4e-ggpo-udp-relay did not listen on port ${ggpoPort}`,
      });
      return;
    }

    json(res, 201, { ok: true, ggpoPort, sessionPort });
    return;
  }

  const deleteMatch = url.pathname.match(/^\/v1\/sessions\/(\d+)$/);
  if (req.method === "DELETE" && deleteMatch) {
    const port = parseInt(deleteMatch[1], 10);
    const stopped = stopSession(port);
    json(res, 200, { ok: true, port, stopped });
    return;
  }

  const ggpoDeleteMatch = url.pathname.match(/^\/v1\/ggpo-sessions\/(\d+)$/);
  if (req.method === "DELETE" && ggpoDeleteMatch) {
    const port = parseInt(ggpoDeleteMatch[1], 10);
    const stopped = stopGgpoSession(port);
    json(res, 200, { ok: true, port, stopped });
    return;
  }

  json(res, 404, { error: "not_found" });
});

server.listen(PORT, BIND, () => {
  console.log(
    `SF4 relay manager on http://${BIND}:${PORT} session=${RELAY_BIN} ggpo=${GGPO_RELAY_BIN}`
  );
});

module.exports = {
  stopPairedSession,
};
