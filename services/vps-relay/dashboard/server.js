/**
 * SF4e VPS Relay Dashboard — authenticated web UI for relay-manager status.
 */
require("dotenv").config();

const crypto = require("crypto");
const os = require("os");
const express = require("express");
const bcrypt = require("bcryptjs");

const PORT = parseInt(process.env.DASHBOARD_PORT || "8789", 10);
const BIND = process.env.DASHBOARD_BIND || "127.0.0.1";
const ADMIN_USERNAME = process.env.ADMIN_USERNAME || "admin";
const ADMIN_PASSWORD_HASH = process.env.ADMIN_PASSWORD_HASH || "";
const SESSION_SECRET = process.env.SESSION_SECRET || "";
const SESSION_TTL_SEC = parseInt(process.env.SESSION_TTL_SEC || "28800", 10);
const RELAY_MANAGER_URL =
  process.env.RELAY_MANAGER_URL || "http://127.0.0.1:8788";
const BROKER_URL = process.env.BROKER_URL || "http://127.0.0.1:8787";
const COOKIE_SECURE =
  process.env.COOKIE_SECURE === "1" ||
  (process.env.COOKIE_SECURE !== "0" && BIND === "127.0.0.1");
const TRUST_PROXY = process.env.DASHBOARD_TRUST_PROXY !== "0";
const STARTED_AT = Date.now();

const SESSION_COOKIE = "sf4e_relay_session";

/** @type {Map<string, { username: string, expiresAt: number }>} */
const sessions = new Map();

const app = express();
if (TRUST_PROXY) {
  app.set("trust proxy", ["127.0.0.1", "::1", "loopback"]);
}
app.use(express.urlencoded({ extended: false }));
app.use(express.json());

/** @type {Map<string, { count: number, resetAt: number }>} */
const loginAttempts = new Map();
const LOGIN_RATE_LIMIT = 5;
const LOGIN_RATE_WINDOW_MS = 15 * 60 * 1000;

function clientIp(req) {
  if (TRUST_PROXY && req.ip) {
    return req.ip;
  }
  const forwarded = req.headers["x-forwarded-for"];
  if (typeof forwarded === "string" && forwarded.trim()) {
    return forwarded.split(",")[0].trim();
  }
  return req.socket.remoteAddress || "unknown";
}

function isLoginRateLimited(ip) {
  const now = Date.now();
  const entry = loginAttempts.get(ip);
  if (!entry || now > entry.resetAt) {
    return false;
  }
  return entry.count >= LOGIN_RATE_LIMIT;
}

function recordLoginFailure(ip) {
  const now = Date.now();
  const entry = loginAttempts.get(ip);
  if (!entry || now > entry.resetAt) {
    loginAttempts.set(ip, { count: 1, resetAt: now + LOGIN_RATE_WINDOW_MS });
    return;
  }
  entry.count += 1;
  loginAttempts.set(ip, entry);
}

function clearLoginFailures(ip) {
  loginAttempts.delete(ip);
}

setInterval(() => {
  const now = Date.now();
  for (const [ip, entry] of loginAttempts.entries()) {
    if (now > entry.resetAt) {
      loginAttempts.delete(ip);
    }
  }
}, 60_000);

function failStartup(message) {
  console.error(`FATAL: ${message}`);
  process.exit(1);
}

if (!ADMIN_PASSWORD_HASH) {
  failStartup("ADMIN_PASSWORD_HASH is required. Copy .env.example to .env and set it.");
}
if (!SESSION_SECRET || SESSION_SECRET.length < 32) {
  failStartup("SESSION_SECRET must be at least 32 characters.");
}

function signToken(token) {
  return crypto
    .createHmac("sha256", SESSION_SECRET)
    .update(token)
    .digest("hex");
}

function createSession(username) {
  const token = crypto.randomBytes(32).toString("hex");
  const expiresAt = Date.now() + SESSION_TTL_SEC * 1000;
  sessions.set(token, { username, expiresAt });
  return { token, expiresAt };
}

