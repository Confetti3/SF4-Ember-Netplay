(function () {
  const PROTOCOL_VERSION = 1;
  const EPHEMERAL_KEYS = [
    "connectionStatus",
    "heartbeatOk",
    "rooms",
    "listError",
    "natStatus",
    "natDetail",
    "natOk",
    "type",
    "v",
  ];
  let state = { simpleUi: false, defaultConnectMethod: 0 };
  let updateInfo = { zipDownloadUrl: "", zipApiUrl: "", latestVersion: "", updateAvailable: false };
  let updateBusy = false;
  let shareValues = { relay: "", lan: "", wan: "" };
  let relayHeartbeatTimer = null;
  let stateReady = false;

  function splitPersistentState(payload) {
    const persistable = Object.assign({}, payload);
    EPHEMERAL_KEYS.forEach(function (key) {
      delete persistable[key];
    });
    return persistable;
  }

  function finishInitialLoad() {
    if (stateReady) return;
    stateReady = true;
    const app = document.getElementById("app");
    if (app) app.classList.remove("is-loading");
    renderUiMode();
  }

  function post(msg) {
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(Object.assign({ v: PROTOCOL_VERSION }, msg));
    }
  }

  function statusIcon(kind) {
    if (kind === "success") return "check";
    if (kind === "error") return "alert";
    return "info";
  }

  function toastIcon(kind) {
    if (kind === "success") return "check";
    if (kind === "error") return "alert";
    return null;
  }

  function setMessageContent(el, text, iconName) {
    el.textContent = "";
    if (!text) return;
    if (window.SF4eIcons && iconName) {
      el.appendChild(window.SF4eIcons.message(text, iconName));
    } else {
      el.textContent = text;
    }
  }

  function showToast(text, kind) {
    const el = document.getElementById("toast");
    setMessageContent(el, text, toastIcon(kind));
    el.className = "toast " + (kind || "");
    el.classList.remove("hidden");
    clearTimeout(showToast._t);
    showToast._t = setTimeout(function () {
      el.classList.add("hidden");
    }, 3500);
  }

  function setStatus(text, kind) {
    const el = document.getElementById("status-strip");
    if (!text) {
      el.classList.add("hidden");
      el.textContent = "";
      return;
    }
    setMessageContent(el, text, statusIcon(kind));
    el.className = "status-strip " + (kind || "");
    el.classList.remove("hidden");
  }

  function setButtonLoading(id, loading) {
    const btn = document.getElementById(id);
    if (!btn) return;
    btn.classList.toggle("btn-loading", loading);
    btn.disabled = loading;
  }

  function showScreen(id) {
    document.querySelectorAll(".screen").forEach(function (s) {
      s.classList.toggle("active", s.id === id);
    });
    syncRelayHeartbeat();
  }

  function isSimple() {
    return !!state.simpleUi;
  }

  function renderUiMode() {
    const simple = isSimple();
    document.querySelectorAll(".simple-only").forEach(function (el) {
      el.classList.toggle("hidden", !simple);
    });
    document.querySelectorAll(".advanced-only").forEach(function (el) {
      el.classList.toggle("hidden", simple);
    });
    document.getElementById("btn-mode-simple").classList.toggle("active", simple);
    document.getElementById("btn-mode-advanced").classList.toggle("active", !simple);
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

  function isShortRoomCode(value) {
    return value && value.indexOf("SF4-") === 0;
  }

  function formatAddress(host, port) {
    if (!host || !String(host).trim()) return "";
    return String(host).trim() + ":" + (port || 23456);
  }

  function setShareCard(id, value, empty) {
    const card = document.querySelector('.share-card[data-share-id="' + id + '"]');
    const valueEl = document.getElementById("share-" + id + "-value");
    const copyBtn = card ? card.querySelector(".share-copy-btn") : null;
    if (!card || !valueEl) return;

    shareValues[id] = value || "";
    valueEl.textContent = value || "—";
    card.dataset.copy = value || "";
    card.classList.toggle("share-card--empty", !!empty || !value);
    if (copyBtn) copyBtn.disabled = !value;
  }

  function flashShareCard(id) {
    const card = document.querySelector('.share-card[data-share-id="' + id + '"]');
    if (!card) return;
    const copyBtn = card.querySelector(".share-copy-btn");
    const iconUse = copyBtn ? copyBtn.querySelector("use") : null;
    card.classList.add("share-card--copied");
    if (iconUse) iconUse.setAttribute("href", "#i-check");
    clearTimeout(flashShareCard._t);
    flashShareCard._t = setTimeout(function () {
      card.classList.remove("share-card--copied");
      if (iconUse) iconUse.setAttribute("href", "#i-copy");
    }, 1200);
  }

  function copyShareValue(id, label) {
    const value = shareValues[id];
    if (!value) return;
    post({ type: "copyText", text: value });
    flashShareCard(id);
  }

  function renderHostShareCards(s) {
    if (!s) s = state;
    const port = s.sessionPort || 23456;
    const preview = s.roomCodePreview || "";
    const hiddenPreview = document.getElementById("host-room-preview");
    if (hiddenPreview) hiddenPreview.textContent = preview;

    let relayCode = "";
    if (isShortRoomCode(preview)) {
      relayCode = preview;
    } else if (s.relayRoomCode && isShortRoomCode(s.relayRoomCode)) {
      relayCode = s.relayRoomCode;
    }
    setShareCard("relay", relayCode, !relayCode);

    const lanAddr = s.lanAddress || s.lanRoomCode || formatAddress(s.lanIp, port);
    setShareCard("lan", lanAddr, !lanAddr);

    const advertiseHost = (document.getElementById("host-advertise") || {}).value || s.advertiseHost || "";
    const wanAddr = s.wanAddress || formatAddress(advertiseHost, port);
    const wanEmpty = !wanAddr || wanAddr.indexOf("(enter") === 0;
    setShareCard("wan", wanEmpty ? "" : wanAddr, wanEmpty);

    const hint = document.getElementById("host-share-hint");
    if (hint) {
      const relayPort = s.relayPort || s.relaySessionPort;
      if (relayCode) {
        hint.textContent = relayPort
          ? "Share the relay code with your opponent. Forward TCP+UDP port " + relayPort + " on your router if joiners cannot connect."
          : "Share the relay code for WAN play, or LAN address for same-network players.";
      } else if (!wanEmpty) {
        hint.textContent = "Share relay code (recommended) or public/LAN address for direct connect.";
      } else {
        hint.textContent = "Create a relay room or refresh public IP to get shareable addresses.";
      }
    }
  }

  function hasRelayRoomFields(data) {
    return (
      data &&
      (data.roomCodePreview !== undefined ||
        data.relayPort !== undefined ||
        data.relayHost !== undefined)
    );
  }

  function finalizeRelayRoomResponse(data) {
    if (!data) return;
    if (data.roomCodePreview !== undefined) {
      state.roomCodePreview = data.roomCodePreview;
    }
    if (data.relayPort !== undefined) {
      state.relayPort = data.relayPort;
    }
    if (data.relayHost !== undefined) {
      state.relayHost = data.relayHost;
    }
    const hiddenPreview = document.getElementById("host-room-preview");
    if (hiddenPreview && data.roomCodePreview !== undefined) {
      hiddenPreview.textContent = data.roomCodePreview;
    }
    if ("connectionStatus" in data) {
      setStatus(data.connectionStatus, "success");
    }
    renderHostShareCards(state);
    syncRelayHeartbeat();
    setButtonLoading("btn-create-relay-room", false);
  }

  function applyPartialState(data) {
    if (data.advertiseHost !== undefined) {
      state.advertiseHost = data.advertiseHost;
      const adv = document.getElementById("host-advertise");
      if (adv) adv.value = data.advertiseHost;
    }
    if ("natStatus" in data) {
      const nat = document.getElementById("host-nat-status");
      if (nat) nat.textContent = data.natStatus + (data.natDetail ? " — " + data.natDetail : "");
    }
    if (hasRelayRoomFields(data)) {
      finalizeRelayRoomResponse(data);
    } else {
      renderHostShareCards(state);
    }
    setButtonLoading("btn-refresh-ip", false);
  }

  function stopRelayHeartbeat() {
    if (relayHeartbeatTimer) {
      clearInterval(relayHeartbeatTimer);
      relayHeartbeatTimer = null;
    }
  }

  function startRelayHeartbeat() {
    stopRelayHeartbeat();
    relayHeartbeatTimer = setInterval(function () {
      const preview = document.getElementById("host-room-preview");
      const code = preview ? preview.textContent : "";
      if (!isShortRoomCode(code)) return;
      if (!document.getElementById("screen-host").classList.contains("active")) return;
      post({ type: "relayHeartbeat", roomCode: code });
    }, 60000);
  }

  function syncRelayHeartbeat() {
    const hostActive = document.getElementById("screen-host").classList.contains("active");
    const preview = document.getElementById("host-room-preview");
    const code = preview ? preview.textContent : state.roomCodePreview || "";
    if (hostActive && isShortRoomCode(code)) {
      startRelayHeartbeat();
    } else {
      stopRelayHeartbeat();
    }
  }

  function applyState(s) {
    if (!s || s.type !== "state") return;
    state = Object.assign({}, state, splitPersistentState(s));
    syncNameFields(state.displayName || "Player");
    syncDelayFields(state.inputDelay || 2);

    const hostPort = document.getElementById("host-port");
    if (hostPort && s.sessionPort !== undefined) hostPort.value = s.sessionPort;

    const adv = document.getElementById("host-advertise");
    if (adv && s.advertiseHost !== undefined) adv.value = s.advertiseHost;

    const joinRoom = document.getElementById("join-room-code");
    const joinAddr = document.getElementById("join-address");
    if (s.lastJoinHost) {
      if (joinRoom) joinRoom.value = s.lastJoinHost;
      if (joinAddr) joinAddr.value = s.lastJoinHost;
    }

    const broker = document.getElementById("broker-url");
    if (broker && s.brokerBaseUrl) broker.value = s.brokerBaseUrl;

    if (stateReady) {
      renderUiMode();
    }

    const hostMethod = document.getElementById("host-connect-method");
    const joinMethod = document.getElementById("join-connect-method");
    const methodValue = connectMethodFromDefault(state.defaultConnectMethod);
    if (hostMethod) hostMethod.value = methodValue;
    if (joinMethod) joinMethod.value = methodValue;

    if ("connectionStatus" in s && !hasRelayRoomFields(s)) setStatus(s.connectionStatus, "success");
    if ("natStatus" in s) {
      const nat = document.getElementById("host-nat-status");
      if (nat) nat.textContent = s.natStatus + (s.natDetail ? " — " + s.natDetail : "");
    }

    const installedEl = document.getElementById("installed-version");
    if (installedEl) {
      const ver = state.installedVersion || "unknown";
      const text = ver.indexOf("v") === 0 ? ver : "v" + ver;
      const textEl = installedEl.querySelector(".version-badge-text");
      if (textEl) textEl.textContent = text;
    }

    if (hasRelayRoomFields(s)) {
      finalizeRelayRoomResponse(s);
    } else {
      renderHostShareCards(state);
      syncRelayHeartbeat();
    }
    finishInitialLoad();
  }

  function renderUpdateStatus(text, kind) {
    const el = document.getElementById("update-status");
    if (!el) return;
    if (!text) {
      el.classList.add("hidden");
      el.textContent = "";
      return;
    }
    el.textContent = text;
    el.className = "update-status " + (kind || "");
    el.classList.remove("hidden");
  }

  function setUpdateBusy(busy) {
    updateBusy = busy;
    const checkBtn = document.getElementById("btn-check-update");
    const installBtn = document.getElementById("btn-install-update");
    if (checkBtn) checkBtn.disabled = busy;
    if (installBtn) installBtn.disabled = busy;
  }

  function showInstallButton(show) {
    const installBtn = document.getElementById("btn-install-update");
    if (installBtn) installBtn.classList.toggle("hidden", !show);
  }

  function connectMethodFromDefault(method) {
    if (method === 0) return "relay";
    if (method === 2) return "autoNat";
    return "direct";
  }

  function getConnectMethod(hostOrJoin) {
    if (isSimple()) return connectMethodFromDefault(state.defaultConnectMethod);
    const id = hostOrJoin === "host" ? "host-connect-method" : "join-connect-method";
    const el = document.getElementById(id);
    return el ? el.value : connectMethodFromDefault(state.defaultConnectMethod);
  }

  function getHostConnectMethod() {
    const preview = document.getElementById("host-room-preview");
    const previewCode = preview ? preview.textContent : "";
    if (isSimple() || isShortRoomCode(previewCode)) {
      return "relay";
    }
    return getConnectMethod("host");
  }

  function getDisplayName() {
    if (isSimple()) {
      const hostActive = document.getElementById("screen-host").classList.contains("active");
      const id = hostActive ? "host-name" : "join-name";
      const el = document.getElementById(id);
      if (el && el.value) return el.value;
    }
    const adv = document.getElementById("host-name-adv") || document.getElementById("join-name-adv");
    return adv && adv.value ? adv.value : "Player";
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
      if (data.type === "error") {
        setButtonLoading("btn-create-relay-room", false);
        if (updateBusy) {
          setUpdateBusy(false);
          renderUpdateStatus(data.message || "Update failed", "error");
          showInstallButton(!!updateInfo.updateAvailable);
        } else {
          showToast(data.message || "Error", "error");
          setStatus(data.message || "Something went wrong", "error");
        }
      } else if (data.type === "copied") {
        showToast("Copied to clipboard", "success");
      } else if (data.type === "state" && "heartbeatOk" in data) {
        return;
      } else if (data.rooms || data.listError) {
        if (data.type === "state") {
          applyState(data);
        }
        renderRoomList(data.rooms, data.listError);
      } else if (data.roomCodePreview !== undefined || data.relayPort !== undefined || data.relayHost !== undefined) {
        if (data.type === "state") {
          applyState(data);
        } else {
          applyPartialState(data);
        }
      } else if (data.advertiseHost !== undefined) {
        applyPartialState(data);
      } else if (data.type === "state") {
        applyState(data);
      } else if (data.type === "updateCheck") {
        setUpdateBusy(false);
        setButtonLoading("btn-check-update", false);
        if (!data.ok) {
          renderUpdateStatus(data.error || "Update check failed", "error");
          showInstallButton(false);
          showToast(data.error || "Update check failed", "error");
          return;
        }
        updateInfo = {
          zipDownloadUrl: data.zipDownloadUrl || "",
          zipApiUrl: data.zipApiUrl || "",
          latestVersion: data.latestVersion || "",
          updateAvailable: !!data.updateAvailable,
        };
        if (data.updateAvailable) {
          const notes = data.releaseNotes ? "\n\n" + data.releaseNotes : "";
          renderUpdateStatus("Update available: " + data.latestVersion + notes, "success");
          showInstallButton(true);
        } else {
          renderUpdateStatus("Up to date (" + (data.latestVersion || data.installedVersion || "") + ")", "muted");
          showInstallButton(false);
        }
      } else if (data.type === "updateApply") {
        setUpdateBusy(true);
        renderUpdateStatus(data.message || "Installing update…", "success");
        showInstallButton(false);
        showToast(data.message || "Installing update…", "success");
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
      empty.querySelector("p").textContent = listError;
      return;
    }
    if (!rooms || !rooms.length) {
      empty.classList.remove("hidden");
      empty.querySelector("p").textContent = "No open rooms. Ask a friend to host or create one.";
      return;
    }
    empty.classList.add("hidden");
    rooms.forEach(function (room) {
      const li = document.createElement("li");
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "room-list-item";

      const main = document.createElement("span");
      main.className = "room-list-main";

      const avatar = document.createElement("span");
      avatar.className = "room-list-avatar";
      avatar.setAttribute("aria-hidden", "true");
      avatar.appendChild(window.SF4eIcons.create("user"));

      const meta = document.createElement("span");
      meta.className = "room-list-meta";

      const nameSpan = document.createElement("span");
      nameSpan.className = "room-list-name";
      nameSpan.textContent = room.displayName || "Host";
      const codeSpan = document.createElement("span");
      codeSpan.className = "room-list-code";
      codeSpan.textContent = room.code || "?";

      meta.appendChild(nameSpan);
      meta.appendChild(codeSpan);
      main.appendChild(avatar);
      main.appendChild(meta);

      const arrow = document.createElement("span");
      arrow.className = "room-list-arrow";
      arrow.setAttribute("aria-hidden", "true");
      arrow.appendChild(window.SF4eIcons.create("chevron-right", "icon icon-sm"));

      btn.appendChild(main);
      btn.appendChild(arrow);
      btn.addEventListener("click", function () {
        document.getElementById("join-room-code").value = room.code || "";
        document.getElementById("join-address").value = room.code || "";
        showScreen("screen-join");
      });
      li.appendChild(btn);
      ul.appendChild(li);
    });
  }

  document.querySelectorAll(".share-card").forEach(function (card) {
    card.addEventListener("click", function (ev) {
      if (ev.target.closest(".share-copy-btn") || ev.target.closest(".share-actions")) return;
      const id = card.getAttribute("data-share-id");
      const labels = { relay: "relay code", lan: "LAN address", wan: "public address" };
      copyShareValue(id, labels[id]);
    });
  });

  document.querySelectorAll(".share-copy-btn").forEach(function (btn) {
    btn.addEventListener("click", function (ev) {
      ev.stopPropagation();
      const id = btn.getAttribute("data-share");
      const labels = { relay: "relay code", lan: "LAN address", wan: "public address" };
      copyShareValue(id, labels[id]);
    });
  });

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
      delete state.connectionStatus;
      showScreen("screen-home");
      setStatus("");
    });
  });

  document.getElementById("btn-mode-simple").addEventListener("click", function () {
    state.simpleUi = true;
    applyUiMode("user");
  });

  document.getElementById("btn-mode-advanced").addEventListener("click", function () {
    state.simpleUi = false;
    applyUiMode("user");
  });

  document.getElementById("btn-refresh-ip").addEventListener("click", function () {
    setButtonLoading("btn-refresh-ip", true);
    setStatus("Detecting public IP…", "");
    post({ type: "fetchPublicIp" });
  });

  document.getElementById("btn-try-upnp").addEventListener("click", function () {
    const port = document.getElementById("host-port");
    setStatus("Trying UPnP port mapping…", "");
    post({
      type: "tryUpnp",
      sessionPort: port ? parseInt(port.value, 10) : 23456,
    });
  });

  document.getElementById("btn-create-relay-room").addEventListener("click", function () {
    setButtonLoading("btn-create-relay-room", true);
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
    renderHostShareCards(state);
  });

  document.getElementById("host-port").addEventListener("input", function () {
    document.getElementById("host-advertise").dispatchEvent(new Event("input"));
    renderHostShareCards(state);
  });

  document.getElementById("btn-start-host").addEventListener("click", function () {
    const broker = document.getElementById("broker-url");
    if (broker && broker.value) {
      post({ type: "saveSettings", brokerBaseUrl: broker.value });
    }
    const method = getHostConnectMethod();
    const preview = document.getElementById("host-room-preview").textContent;
    const portEl = document.getElementById("host-port");
    const sessionPort = portEl ? parseInt(portEl.value, 10) : state.sessionPort || 23456;
    const advEl = document.getElementById("host-advertise");
    post({
      type: "start",
      mode: "host",
      connectMethod: method,
      displayName: getDisplayName(),
      inputDelay: getInputDelay("host"),
      sessionPort: sessionPort || 23456,
      advertiseHost: advEl ? advEl.value : state.advertiseHost || "",
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

  document.getElementById("btn-check-update").addEventListener("click", function () {
    setUpdateBusy(true);
    setButtonLoading("btn-check-update", true);
    renderUpdateStatus("Checking for updates…", "");
    showInstallButton(false);
    post({ type: "checkUpdate" });
  });

  document.getElementById("btn-install-update").addEventListener("click", function () {
    if (!updateInfo.updateAvailable || updateBusy) return;
    const msg =
      "Install " +
      updateInfo.latestVersion +
      "?\n\nThe launcher will close and replace all files in this folder. Custom files in the install folder will be removed.\n\nClose USF4 if it is running.";
    if (!window.confirm(msg)) return;
    setUpdateBusy(true);
    renderUpdateStatus("Downloading update…", "");
    post({
      type: "applyUpdate",
      zipDownloadUrl: updateInfo.zipDownloadUrl,
      zipApiUrl: updateInfo.zipApiUrl,
      latestVersion: updateInfo.latestVersion,
    });
  });

  const appEl = document.getElementById("app");
  if (appEl) appEl.classList.add("is-loading");
  post({ type: "getState" });
})();
