# ember-brackets tests

`cargo test -p ember-brackets` runs everything against fakes on loopback
ports: `support/challonge.rs` and `support/startgg.rs` keep a site's state
(sign-in through `support/oauth.rs`, the organizer's tournament, participants,
sets, scores), and `tournament_flow.rs` runs a whole event through a real
bridge with them.

## Live smoke test

`live_sites.rs` sends read-only requests to the real Challonge and start.gg.
It is ignored by default, and each site is skipped when its token is not set:

```sh
EMBER_LIVE_CHALLONGE_TOKEN=... \
EMBER_LIVE_CHALLONGE_TOURNAMENT=12345678 \
EMBER_LIVE_STARTGG_TOKEN=... \
EMBER_LIVE_STARTGG_EVENT=tournament/some-weekly/event/usf4 \
cargo test -p ember-brackets --test live_sites -- --ignored --nocapture
```

| Variable | What |
|---|---|
| `EMBER_LIVE_CHALLONGE_TOKEN` | A Challonge OAuth access token with the organizer scopes of `bridge/deploy/sites/challonge.json` |
| `EMBER_LIVE_CHALLONGE_TOURNAMENT` | Optional: a tournament of that user's, by ID, for one connector read |
| `EMBER_LIVE_STARTGG_TOKEN` | A start.gg token: an OAuth access token or a personal access token |
| `EMBER_LIVE_STARTGG_EVENT` | Optional: an event slug that user administers, for one connector read |

With a token, it reads the user and their tournament list as the bridge's
site definitions say. With a tournament or event too, the connector reads its
open sets once, with `mark_underway` off and no player linked, so nothing on
the site changes.

A Challonge token comes from the staging OAuth app (the `_STAGING` client ID,
secret and callback in the main checkout's `.env`): open
`https://api.challonge.com/oauth/authorize?response_type=code&client_id=<id>&redirect_uri=<callback>&scope=<scope>`,
sign in, and take `code` from the address it sends you to (the bridge there
says the sign-in expired, and leaves the code unused). Exchange it:

```sh
curl -s https://api.challonge.com/oauth/token \
  -d grant_type=authorization_code -d code=<code> \
  -d client_id=<id> -d client_secret=<secret> -d redirect_uri=<callback>
```

A start.gg personal access token is made at
https://start.gg/admin/profile/developer. Keep tokens in your shell, never in
the repository.