function parseCookie(req, name) {
  const header = req.headers.cookie;
  if (!header) {
    return null;
  }
  for (const part of header.split(";")) {
    const [key, ...rest] = part.trim().split("=");
    if (key === name) {
      return decodeURIComponent(rest.join("="));
    }
  }
  return null;
}

function getSession(req) {
  const raw = parseCookie(req, SESSION_COOKIE);
  if (!raw) {
    return null;
  }
  const dot = raw.lastIndexOf(".");
  if (dot <= 0) {
    return null;
  }
  const token = raw.slice(0, dot);
  const sig = raw.slice(dot + 1);
  if (signToken(token) !== sig) {
    return null;
  }
  const session = sessions.get(token);
  if (!session) {
    return null;
  }
  if (Date.now() > session.expiresAt) {
    sessions.delete(token);
    return null;
  }
  return { token, ...session };
}

function setSessionCookie(res, token) {
  const value = `${token}.${signToken(token)}`;
  const parts = [
    `${SESSION_COOKIE}=${encodeURIComponent(value)}`,
    "HttpOnly",
    "SameSite=Strict",
    "Path=/",
    `Max-Age=${SESSION_TTL_SEC}`,
  ];
  if (COOKIE_SECURE) {
    parts.push("Secure");
  }
  res.setHeader("Set-Cookie", parts.join("; "));
}

function clearSessionCookie(res) {
  const parts = [
    `${SESSION_COOKIE}=`,
    "HttpOnly",
    "SameSite=Strict",
    "Path=/",
    "Max-Age=0",
  ];
  if (COOKIE_SECURE) {
    parts.push("Secure");
  }
  res.setHeader("Set-Cookie", parts.join("; "));
}

function requireAuth(req, res, next) {
  const session = getSession(req);
  if (!session) {
    if (req.path.startsWith("/api/")) {
      res.status(401).json({ error: "unauthorized" });
      return;
    }
    res.redirect("/login");
    return;
  }
  req.session = session;
  next();
}

async function relayFetch(path, options = {}) {
  const url = `${RELAY_MANAGER_URL}${path}`;
  const res = await fetch(url, options);
  const text = await res.text();
  let body;
  try {
    body = text ? JSON.parse(text) : {};
  } catch {
    body = { raw: text };
  }
  return { status: res.status, body };
}

async function brokerFetch(path, options = {}) {
  const url = `${BROKER_URL}${path}`;
  const res = await fetch(url, options);
  const text = await res.text();
  let body;
  try {
    body = text ? JSON.parse(text) : {};
  } catch {
    body = { raw: text };
  }
  return { status: res.status, body };
}

function formatDuration(ms) {
  const sec = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) {
    return `${h}h ${m}m ${s}s`;
  }
  if (m > 0) {
    return `${m}m ${s}s`;
  }
  return `${s}s`;
}

function getSystemStats() {
  const totalMem = os.totalmem();
  const freeMem = os.freemem();
  const usedMem = totalMem - freeMem;
  const load = os.loadavg();
  return {
    hostname: os.hostname(),
    uptimeSec: Math.floor(os.uptime()),
    uptime: formatDuration(os.uptime() * 1000),
    memoryUsedMb: Math.round(usedMem / (1024 * 1024)),
    memoryTotalMb: Math.round(totalMem / (1024 * 1024)),
    memoryUsedPct: totalMem > 0 ? Math.round((usedMem / totalMem) * 100) : 0,
    loadAvg: load.map((v) => Math.round(v * 100) / 100),
    nodeVersion: process.version,
    dashboardUptime: formatDuration(Date.now() - STARTED_AT),
  };
}

function formatUptime(startedAt) {
  return formatDuration(Date.now() - startedAt);
}

function truncateHash(hash) {
  if (!hash || hash.length <= 16) {
    return hash || "—";
  }
  return `${hash.slice(0, 8)}…${hash.slice(-8)}`;
}

