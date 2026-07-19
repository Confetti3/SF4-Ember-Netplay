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
const KILL_GRACE_MS = parseInt(process.env.RELAY_MANAGER_KILL_GRACE_MS || "3000", 10);

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
      finish(msg.length >= 5 && msg.slice(0, 5).toString("ascii") === "SF4OK");
    });
    socket.bind(0, "127.0.0.1", () => {
      socket.send(Buffer.from("SF4H", "ascii"), port, "127.0.0.1", (err) => {
        if (err) {
          finish(false);
        }
      });
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

// Send SIGTERM, then SIGKILL if the process has not exited within the grace
// window. Without the forced follow-up a hung relay binary keeps its port
// bound after we have already dropped it from our map, so the next session
// spawned on that port fails to bind and reports a misleading start_failure.
function terminateProcess(proc, label) {
  try {
    proc.kill("SIGTERM");
  } catch {
    /* ignore */
  }
  if (proc.exitCode !== null || proc.signalCode !== null) {
    return;
  }
  const timer = setTimeout(() => {
    try {
      proc.kill("SIGKILL");
      console.log(`[${label}] force-killed after ${KILL_GRACE_MS}ms grace`);
    } catch {
      /* ignore */
    }
  }, KILL_GRACE_MS);
  if (typeof timer.unref === "function") {
    timer.unref();
  }
  proc.once("exit", () => clearTimeout(timer));
}

function waitForProcessExit(proc, timeoutMs = KILL_GRACE_MS + 1000) {
  if (proc.exitCode !== null || proc.signalCode !== null) {
    return Promise.resolve();
  }
  return new Promise((resolve) => {
    let settled = false;
    const finish = () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve();
    };
    const timer = setTimeout(finish, timeoutMs);
    proc.once("exit", finish);
    proc.once("error", finish);
  });
}

function stopSession(port) {
  const entry = sessions.get(port);
  if (!entry) {
    return false;
  }
  terminateProcess(entry.proc, `relay:${port}`);
  sessions.delete(port);
  return true;
}

function stopGgpoSession(ggpoPort) {
  const entry = ggpoSessions.get(ggpoPort);
  if (!entry) {
    return false;
  }
  terminateProcess(entry.proc, `ggpo:${ggpoPort}`);
  ggpoSessions.delete(ggpoPort);
  return true;
}

async function stopGgpoSessionAndWait(ggpoPort) {
  const entry = ggpoSessions.get(ggpoPort);
  if (!entry) {
    return false;
  }
  terminateProcess(entry.proc, `ggpo:${ggpoPort}`);
  ggpoSessions.delete(ggpoPort);
  await waitForProcessExit(entry.proc);
  if (entry.proc.exitCode === null && entry.proc.signalCode === null) {
    throw new Error(`GGPO relay ${ggpoPort} did not exit`);
  }
  for (let i = 0; i < 20; i++) {
    // eslint-disable-next-line no-await-in-loop
    if (!(await ggpoHealthProbe(ggpoPort, 100))) {
      return true;
    }
    // eslint-disable-next-line no-await-in-loop
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`GGPO relay UDP port ${ggpoPort} did not release`);
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
  // A ChildProcess emits 'error' (not 'exit') when the binary cannot be
  // spawned at all (missing file, EACCES, ...). Without this listener that
  // becomes an unhandled 'error' event and crashes the whole manager,
  // taking down every other active session on the box.
  proc.on("error", (err) => {
    if (sessions.get(port)?.proc === proc) {
      sessions.delete(port);
    }
    console.error(`[relay:${port}] spawn error: ${err && err.message}`);
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

async function startGgpoSession(ggpoPort, sessionPort, roomToken) {
  const existing = ggpoSessions.get(ggpoPort);
  if (
    existing &&
    existing.sessionPort === sessionPort &&
    existing.roomToken === roomToken &&
    existing.proc.exitCode === null &&
    existing.proc.signalCode === null
  ) {
    return { proc: existing.proc, reused: true };
  }
  if (existing) {
    await stopGgpoSessionAndWait(ggpoPort);
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
    stdio: ["ignore", "inherit", "inherit"],
    detached: false,
  });

  // See startSession: without an 'error' listener a spawn failure crashes the
  // whole manager instead of just failing this one session.
  proc.on("error", (err) => {
    if (ggpoSessions.get(ggpoPort)?.proc === proc) {
      ggpoSessions.delete(ggpoPort);
    }
    console.error(`[ggpo:${ggpoPort}] spawn error: ${err && err.message}`);
  });
  proc.on("exit", (code, signal) => {
    if (ggpoSessions.get(ggpoPort)?.proc === proc) {
      ggpoSessions.delete(ggpoPort);
    }
    console.log(`[ggpo:${ggpoPort}] exited code=${code} signal=${signal || ""}`);
  });

  ggpoSessions.set(ggpoPort, { proc, sessionPort, roomToken, startedAt: Date.now() });
  return { proc, reused: false };
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

    let startResult;
    try {
      startResult = await startGgpoSession(ggpoPort, sessionPort, roomToken);
    } catch (err) {
      json(res, 503, {
        error: "replace_failed",
        message: err && err.message ? err.message : String(err),
      });
      return;
    }

    if (!startResult.reused) {
      await new Promise((r) => setTimeout(r, 300));
    }

    let ready = false;
    for (let i = 0; i < 40; i++) {
      // eslint-disable-next-line no-await-in-loop
      if (await ggpoHealthProbe(ggpoPort, 1000)) {
        ready = true;
        break;
      }
      // eslint-disable-next-line no-await-in-loop
      await new Promise((r) => setTimeout(r, 100));
    }

    if (!ready) {
      try {
        await stopGgpoSessionAndWait(ggpoPort);
      } catch (err) {
        console.error(`[ggpo:${ggpoPort}] cleanup failed: ${err && err.message}`);
      }
      json(res, 503, {
        error: "start_failed",
        message: `sf4e-ggpo-udp-relay did not listen on port ${ggpoPort}`,
      });
      return;
    }

    json(res, startResult.reused ? 200 : 201, {
      ok: true,
      ggpoPort,
      sessionPort,
      reused: startResult.reused,
    });
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
    try {
      const stopped = await stopGgpoSessionAndWait(port);
      json(res, 200, { ok: true, port, stopped });
    } catch (err) {
      json(res, 503, {
        ok: false,
        port,
        error: "stop_failed",
        message: err && err.message ? err.message : String(err),
      });
    }
    return;
  }

  json(res, 404, { error: "not_found" });
});

// Last-resort guard: keep the manager alive if an unexpected rejection slips
// through, rather than letting it crash and kill every active relay session.
process.on("unhandledRejection", (reason) => {
  console.error("relay-manager unhandledRejection:", reason);
});

server.listen(PORT, BIND, () => {
  console.log(
    `SF4 relay manager on http://${BIND}:${PORT} session=${RELAY_BIN} ggpo=${GGPO_RELAY_BIN}`
  );
});

module.exports = {
  stopPairedSession,
};
