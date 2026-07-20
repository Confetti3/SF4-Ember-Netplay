/**
 * Room capacity / port allocation tests for the VPS broker.
 * Run: node --test services/room-broker/test/*.test.js
 */

const { describe, it, before, after } = require("node:test");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const http = require("node:http");
const path = require("node:path");

const {
  loadCapacityConfig,
  allocatePortPair,
  releasePortReservation,
  rangesOverlap,
  DEFAULT_MAX_ROOMS,
  DEFAULT_RELAY_PORT_BASE,
  DEFAULT_GGPO_UDP_PORT_BASE,
} = require("../capacity-config");

const BROKER_DIR = path.join(__dirname, "..");
const SERVER_JS = path.join(BROKER_DIR, "server.js");

describe("capacity-config defaults", () => {
  it("defaults to 50 rooms and documented port ranges", () => {
    const cfg = loadCapacityConfig({});
    assert.equal(cfg.maxRooms, 50);
    assert.equal(cfg.maxRooms, DEFAULT_MAX_ROOMS);
    assert.equal(cfg.relayPortBase, DEFAULT_RELAY_PORT_BASE);
    assert.equal(cfg.ggpoUdpPortBase, DEFAULT_GGPO_UDP_PORT_BASE);
    assert.equal(cfg.relayPortBase, 23456);
    assert.equal(cfg.relayPortEnd, 23505);
    assert.equal(cfg.ggpoUdpPortBase, 24456);
    assert.equal(cfg.ggpoUdpPortEnd, 24505);
    assert.equal(cfg.maxActivePlayers, 100);
    assert.equal(cfg.maxClients, 200);
  });

  it("honors MAX_ROOMS override", () => {
    const cfg = loadCapacityConfig({ MAX_ROOMS: "3" });
    assert.equal(cfg.maxRooms, 3);
    assert.equal(cfg.relayPortEnd, 23458);
    assert.equal(cfg.ggpoUdpPortEnd, 24458);
  });
});

describe("capacity-config validation", () => {
  const invalidCases = [
    [{ MAX_ROOMS: "0" }, /MAX_ROOMS must be >= 1/],
    [{ MAX_ROOMS: "-5" }, /MAX_ROOMS must be >= 1/],
    [{ MAX_ROOMS: "abc" }, /MAX_ROOMS must be an integer/],
    [{ MAX_ROOMS: "12.5" }, /MAX_ROOMS must be an integer/],
    [{ RELAY_PORT_BASE: "0" }, /RELAY_PORT_BASE must be >= 1/],
    [{ RELAY_PORT_BASE: "65530", MAX_ROOMS: "20" }, /Session relay range exceeds/],
    [{ GGPO_UDP_PORT_BASE: "65530", MAX_ROOMS: "20" }, /GGPO UDP relay range exceeds/],
    [
      { RELAY_PORT_BASE: "23456", GGPO_UDP_PORT_BASE: "23460", MAX_ROOMS: "20" },
      /overlaps/,
    ],
  ];

  for (const [env, pattern] of invalidCases) {
    it(`rejects ${JSON.stringify(env)}`, () => {
      assert.throws(() => loadCapacityConfig(env), pattern);
    });
  }
});

