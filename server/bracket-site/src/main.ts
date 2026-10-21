// Runs the bracket site from the environment (README.md).
import { readFileSync } from "node:fs";

import { BridgeClient } from "../../../sdk/typescript/src/index.ts";
import { OAuth } from "./oauth.ts";
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
const store = new Store(process.env.BRACKET_SITE_DATABASE ?? "bracket-site.sqlite3");
const origin = process.env.BRACKET_SITE_ORIGIN;
// Sign-in, once the site has its public origin and the Ember bot's Discord application.
const oauth =
  origin && process.env.DISCORD_CLIENT_ID
    ? new OAuth({
        db: store.db,
        origin,
        discord: {
          clientId: env("DISCORD_CLIENT_ID"),
          clientSecret: secret("DISCORD_CLIENT_SECRET_FILE"),
          // Elsewhere only for tests.
          ...(process.env.DISCORD_API_BASE ? { apiBase: process.env.DISCORD_API_BASE } : {}),
          ...(process.env.DISCORD_AUTHORIZE_URL ? { authorizeUrl: process.env.DISCORD_AUTHORIZE_URL } : {}),
        },
        clients: [{ id: env("BRACKET_SITE_BRIDGE_CLIENT_ID"), secret: secret("BRACKET_SITE_BRIDGE_CLIENT_SECRET_FILE"), redirect_uri: env("BRACKET_SITE_BRIDGE_REDIRECT_URI") }],
      })
    : undefined;
const server = bracketSite({
  store,
  ...(oauth ? { oauth, origin } : {}),
  operatorToken: secret("BRACKET_SITE_OPERATOR_TOKEN_FILE"),
  connectorToken: secret("BRACKET_SITE_CONNECTOR_TOKEN_FILE"),
  ...(process.env.BRACKET_SITE_BOT_TOKEN_FILE ? { botToken: secret("BRACKET_SITE_BOT_TOKEN_FILE") } : {}),
  bridge: new BridgeClient({ origin: env("EMBER_BRIDGE_ORIGIN"), credential: secret("EMBER_BRIDGE_CREDENTIAL_FILE") }),
});
server.listen(Number(port), host, () => console.log(`ember-bracket-site listening on http://${host}:${port}`));
