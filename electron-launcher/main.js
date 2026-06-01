const { app, BrowserWindow, ipcMain } = require("electron");
const path = require("path");
const { spawn } = require("child_process");
const fs = require("fs");

let mainWindow = null;
let nativeProc = null;
let stdoutBuffer = "";

function installDir() {
  if (app.isPackaged) {
    return path.dirname(process.execPath);
  }
  return path.join(__dirname, "..");
}

function resolveNativeLauncher() {
  const envPath = process.env.SF4E_LAUNCHER_EXE;
  if (envPath && fs.existsSync(envPath)) {
    return envPath;
  }

  const root = installDir();
  const candidates = [
    path.join(root, "Launcher.exe"),
    path.join(root, "msvc-build", "steam-p2p", "Launcher.exe"),
    path.join(root, "msvc-build", "default", "Launcher.exe"),
    path.join(__dirname, "Launcher.exe"),
  ];

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

function resolveUiIndex() {
  if (app.isPackaged) {
    const bundled = path.join(process.resourcesPath, "launcher-ui", "index.html");
    if (fs.existsSync(bundled)) {
      return bundled;
    }
  }

  const root = installDir();
  const candidates = [
    path.join(root, "launcher-ui", "index.html"),
    path.join(__dirname, "..", "launcher-ui", "index.html"),
    path.join(__dirname, "ui", "index.html"),
    path.join(__dirname, "launcher-ui", "index.html"),
  ];

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

function flushStdoutLines() {
  let idx = stdoutBuffer.indexOf("\n");
  while (idx >= 0) {
    const line = stdoutBuffer.slice(0, idx).trim();
    stdoutBuffer = stdoutBuffer.slice(idx + 1);
    if (line && mainWindow && !mainWindow.isDestroyed()) {
      try {
        mainWindow.webContents.send("native-message", JSON.parse(line));
      } catch (err) {
        console.error("Bad JSON from native bridge:", line, err);
      }
    }
    idx = stdoutBuffer.indexOf("\n");
  }
}

function startNativeBridge() {
  const launcherPath = resolveNativeLauncher();
  if (!launcherPath) {
    throw new Error(
      "Launcher.exe not found beside this app. Copy Launcher.exe, Sidecar.dll, and steam_api.dll into the same folder as SF4NetplayLauncher.exe, or set SF4E_LAUNCHER_EXE."
    );
  }

  const launcherDir = path.dirname(launcherPath);
  nativeProc = spawn(launcherPath, ["--electron-ipc"], {
    cwd: launcherDir,
    stdio: ["pipe", "pipe", "inherit"],
    windowsHide: true,
  });

  nativeProc.stdout.setEncoding("utf8");
  nativeProc.stdout.on("data", function (chunk) {
    stdoutBuffer += chunk;
    flushStdoutLines();
  });

  nativeProc.on("exit", function (code) {
    nativeProc = null;
    if (mainWindow && !mainWindow.isDestroyed()) {
      if (code === 0) {
        mainWindow.close();
      } else {
        mainWindow.webContents.send("native-message", {
          v: 1,
          type: "error",
          message: "Native launcher exited with code " + code,
        });
      }
    }
  });
}

function createWindow() {
  const uiPath = resolveUiIndex();
  if (!uiPath) {
    throw new Error("launcher-ui/index.html not found (bundled UI missing).");
  }

  mainWindow = new BrowserWindow({
    width: 1080,
    height: 780,
    minWidth: 900,
    minHeight: 640,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });

  mainWindow.loadFile(uiPath);

  mainWindow.on("closed", function () {
    mainWindow = null;
    if (nativeProc) {
      nativeProc.kill();
      nativeProc = null;
    }
  });

  try {
    startNativeBridge();
  } catch (err) {
    mainWindow.webContents.once("did-finish-load", function () {
      mainWindow.webContents.send("native-message", {
        v: 1,
        type: "error",
        message: err.message,
      });
    });
  }
}

ipcMain.on("native-post", function (_event, msg) {
  if (!nativeProc || !nativeProc.stdin.writable) {
    return;
  }
  nativeProc.stdin.write(JSON.stringify(msg) + "\n");
});

app.whenReady().then(createWindow);

app.on("window-all-closed", function () {
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("before-quit", function () {
  if (nativeProc) {
    try {
      nativeProc.stdin.write(JSON.stringify({ v: 1, type: "steamClose" }) + "\n");
      nativeProc.stdin.write(JSON.stringify({ v: 1, type: "cancel" }) + "\n");
    } catch (e) { /* ignore */ }
    nativeProc.kill();
  }
});
