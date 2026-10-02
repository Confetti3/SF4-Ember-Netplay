// Claims a link code as a stand-in Ember player, for testing a platform
// integration without the game. Test identities only.
//
//   node tools/test-player.ts <origin> <connection-id> <code> [key-file]
//
// With a key file the same test Ember ID is reused across runs (the file is
// created on first use and holds a private key: keep it out of source control).
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { randomBytes } from "node:crypto";

import { TestPlayer } from "../src/index.ts";

const [origin, connection, code, keyFile] = process.argv.slice(2);
if (!origin || !connection || !code) {
  console.error("usage: node tools/test-player.ts <origin> <connection-id> <code> [key-file]");
  process.exit(2);
}

let seed: Uint8Array | undefined;
if (keyFile) {
  if (existsSync(keyFile)) {
    seed = Buffer.from(readFileSync(keyFile, "utf8").trim(), "hex");
  } else {
    seed = randomBytes(32);
    writeFileSync(keyFile, Buffer.from(seed).toString("hex") + "\n", { mode: 0o600, flag: "wx" });
  }
}

const player = new TestPlayer({ origin, seed });
const claim = await player.claimLink(code, connection);
console.log(`Ember ID     ${player.emberId}`);
console.log(`Fingerprint  ${player.fingerprint}`);
console.log(`Claim        ${claim.claim_id ?? "(already linked)"}  ${claim.state}`);
console.log(`Intent       ${claim.intent_id ?? "-"}`);
