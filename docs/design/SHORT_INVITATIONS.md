# Short invitation links

A room's full invitation (`sf4e3:...`, 277 characters) carries the leader's endpoint, the relay, the room ID, the room capability, the expiry, the coordination authority and the 64-character build ID. That is too long to read out or type, and awkward in chat. A short link stands in for it:

```text
https://embernetplay.link/j#K7QM-4XRT-9PZD
```

The code is the 12 symbols after `#`. The game's Paste invitation box accepts the link, the bare code (`K7QM-4XRT-9PZD`, with or without dashes, any case) and the full invitation as before. Copy invitation still copies the full invitation, and it keeps working whether or not the link service is up.

## Flow

1. In a room, the player chooses Copy short link. The game asks the helper (`short_invite` command).
2. The helper derives the room's code from its room ID and capability, so every member of the room gets the same code. It derives a locator, a sealing key and a write token from the code (below), seals the current full invitation with the key and stores it at `https://embernetplay.link/s/v1/<locator>`.
3. The helper answers with the link (`short_invite` event, status `ready`) and the game copies it. If the service did not take it, the answer is `unavailable` and the game copies the full invitation instead, with a notice saying so.
4. While the room is open the helper stores the invitation again whenever it changes (renewal every half hour, a new leader) and at least every ten minutes, so the link follows the room and survives a restart of the service.
5. A joiner pastes the link or code. Their helper derives the same locator and key, fetches the record, opens it, and checks that the invitation inside derives the same code. Then it joins exactly as with a pasted full invitation: the build, expiry and own-room checks are unchanged.

## Opening the link in a browser

The link opens `/j`, a static page. Its script reads the code from the fragment, which browsers never send, so the web server sees only `/j`. The page makes no requests of its own and loads nothing from other sites. It shows the code with three actions:

- Open in Ember: a link to `ember://join/XXXX-XXXX-XXXX`. It is only followed when the player clicks it; the page never redirects by itself, because a browser without the handler would replace the page with an error.
- Copy link and Copy code, with the steps to paste it on the Join room screen. This always works, whatever the browser or system.

`Launcher.exe` registers the `ember:` scheme for the current user on every start (`HKCU\Software\Classes\ember`, no elevation; rewritten only when it names another Launcher.exe), as `"<Launcher.exe>" --join-link "%1"`. When the browser starts it:

1. The launcher accepts only `ember://join/<code>` (any case, optional trailing slash, at most 64 printable ASCII characters, no query, fragment, user, port or escapes) and reads the code from it. Anything else is ignored. The code is never logged.
2. If Ember is already running, the launcher writes the code into a small named section in the session's `Local\` namespace that the game holds (`Local\SF4EmberJoinLink`), signals the game's event and exits quietly. Both objects carry the default security of the player's own token.
3. If Ember is not running, the launcher starts it as usual and passes the code in `SF4E_JOIN_LINK`, which the game reads once and clears.
4. The game opens its menu at the main menu and joins the room, as if the player had pasted the link and chosen Join room. A game that is still starting joins once it reaches the main menu, within two minutes of the link. A link that arrives while a room is open waits, with a notice, until the player has left that room; it then fills the Join room screen and the player chooses Join room, as does a link the game could not use within the two minutes.

| Where | What happens |
|---|---|
| Windows, Ember run at least once | The browser asks whether to open Ember; the game joins the room. |
| Windows, Ember never run, or moved since | The browser reports no handler (or nothing happens). The page's Copy link works. |
| Wine or Proton | The handler is registered inside the Wine prefix, which Linux browsers do not consult, so Open in Ember does nothing. Copy link and paste into Ember works as on Windows. |

The registration stays behind if Ember's folder is deleted; it then points at a missing file and the page's copy fallback still works.

## Code and keys

- Code: 60 bits as 12 Crockford base32 symbols (`0-9`, `A-Z` without `I L O U`), shown as `XXXX-XXXX-XXXX`. `O` is read as `0`, `I` and `L` as `1`. It is the first 60 bits of `BLAKE3-derive_key("SF4 Ember short invitation code v1", room || capability)`.
- Keys: `Argon2id(code, salt = "SF4 Ember short invitation v1", m = 19 MiB, t = 2, p = 1)` gives 64 bytes: a 16-byte locator, a 32-byte AES-256-GCM key and a 16-byte write token. The salt is fixed because a joiner has nothing but the code.
- Record: `nonce (12) || AES-256-GCM(key, invitation text, aad = salt || locator)`, at most 1024 bytes. A fresh random nonce is used for every store.
- Binding: a joiner accepts the record only if the invitation in it derives the code that was entered. A record cannot point a code at another room.