function page(title, body) {
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${title} — SF4e Relay</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #0f1117;
      --panel: #171a22;
      --border: #2a3140;
      --text: #e8ecf4;
      --muted: #9aa4b8;
      --accent: #4f8cff;
      --danger: #e05a5a;
      --ok: #4caf82;
      --warn: #d4a017;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      line-height: 1.5;
    }
    .wrap { max-width: 1200px; margin: 0 auto; padding: 24px; }
    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      margin-bottom: 24px;
    }
    h1 { margin: 0; font-size: 1.5rem; }
    .card {
      background: var(--panel);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 20px;
      margin-bottom: 16px;
    }
    .stats {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 12px;
    }
    .stat label {
      display: block;
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      color: var(--muted);
      margin-bottom: 4px;
    }
    .stat strong { font-size: 1.1rem; }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.95rem;
    }
    th, td {
      text-align: left;
      padding: 10px 8px;
      border-bottom: 1px solid var(--border);
    }
    th { color: var(--muted); font-weight: 600; font-size: 0.8rem; text-transform: uppercase; }
    .badge {
      display: inline-block;
      padding: 2px 8px;
      border-radius: 999px;
      font-size: 0.75rem;
      font-weight: 600;
    }
    .badge.ok { background: rgba(76, 175, 130, 0.15); color: var(--ok); }
    .badge.bad { background: rgba(224, 90, 90, 0.15); color: var(--danger); }
    .badge.warn { background: rgba(212, 160, 23, 0.15); color: var(--warn); }
    button, .btn {
      appearance: none;
      border: 1px solid var(--border);
      background: #222733;
      color: var(--text);
      border-radius: 8px;
      padding: 8px 14px;
      cursor: pointer;
      font: inherit;
    }
    button:hover, .btn:hover { border-color: var(--accent); }
    .btn-danger { border-color: rgba(224, 90, 90, 0.4); color: #ffb4b4; }
    .btn-danger:hover { border-color: var(--danger); }
    .btn-primary { background: var(--accent); border-color: var(--accent); color: #fff; }
    .toolbar { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .muted { color: var(--muted); font-size: 0.9rem; }
    .login-box { max-width: 380px; margin: 80px auto; }
    label { display: block; margin-bottom: 6px; color: var(--muted); font-size: 0.9rem; }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 10px 12px;
      margin-bottom: 14px;
      border-radius: 8px;
      border: 1px solid var(--border);
      background: #0f1117;
      color: var(--text);
      font: inherit;
    }
    .error {
      background: rgba(224, 90, 90, 0.12);
      border: 1px solid rgba(224, 90, 90, 0.35);
      color: #ffb4b4;
      padding: 10px 12px;
      border-radius: 8px;
      margin-bottom: 14px;
    }
    code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 0.85em; }
    .empty { text-align: center; color: var(--muted); padding: 24px 0; }
    .section-title { display: block; margin-bottom: 12px; }
    .grid-2 {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
      gap: 16px;
    }
    .warn-row td { background: rgba(212, 160, 23, 0.08); }
    .bad-row td { background: rgba(224, 90, 90, 0.08); }
  </style>
</head>
<body>
  ${body}
</body>
</html>`;
}

app.get("/", (req, res) => {
  if (getSession(req)) {
    res.redirect("/dashboard");
    return;
  }
  res.redirect("/login");
});

app.get("/login", (req, res) => {
  if (getSession(req)) {
    res.redirect("/dashboard");
    return;
  }
  const error = req.query.error === "1"
    ? `<div class="error">Invalid username or password.</div>`
    : "";
  res.send(
    page(
      "Login",
      `<div class="wrap login-box">
        <div class="card">
          <h1>SF4e Relay</h1>
          <p class="muted">Sign in to manage VPS relay sessions.</p>
          ${error}
          <form method="post" action="/login">
            <label for="username">Username</label>
            <input id="username" name="username" type="text" autocomplete="username" required>
            <label for="password">Password</label>
            <input id="password" name="password" type="password" autocomplete="current-password" required>
            <button class="btn btn-primary" type="submit">Sign in</button>
          </form>
        </div>
      </div>`
    )
  );
});

app.post("/login", async (req, res) => {
  const ip = clientIp(req);
  if (isLoginRateLimited(ip)) {
    res.redirect("/login?error=1");
    return;
  }

  const username = String(req.body.username || "").trim();
  const password = String(req.body.password || "");

  if (username !== ADMIN_USERNAME) {
    recordLoginFailure(ip);
    res.redirect("/login?error=1");
    return;
  }

  const ok = await bcrypt.compare(password, ADMIN_PASSWORD_HASH);
  if (!ok) {
    recordLoginFailure(ip);
    res.redirect("/login?error=1");
    return;
  }

  clearLoginFailures(ip);
  const { token } = createSession(username);
  setSessionCookie(res, token);
  res.redirect("/dashboard");
});

app.post("/logout", requireAuth, (req, res) => {
  sessions.delete(req.session.token);
  clearSessionCookie(res);
  res.redirect("/login");
});

app.get("/dashboard", requireAuth, (_req, res) => {
  res.send(
    page(
      "Dashboard",
      `<div class="wrap">
        <header>
          <div>
            <h1>SF4e Relay Dashboard</h1>
            <p class="muted">VPS session relay manager</p>
          </div>
          <form method="post" action="/logout">
            <button type="submit">Logout</button>
          </form>
        </header>

        <div class="card">
          <div class="toolbar" style="margin-bottom: 16px;">
            <strong>Overview</strong>
            <span class="muted" id="last-updated">Loading…</span>
            <button type="button" id="refresh-btn">Refresh</button>
          </div>
          <div class="stats">
            <div class="stat"><label>Broker</label><strong id="stat-broker">—</strong></div>
            <div class="stat"><label>Relay Manager</label><strong id="stat-manager">—</strong></div>
            <div class="stat"><label>Active Rooms</label><strong id="stat-rooms">—</strong></div>
            <div class="stat"><label>Relay Processes</label><strong id="stat-sessions">—</strong></div>
            <div class="stat"><label>Port Pool</label><strong id="stat-ports">—</strong></div>
            <div class="stat"><label>Matchmaking Queue</label><strong id="stat-queue">—</strong></div>
            <div class="stat"><label>Relay Host</label><strong id="stat-relay-host">—</strong></div>
            <div class="stat"><label>VPS Memory</label><strong id="stat-memory">—</strong></div>
            <div class="stat"><label>System Uptime</label><strong id="stat-uptime">—</strong></div>
            <div class="stat"><label>Load Avg</label><strong id="stat-load">—</strong></div>
            <div class="stat"><label>Dashboard Uptime</label><strong id="stat-dash-uptime">—</strong></div>
            <div class="stat"><label>Node.js</label><strong id="stat-node">—</strong></div>
          </div>
        </div>

        <div class="grid-2">
          <div class="card">
            <strong class="section-title">Broker Rooms</strong>
            <div style="overflow-x:auto;">
              <table>
                <thead>
                  <tr>
                    <th>Code</th>
                    <th>Host</th>
                    <th>Port</th>
                    <th>Created</th>
                    <th>Last Seen</th>
                    <th>Expires In</th>
                    <th>Relay</th>
                  </tr>
                </thead>
                <tbody id="rooms-body">
                  <tr><td colspan="7" class="empty">Loading rooms…</td></tr>
                </tbody>
              </table>
            </div>
          </div>

          <div class="card">
            <strong class="section-title">Service Details</strong>
            <div class="stats">
              <div class="stat"><label>Broker URL</label><strong id="detail-broker-url" style="font-size:0.85rem;word-break:break-all;">—</strong></div>
              <div class="stat"><label>Manager URL</label><strong id="detail-manager-url" style="font-size:0.85rem;word-break:break-all;">—</strong></div>
              <div class="stat"><label>Port Range</label><strong id="detail-port-range">—</strong></div>
              <div class="stat"><label>Room Idle Timeout</label><strong id="detail-room-idle">—</strong></div>
              <div class="stat"><label>Manager Identity</label><strong id="detail-identity">—</strong></div>
              <div class="stat"><label>Relay Binary</label><strong id="detail-relay-bin" style="font-size:0.85rem;word-break:break-all;">—</strong></div>
              <div class="stat"><label>Hostname</label><strong id="detail-hostname">—</strong></div>
              <div class="stat"><label>VPS Relay Mode</label><strong id="detail-vps-mode">—</strong></div>
            </div>
          </div>
        </div>

        <div class="card">
          <strong class="section-title">Match Coordinator</strong>
          <div style="overflow-x:auto;">
            <table>
              <thead>
                <tr>
                  <th>Room</th>
                  <th>Match ID</th>
                  <th>Transport</th>
                  <th>Session</th>
                  <th>GGPO UDP</th>
                  <th>Started</th>
                  <th>Last Event</th>
                </tr>
              </thead>
              <tbody id="matches-body">
                <tr><td colspan="7" class="empty">Loading matches…</td></tr>
              </tbody>
            </table>
          </div>
        </div>

        <div class="card">
          <strong class="section-title">Relay Processes</strong>
          <div style="overflow-x:auto;">
            <table>
              <thead>
                <tr>
                  <th>Port</th>
                  <th>Room</th>
                  <th>PID</th>
                  <th>Sidecar Hash</th>
                  <th>Uptime</th>
                  <th>Running</th>
                  <th>Listening</th>
                  <th>Status</th>
                  <th></th>
                </tr>
              </thead>
              <tbody id="sessions-body">
                <tr><td colspan="9" class="empty">Loading sessions…</td></tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
      <script>
        function badge(ok, okText, badText, warn) {
          const cls = warn ? "warn" : (ok ? "ok" : "bad");
          const text = warn ? warn : (ok ? okText : badText);
          return '<span class="badge ' + cls + '">' + text + '</span>';
        }

        function escapeHtml(value) {
          return String(value)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");
        }

        function setText(id, value) {
          const el = document.getElementById(id);
          if (el) el.textContent = value;
        }

        async function killSession(port) {
          if (!confirm("Stop relay on port " + port + "?")) {
            return;
          }
          const res = await fetch("/api/sessions/" + port, { method: "DELETE" });
          if (!res.ok) {
            alert("Failed to stop session on port " + port);
            return;
          }
          await loadStatus();
        }

        function renderRooms(rooms) {
          const body = document.getElementById("rooms-body");
          if (!rooms.length) {
            body.innerHTML = '<tr><td colspan="7" class="empty">No active broker rooms.</td></tr>';
            return;
          }
          body.innerHTML = rooms.map(function (r) {
            const rowClass = r.relayOk ? "" : (r.relayRunning ? "warn-row" : "bad-row");
            return '<tr class="' + rowClass + '">' +
              '<td><code>' + escapeHtml(r.code) + '</code></td>' +
              '<td>' + escapeHtml(r.displayName || "—") + '</td>' +
              '<td><code>' + escapeHtml(r.port) + '</code></td>' +
              '<td>' + escapeHtml(r.age) + '</td>' +
              '<td>' + escapeHtml(r.idleFor) + '</td>' +
              '<td>' + escapeHtml(r.expiresIn) + '</td>' +
              '<td>' + badge(r.relayOk, "Healthy", "Down", r.relayRunning && !r.relayListening ? "No UDP" : null) + '</td>' +
            '</tr>';
          }).join("");
        }

        function renderMatches(matches) {
          const body = document.getElementById("matches-body");
          if (!matches.length) {
            body.innerHTML = '<tr><td colspan="7" class="empty">No active matches.</td></tr>';
            return;
          }
          body.innerHTML = matches.map(function (m) {
            const events = Array.isArray(m.events) ? m.events : [];
            const lastEvent = events.length ? events[events.length - 1].type : "—";
            const transport = m.transportActive || m.ggpoTransport || "pending";
            return '<tr>' +
              '<td><code>' + escapeHtml(m.code || "—") + '</code></td>' +
              '<td><code title="' + escapeHtml(m.matchId || "") + '">' + escapeHtml((m.matchId || "—").slice(0, 8)) + '</code></td>' +
              '<td>' + escapeHtml(transport) + '</td>' +
              '<td><code>' + escapeHtml(m.port || "—") + '</code></td>' +
              '<td><code>' + escapeHtml(m.ggpoPort || "—") + '</code></td>' +
              '<td>' + (m.startedAt ? escapeHtml(new Date(m.startedAt).toLocaleTimeString()) : '<span class="muted">—</span>') + '</td>' +
              '<td>' + escapeHtml(lastEvent) + '</td>' +
            '</tr>';
          }).join("");
        }

        function renderSessions(sessions) {
          const body = document.getElementById("sessions-body");
          if (!sessions.length) {
            body.innerHTML = '<tr><td colspan="9" class="empty">No active relay processes.</td></tr>';
            return;
          }
          body.innerHTML = sessions.map(function (s) {
            const rowClass = s.ok ? "" : (s.running ? "warn-row" : "bad-row");
            const status = s.ok ? "Healthy" : (s.running ? "Not listening" : "Stopped");
            return '<tr class="' + rowClass + '">' +
              '<td><code>' + escapeHtml(s.port) + '</code></td>' +
              '<td>' + (s.roomCode ? '<code>' + escapeHtml(s.roomCode) + '</code>' : '<span class="muted">—</span>') + '</td>' +
              '<td>' + escapeHtml(s.pid ?? "—") + '</td>' +
              '<td><code title="' + escapeHtml(s.sidecarHash) + '">' + escapeHtml(s.sidecarHashShort) + '</code></td>' +
              '<td>' + escapeHtml(s.uptime) + '</td>' +
              '<td>' + badge(s.running, "Yes", "No") + '</td>' +
              '<td>' + badge(s.listening, "Yes", "No") + '</td>' +
              '<td>' + badge(s.ok, "Healthy", status, !s.ok && s.running ? "Degraded" : null) + '</td>' +
              '<td><button class="btn-danger" type="button" data-port="' + escapeHtml(s.port) + '">Kill</button></td>' +
            '</tr>';
          }).join("");

          body.querySelectorAll("button[data-port]").forEach(function (btn) {
            btn.addEventListener("click", function () {
              killSession(btn.getAttribute("data-port"));
            });
          });
        }

        async function loadStatus() {
          const res = await fetch("/api/status");
          if (res.status === 401) {
            window.location.href = "/login";
            return;
          }
          if (!res.ok) {
            document.getElementById("last-updated").textContent = "Failed to load status";
            return;
          }
          const data = await res.json();

          document.getElementById("stat-broker").innerHTML = badge(data.broker.ok, "Healthy", "Unhealthy");
          document.getElementById("stat-manager").innerHTML = badge(data.manager.ok, "Healthy", "Unhealthy");
          setText("stat-rooms", String(data.broker.rooms ?? 0) + " / " + String(data.broker.maxRooms ?? "?"));
          setText("stat-sessions", String(data.manager.sessions ?? 0));
          setText("stat-ports", String(data.broker.usedPorts ?? 0) + " / " + String(data.broker.maxRooms ?? "?") + " used");
          setText("stat-queue", String(data.broker.queueSize ?? 0));
          setText("stat-relay-host", data.broker.relayHost || "—");
          setText("stat-memory", (data.system.memoryUsedPct ?? "?") + "% (" + (data.system.memoryUsedMb ?? "?") + " / " + (data.system.memoryTotalMb ?? "?") + " MB)");
          setText("stat-uptime", data.system.uptime || "—");
          setText("stat-load", Array.isArray(data.system.loadAvg) ? data.system.loadAvg.join(", ") : "—");
          setText("stat-dash-uptime", data.system.dashboardUptime || "—");
          setText("stat-node", data.system.nodeVersion || "—");

          setText("detail-broker-url", data.meta.brokerUrl || "—");
          setText("detail-manager-url", data.meta.relayManagerUrl || "—");
          setText("detail-port-range", data.broker.portRange || "—");
          setText("detail-room-idle", data.broker.roomIdleMs ? Math.round(data.broker.roomIdleMs / 60000) + " min" : "—");
          setText("detail-identity", data.manager.identity || "—");
          setText("detail-relay-bin", data.manager.relayBin || "—");
          setText("detail-hostname", data.system.hostname || "—");
          document.getElementById("detail-vps-mode").innerHTML = badge(data.broker.forceVpsRelay, "Enabled", "Disabled");

          document.getElementById("last-updated").textContent =
            "Updated " + new Date().toLocaleTimeString();
          renderRooms(data.rooms || []);
          renderMatches(data.matches || []);
          renderSessions(data.sessions || []);
          if (Array.isArray(data.ggpoSessions) && data.ggpoSessions.length) {
            setText("stat-sessions", String(data.manager.sessions ?? 0) + " session / " + String(data.ggpoSessions.length) + " ggpo");
          }
        }

        document.getElementById("refresh-btn").addEventListener("click", loadStatus);
        loadStatus();
        setInterval(loadStatus, 10000);
      </script>`
    )
  );
});

app.get("/api/status", requireAuth, async (_req, res) => {
  try {
    const [healthRes, sessionsRes, ggpoSessionsRes, brokerHealthRes, brokerRoomsRes, brokerMatchesRes] =
      await Promise.all([
        relayFetch("/v1/health"),
        relayFetch("/v1/sessions"),
        relayFetch("/v1/ggpo-sessions"),
        brokerFetch("/v1/health"),
        brokerFetch("/v1/rooms"),
        brokerFetch("/v1/matches"),
      ]);

    if (healthRes.status >= 500) {
      res.status(503).json({
        error: "relay_manager_unavailable",
        message: "Could not reach relay manager",
      });
      return;
    }

    const manager = healthRes.body || {};
    const broker = brokerHealthRes.body || {};
    const rawSessions = Array.isArray(sessionsRes.body?.sessions)
      ? sessionsRes.body.sessions
      : [];
    const rawGgpoSessions = Array.isArray(ggpoSessionsRes.body?.sessions)
      ? ggpoSessionsRes.body.sessions
      : [];
    const rawRooms = Array.isArray(brokerRoomsRes.body?.rooms)
      ? brokerRoomsRes.body.rooms
      : [];
    const rawMatches = Array.isArray(brokerMatchesRes.body?.matches)
      ? brokerMatchesRes.body.matches
      : rawRooms;
    const now = Date.now();
    const roomIdleMs = broker.roomIdleMs || 15 * 60 * 1000;
    const maxRooms = broker.maxRooms || 20;
    const relayPortBase = broker.relayPortBase || 23456;
    const relayPortEnd = relayPortBase + maxRooms - 1;

    const ggpoSessionsWithHealth = await Promise.all(
      rawGgpoSessions.map(async (session) => {
        const health = await relayFetch(`/v1/ggpo-sessions/${session.port}/health`);
        const h = health.body || {};
        return {
          port: session.port,
          sessionPort: session.sessionPort,
          pid: session.pid,
          startedAt: session.startedAt,
          uptime: formatUptime(session.startedAt),
          running: Boolean(h.running),
          listening: Boolean(h.listening),
          ok: Boolean(h.ok),
        };
      })
    );

    const sessionsWithHealth = await Promise.all(
      rawSessions.map(async (session) => {
        const health = await relayFetch(`/v1/sessions/${session.port}/health`);
        const h = health.body || {};
        return {
          port: session.port,
          pid: session.pid,
          sidecarHash: session.sidecarHash,
          sidecarHashShort: truncateHash(session.sidecarHash),
          startedAt: session.startedAt,
          uptime: formatUptime(session.startedAt),
          running: Boolean(h.running),
          listening: Boolean(h.listening),
          ok: Boolean(h.ok),
        };
      })
    );

    const sessionByPort = new Map(
      sessionsWithHealth.map((session) => [session.port, session])
    );

    const rooms = rawRooms.map((room) => {
      const session = sessionByPort.get(room.port);
      const ageMs = room.createdAt ? now - room.createdAt : 0;
      const idleMs = room.lastSeenAt ? now - room.lastSeenAt : 0;
      const expiresInMs = Math.max(0, roomIdleMs - idleMs);
      return {
        code: room.code,
        displayName: room.displayName,
        port: room.port,
        ggpoPort: room.ggpoPort || 0,
        matchId: room.matchId || null,
        transportActive: room.transportActive || null,
        ggpoTransport: room.ggpoTransport || null,
        host: room.host,
        age: formatDuration(ageMs),
        idleFor: formatDuration(idleMs),
        expiresIn: formatDuration(expiresInMs),
        relayOk: Boolean(session?.ok),
        relayRunning: Boolean(session?.running),
        relayListening: Boolean(session?.listening),
      };
    });

    const matches = rawMatches.map((match) => {
      const room = rawRooms.find((r) => r.code === match.code) || match;
      return {
        code: match.code || room.code,
        matchId: match.matchId || room.matchId,
        transportActive: match.transportActive || room.transportActive,
        ggpoTransport: match.ggpoTransport || room.ggpoTransport,
        port: match.port || room.port,
        ggpoPort: match.ggpoPort || room.ggpoPort,
        startedAt: match.startedAt || room.startedAt,
        events: match.events || [],
      };
    });

    const sessions = sessionsWithHealth.map((session) => {
      const room = rawRooms.find((r) => r.port === session.port);
      return {
        ...session,
        roomCode: room?.code || null,
        displayName: room?.displayName || null,
      };
    });

    res.json({
      ok: true,
      meta: {
        brokerUrl: BROKER_URL,
        relayManagerUrl: RELAY_MANAGER_URL,
      },
      system: getSystemStats(),
      broker: {
        ok: brokerHealthRes.status < 500 && broker.ok !== false,
        rooms: broker.rooms ?? rawRooms.length,
        maxRooms,
        usedPorts: rawSessions.length,
        availablePorts: Math.max(0, maxRooms - rawSessions.length),
        relayHost: broker.relayHost || null,
        forceVpsRelay: Boolean(broker.forceVpsRelay),
        queueSize: broker.queueSize ?? 0,
        relayPortBase,
        relayPortEnd,
        portRange: `${relayPortBase}–${relayPortEnd}`,
        roomIdleMs,
      },
      manager: {
        ok: Boolean(manager.ok),
        sessions: manager.sessions ?? rawSessions.length,
        ggpoSessions: manager.ggpoSessions ?? rawGgpoSessions.length,
        relayBin: manager.relayBin || null,
        ggpoRelayBin: manager.ggpoRelayBin || null,
        identity: manager.identity || null,
        bind: manager.bind || null,
        port: manager.port || null,
      },
      rooms,
      matches,
      ggpoSessions: ggpoSessionsWithHealth,
      sessions,
    });
  } catch (err) {
    console.error("status error:", err);
    res.status(503).json({
      error: "status_unavailable",
      message: err.message,
    });
  }
});

app.delete("/api/sessions/:port", requireAuth, async (req, res) => {
  const port = parseInt(req.params.port, 10);
  if (!port || port < 1 || port > 65535) {
    res.status(400).json({ error: "invalid_port" });
    return;
  }

  try {
    const result = await relayFetch(`/v1/sessions/${port}`, { method: "DELETE" });
    res.status(result.status).json(result.body);
  } catch (err) {
    console.error("delete session error:", err);
    res.status(503).json({
      error: "relay_manager_unavailable",
      message: err.message,
    });
  }
});

setInterval(() => {
  const now = Date.now();
  for (const [token, session] of sessions.entries()) {
    if (now > session.expiresAt) {
      sessions.delete(token);
    }
  }
}, 60_000);

app.listen(PORT, BIND, () => {
  console.log(
    `SF4e relay dashboard on http://${BIND}:${PORT} -> ${RELAY_MANAGER_URL}`
  );
});