describe("port allocation", () => {
  it("allocates unique session and GGPO ports across full default capacity", () => {
    const cfg = loadCapacityConfig({});
    const rooms = [];
    const reserved = new Set();
    const sessionPorts = new Set();
    const ggpoPorts = new Set();

    for (let i = 0; i < cfg.maxRooms; i++) {
      const pair = allocatePortPair(cfg, rooms, reserved);
      assert.notEqual(pair.sessionPort, 0);
      assert.ok(!sessionPorts.has(pair.sessionPort));
      assert.ok(!ggpoPorts.has(pair.ggpoPort));
      sessionPorts.add(pair.sessionPort);
      ggpoPorts.add(pair.ggpoPort);
      rooms.push({ port: pair.sessionPort, ggpoPort: pair.ggpoPort });
      releasePortReservation(reserved, pair.sessionPort);
    }

    assert.equal(Math.min(...sessionPorts), 23456);
    assert.equal(Math.max(...sessionPorts), 23505);
    assert.equal(Math.min(...ggpoPorts), 24456);
    assert.equal(Math.max(...ggpoPorts), 24505);

    const overflow = allocatePortPair(cfg, rooms, reserved);
    assert.deepEqual(overflow, { sessionPort: 0, ggpoPort: 0 });
  });

  it("reuses a freed port only after the room is removed", () => {
    const cfg = loadCapacityConfig({ MAX_ROOMS: "3" });
    const rooms = [];
    const reserved = new Set();

    const a = allocatePortPair(cfg, rooms, reserved);
    rooms.push({ port: a.sessionPort });
    releasePortReservation(reserved, a.sessionPort);

    const b = allocatePortPair(cfg, rooms, reserved);
    rooms.push({ port: b.sessionPort });
    releasePortReservation(reserved, b.sessionPort);

    const c = allocatePortPair(cfg, rooms, reserved);
    rooms.push({ port: c.sessionPort });
    releasePortReservation(reserved, c.sessionPort);

    assert.equal(allocatePortPair(cfg, rooms, reserved).sessionPort, 0);

    // End middle room; next allocation must reclaim that port, not an active one.
    rooms.splice(1, 1);
    const reused = allocatePortPair(cfg, rooms, reserved);
    assert.equal(reused.sessionPort, b.sessionPort);
    assert.equal(reused.ggpoPort, b.ggpoPort);
    assert.notEqual(reused.sessionPort, a.sessionPort);
    assert.notEqual(reused.sessionPort, c.sessionPort);
  });

  it("prevents concurrent creators from receiving the same reserved port", () => {
    const cfg = loadCapacityConfig({ MAX_ROOMS: "2" });
    const rooms = [];
    const reserved = new Set();

    const first = allocatePortPair(cfg, rooms, reserved);
    const second = allocatePortPair(cfg, rooms, reserved);
    assert.notEqual(first.sessionPort, 0);
    assert.notEqual(second.sessionPort, 0);
    assert.notEqual(first.sessionPort, second.sessionPort);
    assert.notEqual(first.ggpoPort, second.ggpoPort);
    assert.equal(reserved.size, 2);

    const third = allocatePortPair(cfg, rooms, reserved);
    assert.deepEqual(third, { sessionPort: 0, ggpoPort: 0 });
  });

  it("detects overlapping ranges", () => {
    assert.equal(rangesOverlap(23456, 23505, 24456, 24505), false);
    assert.equal(rangesOverlap(23456, 23505, 23500, 23520), true);
  });
});

function httpJson(method, url, body) {
  return new Promise((resolve, reject) => {
    const payload = body == null ? null : Buffer.from(JSON.stringify(body), "utf8");
    const req = http.request(
      url,
      {
        method,
        headers: payload
          ? {
              "Content-Type": "application/json",
              "Content-Length": payload.length,
            }
          : {},
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          let json = null;
          try {
            json = text ? JSON.parse(text) : null;
          } catch (_err) {
            json = { raw: text };
          }
          resolve({ status: res.statusCode, body: json });
        });
      }
    );
    req.on("error", reject);
    if (payload) req.write(payload);
    req.end();
  });
}

function waitForHealth(baseUrl, timeoutMs = 8000) {
  const started = Date.now();
  return new Promise((resolve, reject) => {
    const tick = async () => {
      try {
        const res = await httpJson("GET", `${baseUrl}/v1/health`);
        if (res.status === 200 && res.body && res.body.ok) {
          resolve(res.body);
          return;
        }
      } catch (_err) {
        // retry
      }
      if (Date.now() - started > timeoutMs) {
        reject(new Error("broker health timed out"));
        return;
      }
      setTimeout(tick, 100);
    };
    tick();
  });
}