## Service

`rust/ember-short`, an axum service on `127.0.0.1:47810` behind nginx, built and installed with `rust/ember-short/deploy/setup.sh`.

- `PUT /s/v1/{locator}`: `version (1) | ttl seconds (u32 LE) | write token (16) | record`. The first store binds the SHA-256 of the write token; a later store needs the same token (403 otherwise). The service sets the expiry from its own clock, 60 seconds to 2 hours.
- `GET /s/v1/{locator}`: the record, or 404. Responses are `Cache-Control: no-store`.
- `GET /s/health`: `ok`.

Records live in memory only. Limits per client address (an IPv4 address, or an IPv6 /64, hashed with a per-process random key): 20 lookups a minute, 30 stores per ten minutes, 16 live records created. Overall: 20,000 live records (about 22 MB at the largest record size), 1045-byte request bodies. nginx adds 30 requests a minute per address with a burst of 20, and the API and the page have no access log. The unit runs as a transient user with no home, no capabilities, a read-only system and loopback-only sockets.

## What each party learns

- The link service holds, per room, a random-looking locator, a sealed record of about 300 bytes, its expiry, and a keyed hash of the address that created it. It never receives the code, the key or the invitation, and cannot tell which locator belongs to which room.
- Someone who reads the service's memory can only test codes offline: each guess costs one Argon2id evaluation (19 MiB) and can be checked against every stored locator at once. With N live records a hit takes about 2^60 / N guesses. At the full 20,000 records that is about 2^45.7, or 5.5 x 10^13 evaluations; at the order of 10^4 evaluations a second per large GPU, that is roughly 175 GPU-years, for records that expire within two hours of their room closing. At a realistic few hundred live rooms it is thousands of GPU-years.
- Someone without the code who asks the service directly has to guess a code, run Argon2id on it and spend one lookup. Each guess finds a record with probability N / 2^60 (under 2 x 10^-14 at the full store), and lookups are limited to 20 a minute per address.
- Anyone who holds the code, or the full invitation, can join the room, exactly as with the full invitation today; the room capability is what admits a member. They can also replace the record, since the write token comes from the code. That lets a member break the link for others (the full invitation still works), but not point it at another room, because of the binding check.
- The link sits in chat logs and browser history like the full invitation does. It is only as private as the place it is shared.
- Opened through `ember:`, the code is on the launcher's command line and in the game's environment until it is read, where the player's own processes can see it. Any page can offer an `ember://join/` link, but the browser asks before opening Ember, and that answer is the player's choice to join. A link never takes a player out of a room or a match, and a joined room can be left at once.

## Failure handling

| Situation | What the player sees |
|---|---|
| Copy short link, service down or full | The full invitation is copied, with "The link service did not answer, so the full invitation was copied instead." |
| Paste a link, service down | "Could not reach the Ember link service to open this short link. Ask the host for the full invitation and paste that instead." |
| Paste a link with no record (mistyped, or the room closed over two hours ago) | "No room was found for this short link. ..." |
| Record opens but names an expired invitation, another build or another room | The existing expired, other-package or incomplete-invitation messages |

## IPC

- Command `{"type":"short_invite","epoch":E}`. The helper answers once with `{"type":"short_invite","epoch":E,"link":"https://embernetplay.link/j#...","status":"ready"}` or with `"link":"","status":"unavailable"`. It is never answered with an `error` event, so a link failure cannot close the room.
- `join` takes a link or code in `invitation`. Failures use the existing `invalid_or_incompatible_invitation` code with the reasons `short_unavailable` and `short_unknown`; a record that does not open, or names another room, is `malformed`.

The helper and the game ship together, so the new command and event need no version negotiation.

## Operating the service

- Build as katie: `cd ~/ember-short/src && RUSTUP_TOOLCHAIN=1.98.0 cargo build --release --locked && cp target/release/ember-short ~/ember-short/bin/`.
- Install or update: `ssh -t vps 'sudo bash ~/ember-short/setup.sh'`. It installs the binary, the unit, the nginx zone and snippet, adds one `include snippets/ember-short.conf;` line to the 443 server of `embernetplay.link`, checks `nginx -t` (restoring the previous files if it fails) and reloads.
- A restart empties the store; open rooms store their invitation again within ten minutes.
- `SF4E_SHORT_INVITE_SERVICE` points a helper at a loopback service (`http://127.0.0.1:<port>/s/v1/`) for tests; any other value is ignored.
