/**
 * VPS relay manager — starts/stops sf4e-session-relay per broker-allocated port.
 * Bind locally; broker calls http://127.0.0.1:8788
 */
const http = require("http");
const { spawn } = require("child_process");
const net = require("net");
const path = require("path");

const PORT = parseInt(process.env.RELAY_MANAGER_PORT || "8788", 10);
const BIND = process.env.RELAY_MANAGER_BIND || "127.0.0.1";
const RELAY_BIN =
  process.env.SF4E_SESSION_RELAY_BIN ||
  path.join(__dirname, "bin", "sf4e-session-relay");
const RELAY_IDENTITY = process.env.RELAY_IDENTITY || "relay-vps";

/** @type {Map<number, { proc: import('child_process').ChildProcess, sidecarHash: string, startedAt: number }>} */
const sessions = new Map();

function json(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, { "Content-Type": "application/json" });
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

function udpProbe(port, timeoutMs = 1500) {
  return new Promise((resolve) => {
    const socket = require("dgram").createSocket("udp4");
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

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${BIND}:${PORT}`);

  if (req.method === "GET" && url.pathname === "/v1/health") {
    json(res, 200, {
      ok: true,
      sessions: sessions.size,
      relayBin: RELAY_BIN,
    });
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

  const deleteMatch = url.pathname.match(/^\/v1\/sessions\/(\d+)$/);
  if (req.method === "DELETE" && deleteMatch) {
    const port = parseInt(deleteMatch[1], 10);
    const stopped = stopSession(port);
    json(res, 200, { ok: true, port, stopped });
    return;
  }

  json(res, 404, { error: "not_found" });
});

server.listen(PORT, BIND, () => {
  console.log(`SF4 relay manager on http://${BIND}:${PORT} bin=${RELAY_BIN}`);
});
