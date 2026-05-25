(function () {
  const PROTOCOL_VERSION = 1;
  let state = { simpleUi: true, defaultConnectMethod: 0 };

  function post(msg) {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(Object.assign({ v: PROTOCOL_VERSION }, msg));
    }
  }

  function showToast(text, kind) {
    const el = document.getElementById("toast");
    el.textContent = text;
    el.className = "toast " + (kind || "");
    el.classList.remove("hidden");
    clearTimeout(showToast._t);
    showToast._t = setTimeout(function () {
      el.classList.add("hidden");
    }, 4000);
  }

  function setStatus(text, kind) {
    const el = document.getElementById("status-strip");
    if (!text) {
      el.classList.add("hidden");
      return;
    }
    el.textContent = text;
    el.className = "status-strip " + (kind || "");
    el.classList.remove("hidden");
  }

  function showScreen(id) {
    document.querySelectorAll(".screen").forEach(function (s) {
      s.classList.toggle("active", s.id === id);
    });
  }

  function isSimple() {
    const t = document.getElementById("toggle-simple-ui");
    return t ? t.checked : true;
  }

  function renderUiMode() {
    const simple = isSimple();
    document.querySelectorAll(".simple-only").forEach(function (el) {
      el.classList.toggle("hidden", !simple);
    });
    document.querySelectorAll(".advanced-only").forEach(function (el) {
      el.classList.toggle("hidden", simple);
    });
  }

  function applyUiMode(fromSource) {
    renderUiMode();
    if (fromSource === "user") {
      post({ type: "setUiMode", simpleUi: isSimple() });
    }
  }

  function syncDelayFields(delay) {
    ["host-delay", "host-delay-simple", "join-delay", "join-delay-simple"].forEach(function (id) {
      const el = document.getElementById(id);
      if (el) el.value = delay;
    });
  }

  function syncNameFields(name) {
    ["host-name", "host-name-adv", "join-name", "join-name-adv"].forEach(function (id) {
      const el = document.getElementById(id);
      if (el) el.value = name;
    });
  }

  function applyState(s) {
    if (!s || s.type !== "state") return;
    state = s;
    syncNameFields(s.displayName || "Player");
    syncDelayFields(s.inputDelay || 2);

    const hostPort = document.getElementById("host-port");
    if (hostPort) hostPort.value = s.sessionPort || 23456;

    const lanCode = document.getElementById("host-lan-code");
    if (lanCode) lanCode.textContent = s.lanRoomCode || "—";

    const adv = document.getElementById("host-advertise");
    if (adv) adv.value = s.advertiseHost || "";

    const preview = document.getElementById("host-room-preview");
    if (preview) preview.textContent = s.roomCodePreview || "—";

    const joinRoom = document.getElementById("join-room-code");
    const joinAddr = document.getElementById("join-address");
    const last = s.lastJoinHost || "";
    if (joinRoom && last) joinRoom.value = last.indexOf("SF4-") === 0 ? last : last;
    if (joinAddr && last) joinAddr.value = last;

    const broker = document.getElementById("broker-url");
    if (broker && s.brokerBaseUrl) broker.value = s.brokerBaseUrl;

    const toggle = document.getElementById("toggle-simple-ui");
    if (toggle && typeof s.simpleUi === "boolean" && toggle.checked !== s.simpleUi) {
      toggle.checked = s.simpleUi;
    }
    renderUiMode();

    if (s.connectionStatus) setStatus(s.connectionStatus, "success");
    if (s.natStatus) {
      const nat = document.getElementById("host-nat-status");
      if (nat) nat.textContent = s.natStatus + (s.natDetail ? " — " + s.natDetail : "");
    }
  }

  function getConnectMethod(hostOrJoin) {
    if (isSimple()) return "relay";
    const id = hostOrJoin === "host" ? "host-connect-method" : "join-connect-method";
    const el = document.getElementById(id);
    return el ? el.value : "relay";
  }

  function getDisplayName() {
    const id = isSimple() ? (document.getElementById("screen-host").classList.contains("active") ? "host-name" : "join-name") : null;
    if (id) {
      const el = document.getElementById(id);
      if (el && el.value) return el.value;
    }
    const adv = document.getElementById("host-name-adv") || document.getElementById("join-name-adv");
    return adv ? adv.value : "Player";
  }

  function getInputDelay(hostOrJoin) {
    const simpleId = hostOrJoin === "host" ? "host-delay-simple" : "join-delay-simple";
    const advId = hostOrJoin === "host" ? "host-delay" : "join-delay";
    const el = document.getElementById(isSimple() ? simpleId : advId);
    return el ? parseInt(el.value, 10) : 2;
  }

  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener("message", function (ev) {
      let data = ev.data;
      if (typeof data === "string") {
        try {
          data = JSON.parse(data);
        } catch (e) {
          return;
        }
      }
      if (data.type === "state") {
        applyState(data);
      } else if (data.type === "error") {
        showToast(data.message || "Error", "error");
        setStatus(data.message || "Something went wrong", "error");
      } else if (data.type === "copied") {
        showToast("Copied to clipboard", "success");
      } else if (data.rooms) {
        renderRoomList(data.rooms, data.listError);
      } else if (data.roomCodePreview) {
        const preview = document.getElementById("host-room-preview");
        if (preview) preview.textContent = data.roomCodePreview;
        if (data.connectionStatus) setStatus(data.connectionStatus, "success");
      }
    });
  }

  function renderRoomList(rooms, listError) {
    const ul = document.getElementById("room-list");
    const empty = document.getElementById("room-list-empty");
    if (!ul) return;
    ul.innerHTML = "";
    if (listError) {
      showToast(listError, "error");
      empty.classList.remove("hidden");
      empty.textContent = listError;
      return;
    }
    if (!rooms || !rooms.length) {
      empty.classList.remove("hidden");
      empty.textContent = "No open rooms. Ask a friend to host or create one.";
      return;
    }
    empty.classList.add("hidden");
    rooms.forEach(function (room) {
      const li = document.createElement("li");
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "room-list-item";
      btn.textContent = (room.displayName || "Host") + " — " + (room.code || "?");
      btn.addEventListener("click", function () {
        document.getElementById("join-room-code").value = room.code || "";
        document.getElementById("join-address").value = room.code || "";
        showScreen("screen-join");
      });
      li.appendChild(btn);
      ul.appendChild(li);
    });
  }

  document.querySelectorAll(".mode-card").forEach(function (btn) {
    btn.addEventListener("click", function () {
      const mode = btn.getAttribute("data-mode");
      if (mode === "host") showScreen("screen-host");
      else if (mode === "join") showScreen("screen-join");
      else if (mode === "offline") showScreen("screen-offline");
    });
  });

  document.querySelectorAll("[data-back]").forEach(function (btn) {
    btn.addEventListener("click", function () {
      showScreen("screen-home");
      setStatus("");
    });
  });

  document.getElementById("toggle-simple-ui").addEventListener("change", function () {
    applyUiMode("user");
  });

  document.getElementById("btn-refresh-ip").addEventListener("click", function () {
    post({ type: "fetchPublicIp" });
  });

  document.getElementById("btn-try-upnp").addEventListener("click", function () {
    const port = document.getElementById("host-port");
    post({
      type: "tryUpnp",
      sessionPort: port ? parseInt(port.value, 10) : 23456,
    });
  });

  document.getElementById("btn-create-relay-room").addEventListener("click", function () {
    setStatus("Creating relay room…", "");
    post({
      type: "createRelayRoom",
      displayName: getDisplayName(),
    });
  });

  document.getElementById("host-advertise").addEventListener("input", function () {
    const adv = document.getElementById("host-advertise");
    const port = document.getElementById("host-port");
    post({
      type: "previewRoomCode",
      advertiseHost: adv ? adv.value : "",
      sessionPort: port ? parseInt(port.value, 10) : 23456,
    });
  });
  document.getElementById("host-port").addEventListener("input", function () {
    document.getElementById("host-advertise").dispatchEvent(new Event("input"));
  });

  document.getElementById("btn-copy-room").addEventListener("click", function () {
    const text = document.getElementById("host-room-preview").textContent;
    post({ type: "copyText", text: text });
  });

  document.getElementById("btn-start-host").addEventListener("click", function () {
    const broker = document.getElementById("broker-url");
    if (broker && broker.value) {
      post({ type: "saveSettings", brokerBaseUrl: broker.value });
    }
    const method = getConnectMethod("host");
    const preview = document.getElementById("host-room-preview").textContent;
    post({
      type: "start",
      mode: "host",
      connectMethod: method,
      displayName: getDisplayName(),
      inputDelay: getInputDelay("host"),
      sessionPort: parseInt(document.getElementById("host-port").value, 10) || 23456,
      advertiseHost: document.getElementById("host-advertise").value,
      relayRoomCode: preview && preview.indexOf("SF4-") === 0 ? preview : "",
      tryUpnp: method === "autoNat",
    });
  });

  document.getElementById("btn-start-join").addEventListener("click", function () {
    const method = getConnectMethod("join");
    const code = isSimple()
      ? document.getElementById("join-room-code").value
      : document.getElementById("join-address").value;
    post({
      type: "start",
      mode: "join",
      connectMethod: method === "direct" && code.indexOf("SF4-") !== 0 ? "direct" : "relay",
      displayName: getDisplayName(),
      inputDelay: getInputDelay("join"),
      joinAddress: code,
      roomCode: code,
    });
  });

  document.getElementById("btn-start-offline").addEventListener("click", function () {
    post({ type: "start", mode: "offline" });
  });

  document.getElementById("btn-find-match").addEventListener("click", function () {
    setStatus("Searching for an opponent…", "");
    post({
      type: "start",
      mode: "join",
      connectMethod: "matchmaking",
      displayName: getDisplayName(),
      inputDelay: 2,
      joinAddress: "",
    });
  });

  document.getElementById("btn-browse-rooms").addEventListener("click", function () {
    showScreen("screen-rooms");
    post({ type: "listRooms" });
  });

  document.getElementById("btn-refresh-rooms").addEventListener("click", function () {
    post({ type: "listRooms" });
  });

  document.getElementById("broker-url").addEventListener("change", function () {
    const broker = document.getElementById("broker-url");
    if (broker) post({ type: "saveSettings", brokerBaseUrl: broker.value });
  });

  post({ type: "getState" });
})();
