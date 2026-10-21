// Signing in: the site is an OAuth server for the bridge (a sign-in site, as
// Challonge is), and signs each user in with Discord behind it, through the
// Ember bot's Discord application. A browser on the site's own pages gets a
// session cookie the same way. Codes and tokens are kept hashed.
import { createHash, randomBytes, timingSafeEqual } from "node:crypto";
import type { IncomingMessage, ServerResponse } from "node:http";
import type { DatabaseSync } from "node:sqlite";

import { fail, readBody, send, type Route } from "./http.ts";
import type { Actor } from "./server.ts";

export interface Client {
  id: string;
  secret: string;
  /** The one address codes go back to, exactly. */
  redirect_uri: string;
}

export interface OAuthOptions {
  db: DatabaseSync;
  /** The site's public origin, which Discord sends users back to. */
  origin: string;
  discord: { clientId: string; clientSecret: string; apiBase?: string; authorizeUrl?: string };
  /** The bridge. */
  clients: Client[];
  now?: () => number;
}

/** `organizer` only for an Ember ID the bridge lets organize: the bridge alone asks for it. */
const SCOPES = ["player", "organizer"];
const SECONDS = { code: 300, access: 3600, refresh: 60 * 86400, session: 30 * 86400, pending: 600 };
export const SESSION_COOKIE = "bracket_session";

interface Pending {
  until: number;
  /** Back to a client with a code, or back to a page of the site with a session. */
  client?: Client;
  redirect_uri?: string;
  scope: string;
  state?: string;
  back?: string;
}

interface Grant {
  discord: string;
  username: string;
  scope: string;
}

export class OAuth {
  readonly #options: OAuthOptions;
  // ponytail: pending sign-ins live in memory; a restart only asks those mid-sign-in to start again.
  readonly #pending = new Map<string, Pending>();

  constructor(options: OAuthOptions) {
    this.#options = options;
    options.db.exec(`CREATE TABLE IF NOT EXISTS grants (
      hash TEXT PRIMARY KEY, kind TEXT NOT NULL, client TEXT, redirect_uri TEXT,
      discord TEXT NOT NULL, username TEXT NOT NULL, scope TEXT NOT NULL, expires_at INTEGER NOT NULL)`);
  }

