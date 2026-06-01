(function () {
  const PROTOCOL_VERSION = 1;
  let state = {};
  let pollTimer = null;
  let allFriends = [];
  let steamBuild = { sidecarHash: "", buildGit: "", sessionToken: "" };
  let pendingInvite = null;
  let p2pConnected = false;

  function $(id) { return document.getElementById(id); }

  function post(msg) {
    const payload = Object.assign({ v: PROTOCOL_VERSION }, msg);
    if (window.launcherBridge && window.launcherBridge.postMessage) {
      window.launcherBridge.postMessage(payload);
      return;
    }
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(payload);
      return;
    }
    log("Launcher bridge unavailable");
  }

  function setLogCollapsed(collapsed) {
    const panel = $("log-panel");
    const btn = $("btn-toggle-log");
    if (!panel || !btn) return;
    panel.classList.toggle("collapsed", collapsed);
    btn.textContent = collapsed ? "Show log" : "Hide log";
    btn.setAttribute("aria-expanded", collapsed ? "false" : "true");
    try {
      localStorage.setItem("sf4e-log-collapsed", collapsed ? "1" : "0");
    } catch (e) { /* ignore */ }
  }

  function log(text) {
    const el = $("log");
    const line = "[" + new Date().toLocaleTimeString() + "] " + text;
    el.textContent = line + "\n" + el.textContent;
  }

  function value(id) {
    return ($(id) || {}).value || "";
  }

  function numberValue(id, fallback) {
    const n = parseInt(value(id), 10);
    return isNaN(n) ? fallback : n;
  }

  function peerSteamId() {
    return value("peer-steamid").trim();
  }

  function setText(id, text) {
    const el = $(id);
    if (el) el.textContent = text == null || text === "" ? "-" : String(text);
  }

  function setStatus(text, kind) {
    const el = $("status-strip");
    if (!el) return;
    if (!text) {
      el.classList.add("hidden");
      el.textContent = "";
      return;
    }
    el.textContent = text;
    el.className = "status-strip " + (kind || "");
    el.classList.remove("hidden");
  }

  function updateStartButtons() {
    const hostBtn = $("btn-start-host");
    const joinBtn = $("btn-start-join");
    if (hostBtn) hostBtn.disabled = !p2pConnected || !peerSteamId();
    if (joinBtn) joinBtn.disabled = !p2pConnected || !pendingInvite;
  }

  function updateConnectionLines() {
    const label = p2pConnected ? "Connected" : "Not connected";
    setText("host-connection", "Connection: " + label);
    setText("join-connection", "Connection: " + label);
  }

  function applyStatus(s) {
    if (!s) return;
    if (s.connected != null) {
      p2pConnected = !!s.connected;
      updateStartButtons();
      updateConnectionLines();
    }
    setText("steam-init", s.initialized ? "initialized" : "not initialized");
    setText("steam-id", s.steamId || "-");
    setText("persona", s.persona || "-");
    setText("p2p-state", s.connected ? "connected" : (s.failed ? "failed" : "idle/listening"));
    setText("last-event", s.lastError || s.lastEvent || "No events yet.");
    if (s.lastEvent) log(s.lastEvent);
    if (s.lastError) log("ERROR: " + s.lastError);
    processMessages(s.messages);
  }

  function renderInviteCard() {
    const card = $("invite-card");
    const summary = $("invite-summary");
    const acceptBtn = $("btn-accept-invite");
    if (!card || !summary) return;
    if (!pendingInvite) {
      card.classList.add("empty");
      summary.textContent = "Wait for a Steam invite from the host, then click Accept invite.";
      if (acceptBtn) acceptBtn.disabled = true;
      return;
    }
    card.classList.remove("empty");
    summary.textContent =
      "From " + pendingInvite.senderSteamId +
      " port " + pendingInvite.virtualPort +
      " build " + pendingInvite.buildGit;
    if (acceptBtn) acceptBtn.disabled = false;
  }

  function processMessages(messages) {
    if (!messages || !messages.length) return;
    messages.forEach(function (m) {
      if (m.kind !== "invite" || !m.invite) return;
      pendingInvite = {
        senderSteamId: m.invite.senderSteamId || m.fromSteamId,
        virtualPort: m.invite.virtualPort,
        role: m.invite.role,
        sidecarHash: m.invite.sidecarHash,
        buildGit: m.invite.buildGit,
        sessionToken: m.invite.sessionToken
      };
      $("peer-steamid").value = pendingInvite.senderSteamId;
      log("Invite received from " + pendingInvite.senderSteamId);
      renderInviteCard();
      setStatus("Steam invite received — click Accept invite + connect", "info");
    });
  }

  function friendSearchQuery() {
    return value("friend-search").trim().toLowerCase();
  }

  function filterFriends(friends) {
    const onlySf4 = $("only-sf4") && $("only-sf4").checked;
    const query = friendSearchQuery();
    return (friends || []).filter(function (f) {
      if (onlySf4 && !f.inSf4) return false;
      if (!query) return true;
      const name = (f.name || "").toLowerCase();
      const steamId = (f.steamId || "").toLowerCase();
      const status = f.inSf4 ? "usf4" : ("state " + String(f.personaState || ""));
      return name.indexOf(query) >= 0 || steamId.indexOf(query) >= 0 || status.indexOf(query) >= 0;
    });
  }

  function updateFriendCount(shown, total) {
    setText("friend-count", shown + " / " + total + " friends");
  }

  function renderFriends(friends) {
    const el = $("friends");
    const total = (friends || []).length;
    const filtered = filterFriends(friends);
    updateFriendCount(filtered.length, total);
    el.textContent = "";
    if (!friends || !friends.length) {
      el.classList.add("empty");
      el.textContent = "No friends loaded. Click Refresh.";
      return;
    }
    if (!filtered.length) {
      el.classList.add("empty");
      el.textContent = friendSearchQuery() ? "No friends match your search." : "No friends match filters.";
      return;
    }
    el.classList.remove("empty");
    filtered.forEach(function (f) {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "friend-row";
      row.innerHTML = "<strong></strong><span></span><em></em>";
      row.querySelector("strong").textContent = f.name || "Unknown";
      row.querySelector("span").textContent = f.steamId || "";
      row.querySelector("em").textContent = f.inSf4 ? "USF4" : "state " + f.personaState;
      row.addEventListener("click", function () {
        $("peer-steamid").value = f.steamId || "";
        updateStartButtons();
        log("Selected " + (f.name || f.steamId));
      });
      el.appendChild(row);
    });
  }

  function syncNamesFromState(s) {
    const name = s.displayName || "";
    if ($("host-name")) $("host-name").value = name;
    if ($("join-name")) $("join-name").value = name;
    const delay = s.inputDelay != null ? s.inputDelay : 2;
    if ($("host-delay")) $("host-delay").value = delay;
    if ($("join-delay")) $("join-delay").value = delay;
    const ver = s.installedVersion || "unknown";
    setText("version", "build: " + ver);
    const hint = "Both players must use the same build (" + ver + ").";
    document.querySelectorAll(".version-match-hint").forEach(function (el) {
      el.textContent = hint;
    });
  }

  function handleMessage(msg) {
    if (!msg || !msg.type) return;
    if (msg.type === "state") {
      state = Object.assign(state, msg);
      syncNamesFromState(msg);
      post({ type: "steamBuildInfo" });
      post({ type: "steamStatus" });
      post({ type: "steamRefreshFriends", onlySf4: false });
      return;
    }
    if (msg.type === "steamBuildInfo") {
      if (msg.sidecarHash) steamBuild.sidecarHash = msg.sidecarHash;
      if (msg.buildGit) steamBuild.buildGit = msg.buildGit;
      if (msg.sessionToken) steamBuild.sessionToken = msg.sessionToken;
      if (!msg.ok) log("Build info: " + (msg.message || "unavailable"));
      return;
    }
    if (msg.type === "launcherFinished") {
      if (msg.cancelled) {
        log("Launch cancelled.");
      } else {
        log("Launching game...");
      }
      return;
    }
    if (msg.type === "error") {
      log("ERROR: " + (msg.message || "unknown"));
      setStatus(msg.message || "Error", "error");
      return;
    }
    if (msg.type === "steamFriends") {
      applyStatus(msg);
      allFriends = msg.friends || [];
      renderFriends(allFriends);
      return;
    }
    if (
      msg.type === "steamStatus" ||
      msg.type === "steamListen" ||
      msg.type === "steamConnect" ||
      msg.type === "steamClosed" ||
      msg.type === "steamMessages" ||
      msg.type === "steamPrepareHost" ||
      msg.type === "steamPrepareJoin"
    ) {
      applyStatus(msg);
      if (msg.type === "steamPrepareHost") {
        if (msg.ok) {
          setStatus("Host listening — wait for joiner to connect", "success");
          log("Invite sent; listening on port " + numberValue("virtual-port", 7));
        } else {
          setStatus(msg.lastError || "Host prepare failed", "error");
        }
      }
      if (msg.type === "steamPrepareJoin") {
        if (msg.ok) {
          setStatus("Connected to host", "success");
          log("Joined host Steam P2P session");
        } else {
          setStatus(msg.message || msg.lastError || "Join failed", "error");
        }
      }
      if (msg.type === "steamMessages") processMessages(msg.messages);
      return;
    }
  }

  function showTab(tab) {
    $("tab-host").classList.toggle("active", tab === "host");
    $("tab-join").classList.toggle("active", tab === "join");
    $("screen-host").classList.toggle("active", tab === "host");
    $("screen-join").classList.toggle("active", tab === "join");
  }

  function startGame(mode) {
    const isHost = mode === "host";
    post({
      type: "steamStart",
      mode: mode,
      displayName: value(isHost ? "host-name" : "join-name"),
      inputDelay: numberValue(isHost ? "host-delay" : "join-delay", 2),
      virtualPort: numberValue("virtual-port", 7),
      peerSteamId: isHost ? peerSteamId() : (pendingInvite ? pendingInvite.senderSteamId : peerSteamId()),
      sessionToken: isHost ? steamBuild.sessionToken : (pendingInvite ? pendingInvite.sessionToken : ""),
      connectMethod: "steam"
    });
  }

  function bind() {
    $("tab-host").addEventListener("click", function () { showTab("host"); });
    $("tab-join").addEventListener("click", function () { showTab("join"); });
    $("btn-refresh-status").addEventListener("click", function () { post({ type: "steamStatus" }); });
    $("btn-refresh-friends").addEventListener("click", function () {
      post({ type: "steamRefreshFriends", onlySf4: $("only-sf4").checked });
    });
    $("only-sf4").addEventListener("change", function () { renderFriends(allFriends); });
    $("friend-search").addEventListener("input", function () { renderFriends(allFriends); });
    $("btn-clear-search").addEventListener("click", function () {
      $("friend-search").value = "";
      renderFriends(allFriends);
    });
    $("peer-steamid").addEventListener("input", updateStartButtons);
    $("btn-prepare-host").addEventListener("click", function () {
      if (!peerSteamId()) {
        log("Select a friend first.");
        return;
      }
      post({
        type: "steamPrepareHost",
        targetSteamId: peerSteamId(),
        virtualPort: numberValue("virtual-port", 7),
        sessionToken: steamBuild.sessionToken,
        sidecarHash: steamBuild.sidecarHash,
        buildGit: steamBuild.buildGit
      });
    });
    $("btn-accept-invite").addEventListener("click", function () {
      if (!pendingInvite) return;
      post({
        type: "steamPrepareJoin",
        senderSteamId: pendingInvite.senderSteamId,
        virtualPort: pendingInvite.virtualPort,
        role: pendingInvite.role,
        sidecarHash: pendingInvite.sidecarHash,
        buildGit: pendingInvite.buildGit,
        sessionToken: pendingInvite.sessionToken,
        inviteVersion: 1
      });
    });
    $("btn-start-host").addEventListener("click", function () { startGame("host"); });
    $("btn-start-join").addEventListener("click", function () { startGame("join"); });
    $("btn-clear-log").addEventListener("click", function () { $("log").textContent = ""; });
    $("btn-toggle-log").addEventListener("click", function () {
      const panel = $("log-panel");
      setLogCollapsed(!panel.classList.contains("collapsed"));
    });
  }

  function onBridgeMessage(data) {
    handleMessage(data);
  }

  if (window.launcherBridge && window.launcherBridge.onMessage) {
    window.launcherBridge.onMessage(onBridgeMessage);
  } else if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener("message", function (event) {
      onBridgeMessage(event.data);
    });
  }

  bind();
  renderInviteCard();
  updateStartButtons();
  try {
    setLogCollapsed(localStorage.getItem("sf4e-log-collapsed") !== "0");
  } catch (e) {
    setLogCollapsed(true);
  }
  $("app").classList.remove("is-loading");
  post({ type: "getState" });
  pollTimer = setInterval(function () { post({ type: "steamPollMessages" }); }, 1000);
  window.addEventListener("beforeunload", function () {
    clearInterval(pollTimer);
    post({ type: "steamClose" });
  });
})();