describe("broker HTTP capacity behavior", () => {
  let child;
  let baseUrl;
  const port = 18787 + Math.floor(Math.random() * 1000);
  const natPort = port + 1;

  before(async () => {
    child = spawn(
      process.execPath,
      [SERVER_JS],
      {
        cwd: BROKER_DIR,
        env: {
          ...process.env,
          PORT: String(port),
          BROKER_BIND: "127.0.0.1",
          NAT_PROBE_PORT: String(natPort),
          NAT_PROBE_BIND: "127.0.0.1",
          FORCE_VPS_RELAY: "0",
          BROKER_GGPO_TRANSPORT: "legacy",
          MAX_ROOMS: "3",
          RELAY_PORT_BASE: "23456",
          GGPO_UDP_PORT_BASE: "24456",
        },
        stdio: ["ignore", "pipe", "pipe"],
      }
    );
    baseUrl = `http://127.0.0.1:${port}`;
    let stderr = "";
    child.stderr.on("data", (d) => {
      stderr += d.toString();
    });
    child.on("exit", (code) => {
      if (code && code !== 0) {
        console.error("broker exited:", code, stderr);
      }
    });
    await waitForHealth(baseUrl);
  });

  after(async () => {
    if (child && !child.killed) {
      child.kill("SIGTERM");
      await new Promise((resolve) => child.on("exit", resolve));
    }
  });

  it("reports configured maxRooms and port ends", async () => {
    const health = await httpJson("GET", `${baseUrl}/v1/health`);
    assert.equal(health.status, 200);
    assert.equal(health.body.maxRooms, 3);
    assert.equal(health.body.relayPortBase, 23456);
    assert.equal(health.body.relayPortEnd, 23458);
    assert.equal(health.body.ggpoUdpPortBase, 24456);
    assert.equal(health.body.ggpoUdpPortEnd, 24458);
  });

  it("allows exactly MAX_ROOMS creations then returns stable capacity error", async () => {
    const created = [];
    for (let i = 0; i < 3; i++) {
      const res = await httpJson("POST", `${baseUrl}/v1/rooms`, {
        displayName: `Host${i}`,
        relayHost: "127.0.0.1",
      });
      assert.equal(res.status, 201, JSON.stringify(res.body));
      created.push(res.body);
    }

    const ports = created.map((r) => r.port);
    assert.equal(new Set(ports).size, 3);

    const full = await httpJson("POST", `${baseUrl}/v1/rooms`, {
      displayName: "Overflow",
      relayHost: "127.0.0.1",
    });
    assert.equal(full.status, 503);
    assert.equal(full.body.error, "full");
    assert.match(
      full.body.message,
      /currently at room capacity/i
    );

    // Free one room and confirm reuse.
    const victim = created[1];
    const del = await httpJson("DELETE", `${baseUrl}/v1/rooms/${victim.code}`, {
      hostSecret: victim.hostSecret,
    });
    assert.equal(del.status, 200);

    const reused = await httpJson("POST", `${baseUrl}/v1/rooms`, {
      displayName: "Reuse",
      relayHost: "127.0.0.1",
    });
    assert.equal(reused.status, 201);
    assert.equal(reused.body.port, victim.port);
  });
});

describe("broker default MAX_ROOMS via subprocess", () => {
  it("starts with maxRooms=50 when MAX_ROOMS is unset", async () => {
    const port = 19787 + Math.floor(Math.random() * 500);
    const natPort = port + 1;
    const env = { ...process.env };
    delete env.MAX_ROOMS;
    const child = spawn(process.execPath, [SERVER_JS], {
      cwd: BROKER_DIR,
      env: {
        ...env,
        PORT: String(port),
        BROKER_BIND: "127.0.0.1",
        NAT_PROBE_PORT: String(natPort),
        NAT_PROBE_BIND: "127.0.0.1",
        FORCE_VPS_RELAY: "0",
        BROKER_GGPO_TRANSPORT: "legacy",
      },
      stdio: ["ignore", "pipe", "pipe"],
    });
    try {
      const health = await waitForHealth(`http://127.0.0.1:${port}`);
      assert.equal(health.maxRooms, 50);
      assert.equal(health.relayPortEnd, 23505);
      assert.equal(health.ggpoUdpPortEnd, 24505);
    } finally {
      child.kill("SIGTERM");
      await new Promise((resolve) => child.on("exit", resolve));
    }
  });

  it("exits nonzero on invalid MAX_ROOMS", async () => {
    const child = spawn(process.execPath, [SERVER_JS], {
      cwd: BROKER_DIR,
      env: {
        ...process.env,
        MAX_ROOMS: "0",
        PORT: "19999",
        BROKER_BIND: "127.0.0.1",
        NAT_PROBE_PORT: "19998",
        NAT_PROBE_BIND: "127.0.0.1",
        FORCE_VPS_RELAY: "0",
      },
      stdio: ["ignore", "pipe", "pipe"],
    });
    const code = await new Promise((resolve) => child.on("exit", resolve));
    assert.notEqual(code, 0);
  });
});