  #now(): number {
    return Math.floor((this.#options.now?.() ?? Date.now()) / 1000);
  }

  #issue(kind: string, grant: Grant, seconds: number, client?: Client): string {
    const value = randomBytes(32).toString("base64url");
    const db = this.#options.db;
    db.prepare("DELETE FROM grants WHERE expires_at <= ?").run(this.#now());
    db.prepare("INSERT INTO grants (hash, kind, client, redirect_uri, discord, username, scope, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)").run(
      hash(value),
      kind,
      client?.id ?? null,
      client?.redirect_uri ?? null,
      grant.discord,
      grant.username,
      grant.scope,
      this.#now() + seconds,
    );
    return value;
  }

  /** The grant `value` holds as `kind`, if it has not expired; `take` uses it up. */
  #find(kind: string, value: string, take = false): (Grant & { client: string | null; redirect_uri: string | null }) | undefined {
    const db = this.#options.db;
    const row = db.prepare("SELECT * FROM grants WHERE hash = ? AND kind = ? AND expires_at > ?").get(hash(value), kind, this.#now()) as
      | (Grant & { client: string | null; redirect_uri: string | null })
      | undefined;
    if (row && take) db.prepare("DELETE FROM grants WHERE hash = ?").run(hash(value));
    return row;
  }

  /** Who a bearer token is: an access token's user, an organizer when its scope says so. */
  actor(token: string): Actor | undefined {
    const grant = this.#find("access", token);
    return grant && { discord: grant.discord, name: grant.username, operator: false, organizer: grant.scope.split(" ").includes("organizer") };
  }

  /** Who a session cookie is: always a player, as only the bridge signs organizers in. */
  session(request: IncomingMessage): Actor | undefined {
    const value = cookie(request, SESSION_COOKIE);
    const grant = value ? this.#find("session", value) : undefined;
    return grant && { discord: grant.discord, name: grant.username, operator: false, organizer: false };
  }

  routes(): Route[] {
    return [
      ["GET", /^\/oauth\/authorize$/, (_, __, url) => this.#authorize(url)],
      ["GET", /^\/signin$/, (_, __, url) => this.#signIn(url)],
      ["GET", /^\/oauth\/discord\/callback$/, async (_, __, url) => this.#callback(url)],
      ["POST", /^\/oauth\/token$/, async (request) => [200, await this.#token(request)]],
      [
        "GET",
        /^\/oauth\/me$/,
        (request) => {
          const token = /^Bearer (.+)$/.exec(request.headers.authorization ?? "")?.[1] ?? "";
          const grant = this.#find("access", token) ?? fail(401, "wrong or missing token");
          return [200, { discord_id: grant.discord, username: grant.username }];
        },
      ],
    ];
  }

  /** Starts a client's sign-in: on to Discord. */
  #authorize(url: URL): never {
    const params = url.searchParams;
    const client = this.#options.clients.find((each) => each.id === params.get("client_id")) ?? fail(400, "unknown client_id");
    if (params.get("redirect_uri") !== client.redirect_uri) fail(400, "redirect_uri is not the client's");
    if (params.get("response_type") !== "code") fail(400, "response_type is code");
    const scope = (params.get("scope") || "player").split(" ");
    if (!scope.every((each) => SCOPES.includes(each))) fail(400, `scope is from ${SCOPES.join(", ")}`);
    return this.#toDiscord({ client, redirect_uri: client.redirect_uri, scope: scope.join(" "), state: params.get("state") ?? undefined });
  }

  /** Starts a browser's sign-in to the site's own pages. */
  #signIn(url: URL): never {
    const back = url.searchParams.get("back") ?? "/";
    return this.#toDiscord({ scope: "player", back: back.startsWith("/") && !back.startsWith("//") ? back : "/" });
  }

  #toDiscord(pending: Omit<Pending, "until">): never {
    const state = randomBytes(16).toString("base64url");
    const now = this.#now();
    for (const [key, each] of this.#pending) if (each.until <= now) this.#pending.delete(key);
    this.#pending.set(state, { ...pending, until: now + SECONDS.pending });
    const { discord, origin } = this.#options;
    const to = new URL(discord.authorizeUrl ?? "https://discord.com/oauth2/authorize");
    for (const [name, value] of Object.entries({ response_type: "code", client_id: discord.clientId, scope: "identify", redirect_uri: `${origin}/oauth/discord/callback`, state, prompt: "none" })) {
      to.searchParams.set(name, value);
    }
    throw new Redirect(to.href);
  }

  async #callback(url: URL): Promise<never> {
    const state = url.searchParams.get("state") ?? "";
    const pending = this.#pending.get(state);
    this.#pending.delete(state);
    if (!pending || pending.until <= this.#now()) fail(400, "this sign-in has expired; start again");
    const back = (params: Record<string, string>) => {
      const to = new URL(pending.redirect_uri!);
      for (const [name, value] of Object.entries({ ...params, ...(pending.state ? { state: pending.state } : {}) })) to.searchParams.set(name, value);
      return new Redirect(to.href);
    };
    const code = url.searchParams.get("code");
    if (!code) {
      if (pending.client) throw back({ error: "access_denied" });
      fail(400, "Discord did not sign you in");
    }
    const user = await this.#discordUser(code);
    if (!user) {
      if (pending.client) throw back({ error: "server_error" });
      fail(502, "Discord did not answer; try again");
    }
    const grant = { discord: user.id, username: user.username, scope: pending.scope };
    if (pending.client) throw back({ code: this.#issue("code", grant, SECONDS.code, pending.client) });
    const session = this.#issue("session", grant, SECONDS.session);
    throw new Redirect(pending.back!, `${SESSION_COOKIE}=${session}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=${SECONDS.session}`);
  }

  /** Who Discord signed in with `code`, or undefined on any failure. */
  async #discordUser(code: string): Promise<{ id: string; username: string } | undefined> {
    const { discord, origin } = this.#options;
    const api = discord.apiBase ?? "https://discord.com/api";
    try {
      const token = await fetch(`${api}/oauth2/token`, {
        method: "POST",
        headers: { "content-type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({ grant_type: "authorization_code", code, redirect_uri: `${origin}/oauth/discord/callback`, client_id: discord.clientId, client_secret: discord.clientSecret }),
        signal: AbortSignal.timeout(10_000),
      });
      if (!token.ok) return undefined;
      const { access_token } = (await token.json()) as { access_token?: string };
      const me = await fetch(`${api}/users/@me`, { headers: { authorization: `Bearer ${access_token}` }, signal: AbortSignal.timeout(10_000) });
      if (!me.ok) return undefined;
      const user = (await me.json()) as { id?: unknown; username?: unknown; global_name?: unknown };
      if (typeof user.id !== "string" || !/^\d{17,20}$/.test(user.id) || typeof user.username !== "string") return undefined;
      return { id: user.id, username: typeof user.global_name === "string" && user.global_name ? user.global_name : user.username };
    } catch {
      return undefined;
    }
  }

  /** `POST /oauth/token`: a code or a refresh token for new tokens; a refresh token is used once. */
  async #token(request: IncomingMessage): Promise<object> {
    const form = new URLSearchParams(await readBody(request));
    const client = this.#options.clients.find((each) => each.id === form.get("client_id"));
    if (!client || !same(form.get("client_secret") ?? "", client.secret)) throw new OAuthError(401, "invalid_client");
    let grant;
    switch (form.get("grant_type")) {
      case "authorization_code":
        grant = this.#find("code", form.get("code") ?? "", true);
        if (!grant || grant.client !== client.id || grant.redirect_uri !== form.get("redirect_uri")) throw new OAuthError(400, "invalid_grant");
        break;
      case "refresh_token":
        grant = this.#find("refresh", form.get("refresh_token") ?? "", true);
        if (!grant || grant.client !== client.id) throw new OAuthError(400, "invalid_grant");
        break;
      default:
        throw new OAuthError(400, "unsupported_grant_type");
    }
    return {
      access_token: this.#issue("access", grant, SECONDS.access, client),
      token_type: "Bearer",
      expires_in: SECONDS.access,
      refresh_token: this.#issue("refresh", grant, SECONDS.refresh, client),
      scope: grant.scope,
    };
  }
}

/** A redirect, thrown from a route; the server answers it. */
export class Redirect extends Error {
  readonly location: string;
  readonly cookie: string | undefined;

  constructor(location: string, cookie?: string) {
    super(location);
    this.location = location;
    this.cookie = cookie;
  }

  answer(response: ServerResponse): void {
    response.writeHead(302, { location: this.location, ...(this.cookie ? { "set-cookie": this.cookie } : {}) }).end();
  }
}

/** An error in OAuth's shape (RFC 6749 §5.2). */
export class OAuthError extends Error {
  readonly status: number;

  constructor(status: number, code: string) {
    super(code);
    this.status = status;
  }

  answer(response: ServerResponse): void {
    send(response, this.status, { error: this.message });
  }
}

function hash(value: string): string {
  return createHash("sha256").update(value).digest("hex");
}

function same(given: string, secret: string): boolean {
  return timingSafeEqual(Buffer.from(hash(given)), Buffer.from(hash(secret)));
}

export function cookie(request: IncomingMessage, name: string): string | undefined {
  for (const part of (request.headers.cookie ?? "").split(";")) {
    const [key, ...value] = part.trim().split("=");
    if (key === name) return value.join("=");
  }
  return undefined;
}
