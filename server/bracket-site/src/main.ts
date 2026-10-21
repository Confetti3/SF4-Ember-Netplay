// Runs the bracket site from the environment (README.md).
import { readFileSync } from "node:fs";

import { BridgeClient } from "../../../sdk/typescript/src/index.ts";
import { bracketSite } from "./server.ts";
import { Store } from "./store.ts";

function env(name: string): string {
  const value = process.env[name];
  if (!value) throw new Error(`${name} is not set`);
  return value;
}

/** The first line of the file the variable names, so no secret is in the environment. */
function secret(name: string): string {
  const value = readFileSync(env(name), "utf8").split(/\r?\n/)[0]!.trim();
  if (!value) throw new Error(`the file ${name} names is empty`);
  return value;
}

const [host, port] = (process.env.BRACKET_SITE_LISTEN ?? "127.0.0.1:8790").split(/:(?=\d+$)/);
const server = bracketSite({
  store: new Store(process.env.BRACKET_SITE_DATABASE ?? "bracket-site.sqlite3"),
  organizerToken: secret("BRACKET_SITE_ORGANIZER_TOKEN_FILE"),
  connectorToken: secret("BRACKET_SITE_CONNECTOR_TOKEN_FILE"),
  bridge: new BridgeClient({ origin: env("EMBER_BRIDGE_ORIGIN"), credential: secret("EMBER_BRIDGE_CREDENTIAL_FILE") }),
});
server.listen(Number(port), host, () => console.log(`ember-bracket-site listening on http://${host}:${port}`));
