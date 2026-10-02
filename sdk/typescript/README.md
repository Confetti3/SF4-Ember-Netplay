# @ember/bridge-sdk

A small TypeScript client for the Ember tournament bridge, plus the helpers a
platform needs to receive its events safely. It has no runtime dependencies;
it uses Node's built-in `fetch` and `crypto`.

```ts
import { BridgeClient, verifyWebhook, parseEvent, eventType } from "@ember/bridge-sdk";

const bridge = new BridgeClient({ origin: "https://bridge.example", credential: process.env.EMBER_PROVIDER_TOKEN! });

// Link: show the code to your signed-in user, who enters it in Ember.
const intent = await bridge.createLinkIntent(user.id, user.name);
// After Ember submits a claim, show its fingerprint and approve that exact claim.
await bridge.approveLinkClaim(intent.intent_id, { claimId, emberId, subject: user.id });

// Matches. Reuse the idempotency key when retrying. With
// native_rules_profile "ember-room-v1" the players play it in Ember and the
// game reports each result; with "organizer-reported-v1" an organizer does.
const match = await bridge.createMatch(spec, `match-${bracketMatchId}`);
// A Play button for both players: match.play_url opens the match in Ember,
// or offers to install Ember first.

// A king-of-the-hill lobby: first to 2, the winner keeps the seat.
const lobby = await bridge.createLobby({ external_lobby_id: "stream-night", games_to_win: 2, rotation: "winner_stays", required_build_id: build });
// Queue a linked player who asked to play (for example with a chat command).
await bridge.joinLobby(lobby.lobby_id, { participantId, emberId });

// Sets and games won and lost, and the latest matches.
const record = await bridge.getPlayerRecord(emberId);

// A double elimination bracket, grand final first to 3. Each set becomes a
// match when its players are known; record games on it as usual.
const cup = await bridge.createTournament({ external_tournament_id: "weekly-12", format: "double_elimination", games_to_win: 2, finals_games_to_win: 3, required_build_id: build, metadata: { title: "Weekly 12" } });
await bridge.registerEntrant(cup.tournament_id, { participantId, emberId });
await bridge.startTournament(cup.tournament_id, (await bridge.getTournament(cup.tournament_id)).revision);

// Webhooks: verify the raw body first, then parse.
verifyWebhook(rawBody, request.headers, [subscriptionSecret]);
const event = parseEvent(rawBody);
if (event.type === eventType("match.completed")) { /* advance the bracket */ }
```

`verifyWebhook` follows Standard Webhooks: it checks the timestamp tolerance
(five minutes) and the HMAC over the exact bytes received. Deduplicate on
`event.id`, and answer 2xx only after the event is stored.

The package also implements the identity and signature rules shared with
Ember (`emberIdFromPublicKey`, `verifyProof`, `verifyReport`, `canonicalize`).
Its tests check them against the specification's public fixtures.
`TestPlayer` stands in for a player's game in tests: it links, claims a match,
publishes a room, asks for game permits and signs reports the way Ember does.

## Tests

```sh
npm test
```

`test/bridge.test.ts` runs against the real bridge when
`rust/ember/target/debug/ember-bridge` exists (`cargo build -p ember-bridge`)
and is skipped otherwise. Node 22.18 or later runs the TypeScript sources
directly, as long as it was built with TypeScript support: some Linux
distribution packages leave it out (`node -p process.features.typescript`
prints `false`), so use the nodejs.org build there. `npm run build` emits
JavaScript with `tsc`, and the published package needs no TypeScript support.
