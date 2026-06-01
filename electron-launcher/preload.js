const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("launcherBridge", {
  postMessage: function (msg) {
    ipcRenderer.send("native-post", msg);
  },
  onMessage: function (handler) {
    ipcRenderer.on("native-message", function (_event, data) {
      handler(data);
    });
  },
});
