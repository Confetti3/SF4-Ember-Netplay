const fs = require("fs");
const path = require("path");

const repoRoot = path.join(__dirname, "..", "..");
const srcUi = path.join(repoRoot, "launcher-ui");
const destUi = path.join(__dirname, "..", "ui");

if (!fs.existsSync(srcUi)) {
  console.error("Missing launcher-ui at", srcUi);
  process.exit(1);
}

fs.rmSync(destUi, { recursive: true, force: true });
fs.cpSync(srcUi, destUi, { recursive: true });
console.log("Synced launcher-ui -> electron-launcher/ui");
