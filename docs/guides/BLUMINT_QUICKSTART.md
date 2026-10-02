# Ember bridge: getting started for BluMint

This is the starting point for BluMint's side of the Ember integration. Ember
is the netplay mod for Ultra Street Fighter IV. The Ember bridge is a small web
service that links a player's Ember ID to a BluMint account and keeps the
matches, results and events a tournament platform needs. Your service talks to
the bridge over HTTPS; nothing BluMint-specific runs in the game.

## What you get

| | |
|---|---|
| Staging bridge | `https://bridge.embernetplay.link` |
| Your connection | `blumint-staging` (tenant `blumint`) |
| Provider credential | links accounts, creates matches, lobbies and tournaments, reads events |
| Organizer credential | records game results and corrections for the `blumint` tenant |

Both credentials arrive separately from this document. Treat them as secrets:
send them only in an `Authorization: Bearer` header to the origin above.

`GET https://bridge.embernetplay.link/.well-known/ember-bridge.json` describes
the bridge and `GET /v1/capabilities` (with a credential) lists what it
supports.

This is a staging service for building and testing the integration. Its data
may be reset before production, and it carries no uptime promise yet.

## The pieces in this repository

- `docs/guides/INTEGRATIONS.md`: the API guide (linking, matches, lobbies,
  tournaments, records, events, webhooks).
- `sdk/typescript`: a TypeScript client with no runtime dependencies,
  including webhook verification. See its README.
- `sdk/typescript/tools/walkthrough.ts`: the whole provider flow in one
  script.
- `sdk/typescript/tools/test-player.ts`: a stand-in Ember player for testing
  account linking without the game.
- `docs/design/identity-bridge/`: the full design (EMBER-TB-001), its JSON
  schemas and test fixtures. `STATUS.md` there says what is built and what is
  not.

## First run

With Node 22.18 or later (from nodejs.org), in `sdk/typescript`:

```sh
EMBER_ORIGIN=https://bridge.embernetplay.link \
EMBER_CONNECTION=blumint-staging \
EMBER_PROVIDER_TOKEN=... \
EMBER_ORGANIZER_TOKEN=... \
node tools/walkthrough.ts
```

It links two throwaway test players, plays a first-to-2 match through
organizer results, and prints the events that produced. Every run uses new
test identities.

## Linking a BluMint account to an Ember ID

1. A signed-in BluMint user asks to link Ember. Your backend calls
   `createLinkIntent(subject)`, where `subject` is your stable user ID, and
   shows the returned code to that user only. It works once and expires after
   five minutes.
2. In Ember (Profile, Ember ID, Linked accounts) the player adds the service
   `https://bridge.embernetplay.link` once and trusts it, then types the
   code. Ember proves it holds the player's key and submits a claim. Your
   page should tell players that address.
3. Your page shows the claim's fingerprint (`getLinkIntent`) next to the one
   Ember shows. When the user confirms they match, approve that exact claim
   with `approveLinkClaim`.

From then on `resolvePlayers([subject])` gives that user's Ember ID and
participant ID, which is what matches, lobbies and tournaments take.

Until you have a game build with Ember ID (below), stand in for the player:

```sh
node tools/test-player.ts https://bridge.embernetplay.link blumint-staging <CODE> player-a.key
```

It prints the test Ember ID and fingerprint and submits the claim. Passing a
key file keeps the same test identity across runs; the file holds a private
key, so keep it out of source control.

## Matches and results today

A match is two linked players and a set length (first to 1, 2, 3 or 5). Today
results come from organizer adjudication: your organizer credential records
each game, and the match completes when someone reaches the set length.
Voiding a game scores the match again: a finished match stays finished while
a player still has enough wins, and otherwise reopens if it can. A correction
that would reopen it is refused when either player has started another match
since (and for lobby and bracket sets that have moved on), so read the state
and revision the bridge returns, and handle a refusal, before changing your
own result. Every change is an
event, delivered by webhook (Standard Webhooks signatures), by cursor polling
or by server-sent events.

On top of matches the bridge can run lobbies (first-to-N queues with winner
stays, loser stays or both rotate) and whole tournaments (single and double
elimination, round robin). Use them if they help; a platform that runs its own
brackets only needs matches.

## Not built yet

- **Results from the game.** Ember does not yet report game results to the
  bridge by itself, and tournament matches are not yet enforced inside Ember's
  rooms. That is the next stage of work; until then an organizer records
  results.
- **A player build with Ember ID.** The current public Ember release does not
  include the Ember ID screens. A test build can be provided for end-to-end
  tests with real players.
- **Production.** There is no production bridge yet.

## What we need from BluMint

These decide how the rest is built (spec section 22):

1. **Which direction you want.** This staging bridge assumes BluMint calls the
   Ember bridge with the SDK. If you would rather Ember call BluMint's API, we
   need that API's versioned documentation or OpenAPI schema, sample callbacks
   with secrets removed, and its authentication and secret rotation rules.
2. **Account ownership.** Is the link-code flow above acceptable, or can
   BluMint provide a verified login or a signed subject assertion instead?
3. **Match identity.** What ID you would use as `external_match_id`, and how
   you retry a create or a result after a lost response.
4. **Results.** How BluMint records a set score, a correction, a forfeit and a
   no-show, and what it does with a disputed result.
5. **Play.** What happens when a player clicks Play: a per-player link into
   Ember, or one shared match link.
6. **Webhooks.** The HTTPS URL that should receive bridge events, if any.
7. **Testing.** A staging account and test tournament on your side, and a
   contact for questions.
