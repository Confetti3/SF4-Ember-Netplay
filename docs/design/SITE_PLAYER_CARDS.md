# Player cards, points boards and rating details on embernetplay.link

The website shows what Ember's Profile shows: a player's card, the points
boards, and, to each player alone, the rating behind their class. It adds no
stored data. Points and tiers are `FIGHTER_LEVELS.md`, the card is
`PLAYER_CARD.md` and `PROFILE.md`, the class is `SKILL_BADGE.md`; this
document is the pages, the bridge routes behind them and the browser sign-in.

## Decisions

| Question | Decision |
|---|---|
| Which boards? | One for the account's points (BP) and one for each fighter's (FP). BP is an account's total, so there is no BP of a fighter to list. |
| Who is on a board? | Who the class board may list: an account that takes part in ranking and sent a name (`ranking::listed`), whether or not its class has settled. Stopping to take part leaves every board. |
| Who reads a board? | Anyone, like the class board. So the points of a listed player, with the name, flag and class the class board already shows, are public: before, points went only to a caller with a Discord account (`PROFILE.md`, Privacy). A block does not hide a board line, as it does not hide a class board line. |
| Who reads a card? | A browser signed in with Discord whose account is connected to an Ember ID: the rule of `PROFILE.md`, Privacy, so a block holds on the site as in the game. |
| Whose card opens from a board? | Every listed player's, as a room member's opens from a room: a board line carries a card token. Turning friends off takes a player out of search by name and by code, on the site as in the game, and not off the boards; stopping to take part in ranking does that. |
| Who reads a rating? | Its own player, signed in. This is the one change to `SKILL_BADGE.md`, which said the rating never leaves the bridge: it still never reaches another player, a card, a board or a ticket. |
| A place on a board? | Yes: points are a count of games, not skill, so a board has positions where the class board has none. Equal points share a place. |
| How are points written? | As the game writes them: `1986 FP`, no separators. A tier colours a picture's frame, never the number. |
| A link to a card? | By card token from a board (valid 7 days, `FIGHTER_LEVELS.md`) or by name (`/players#name=...`), which finds who the game's own search finds. No lasting public id is made for an account. |
| Titles on a card? | Not yet: a worn title is an award family id whose words are the game's. |

## Pages

All in `server/ember-short/deploy/site`, built by `scripts/site/make.py`.

| Path | Page | Script | Shows |
|---|---|---|---|
| `/rankings` | `rankings.html` | `rankings.js` | The class board, as before, with links to the two points views. |
| `/points` | `points.html` | `points.js` | The BP board (`#bp`) and a fighter's FP board (`#fp=20`), a fighter picker, the signed-in player's own place. |
| `/me` | `me.html` | `me.js` | Signed out: Connect Discord. Signed in without an Ember ID: how to connect one. Signed in: the own card (`#card`) and the rating details (`#rating`). |
| `/players` | `players.html` | `players.js` | Another player's card, by `#card=<token>` or `#name=<name>`. |

`card.js` draws a card for `me.js` and `players.js`; `account.js` reads the
session for every page that needs it and keeps the header's account link.
The site's policy stays `connect-src 'self'`, `form-action 'none'`: every
call is to the site's own origin, which nginx passes to the bridge.

## Bridge routes

Offered where public rooms are on (`rooms::enabled`), like every points
route; 404 otherwise. The `/v1/website/*` routes also need `website` in the
configuration and Discord sign-in.

| nginx path on the site | Bridge route | Credential |
|---|---|---|
| `GET /points.json?fighter=N` | `GET /v1/points/board/public?fighter=N` | none |
| `GET /account/sign-in` | `GET /v1/website/sign-in` | none |
| `GET /account/discord/callback` | `GET /v1/website/discord/callback` | the state cookie |
| `POST /account/sign-out` | `POST /v1/website/sign-out` | a request from the site's own pages |
| `GET /account/me.json` | `GET /v1/website/me` | the session cookie |
| `POST /account/card.json` | `POST /v1/website/card` | the session cookie |

### `GET /v1/points/board/public`

`fighter` 0 to 43 names that fighter's board; without it the answer is the
account board. Any other query is 400.

```json
{"listed_at":1795000000,"fighter":20,"total":580,
 "players":[{"place":1,"name":"Player 2","country":"JP","band":9,
             "points":6912,"tier":4,"card":"c1.…"}]}
```

| Field | Rule |
|---|---|
| `fighter` | the fighter asked for; absent on the account board |
| `total` | players on the board, which can be more than `players` |
| `players` | the first 500 (`MAX_BOARD_LINES`), most points first, then by name |
| `place` | 1 + the players with more points, so equal points share a place |
| `band` | the class the class board shows today, 1 to 10; absent while it is placing or faint, for a developer, and while the bridge hides classes |
| `tier` | 0 to 4, on the account's or the fighters' thresholds |
| `card` | a card token, which only a signed-in browser can open; absent for an account no token names |

The boards are read all at once (`skill::boards::Snapshot`) and kept for 60
seconds, so a page of any board and a player's places come from one moment.
Each board's answer is written once a read, however often it is asked for,
and nginx caches it for 30 seconds.

### Signing in

`GET /v1/website/sign-in` sends the browser to Discord (`identify`, the
scope the game's own sign-in asks) with a state only this bridge seals
(`sealed::Sealed::SignIn`, good for ten minutes), kept in the cookie
`ember_site_state`. Discord returns to
`<website.origin>/account/discord/callback`, which the Discord application's
redirect list must hold. The callback asks Discord who signed in only for a
state this bridge sealed in the last ten minutes, come back to the browser
whose cookie holds it, and only once for each; it answers with the session
cookie and a redirect to `/me`, and anything else redirects to
`/me#sign-in-failed`. Starts are limited to 10 an hour from one address and
240 across the bridge, and since each start is good for one question, those
limits bound what anyone can make the bridge ask Discord, whose answers the
game's own sign-in needs too. Nothing of the Discord account but its user id
is kept, and nothing is written to the database: the account is not
connected to anything by signing in here.

The session is the cookie `ember_site`: a sealed token of the Discord user id
(`sealed::Sealed::Session`, `s1.`). A card token is the same kind of token
for another purpose (`c1.`), and a token opens only as what it was sealed
for, so a card token anyone in a room is shown is never a session. The cookie
is `HttpOnly`, `SameSite=Lax`, `Secure` on an https site, valid 30 days,
and goes only to `/account/`, like the state cookie: no other request to the
site carries a session.

A session is not stored. It reads what its player already sees in the game
and changes nothing, so the bridge keeps no list of them: signing out clears
the cookie in that browser only, and the operator ends every session at once by
setting `website.sessions_from` to the present second, before which no
session is read.

Signing out is taken only from the site's own pages: the browser says the
request is same-origin (`Sec-Fetch-Site`), or, from a browser that does not
say, its `Origin` is the site's. The site's pages send no referrer, under
which a browser sends `Origin: null` with a same-origin POST, so the page
asks with a referrer policy of `same-origin` for the browsers that need the
origin.

The header's account link remembers the signed-in player's name and main
fighter for the tab's session (`sessionStorage`), so pages that do not read
the session can show them; signing out and any signed-out answer forget
them.

### `GET /v1/website/me`

```json
{"state":"signed_out"}
{"state":"no_account"}
{"state":"signed_in",
 "card":{…PlayerCard…},
 "points":{"points":"counted","account":{…OwnAccount…},"fighters":[{…OwnFighter…}]},
 "places":{"account":212,"fighters":[[20,37],[23,58]]},
 "link":"c1.…",
 "rating":{…} ,
 "rules":{…}}
```

`no_account`: the Discord account is connected to no Ember ID. `card` is the
player's card as `POST /v1/cards/lookup` answers it (`PlayerCard`), `points`
the answer of `GET /v1/points/me` without its rival. `places` holds the
player's place on each board they are on; `account` is absent off the account
board. `link` is a fresh token of the player's own card, for a link to share.
30 calls in 10 minutes for an account.

`rating` is absent before the player's first rated game and while the bridge
hides classes:

```json
{"takes_part":true,"rating":1742.0,"deviation":68.0,"volatility":0.0594,
 "band":7,"settled":true,"up_at":1825.0,"down_below":1675.0,
 "faint_in_days":333,"played_at":1795000000,
 "examples":[{"opponent":1742.0,"band":7,
              "win":{"weight":1.0,"change":13.8},
              "loss":{"weight":1.0,"change":-13.0}}]}
```

| Field | Rule |
|---|---|
| `deviation` | grown by the days since the last game, as a game would find it |
| `band` | the class the next room ticket shows; 0 before one |
| `settled` | false while placing, and for a class that shows faint |
| `up_at`, `down_below` | where the class moves (`shown_band`): the next class's floor plus the margin, the own floor less it; absent at the top, at the bottom and before a class |
| `faint_in_days` | whole days without a game until the deviation passes the settled one; absent before a class and once faint |
| `examples` | four next games: a settled opponent at the player's rating, one 200 above, one 400 below, and a new player (`band` 0). Each result's `weight` is how much of a game it counts as and `change` what it adds to the rating |

`rules` is the numbers the rating rules are made of (`skill::explain::RULES`):
`start_rating`, `start_deviation`, `start_volatility`, `floors` (the nine
class floors, D+ to Grand Master), `margin`, `settled_deviation`,
`unsure_deviation`, `clear_deviations`, `full_gap`, `min_gap`, `min_weight`,
`upset_weight`, `full_repeats`, `repeat_weight`, `sitting_secs`. The page
draws every number it explains from `rules` and `rating`, and works none out:
the examples are rated by the code that rates a game.

### `POST /v1/website/card`

`{"card":"c1.…"}` or `{"name":"Player 2"}`; exactly one.

```json
{"card":{…PlayerCard…},"places":{"account":18,"fighters":[[34,3]]}}
{"players":[{…SearchHit…}]}
{"card":null}
```

A token answers its card, or `null` when it does not open, is over 7 days
old, or names a player who blocked the caller. A name answers the card of the
one player with exactly that name (ASCII case aside), the players to choose
from when several have it, and `null` for none: it finds who
`POST /v1/cards/search` finds, so never the caller and never a player who
turned friends off; several are listed 20 at most. A caller whose Discord
account is connected to no Ember ID gets 403 with `reason` `no_ember_id`; one
signed out gets 401. 120 calls in 10 minutes for an account.

## Configuration

```json
"website": { "origin": "https://embernetplay.link", "sessions_from": 0 }
```

Needs `discord`; its routes answer only where public rooms are on. Deploying: add
`https://embernetplay.link/account/discord/callback` to the Discord
application's redirects, then the bridge, whose deployed settings
(`deploy/tenants.json`, production's alone) carry `website`, then the site
and its nginx file. Without `website` the signed-in routes answer 404 and the
pages say the service does not offer them yet; the boards need only the new
bridge.

## Work

| Part | Files |
|---|---|
| Listed players, read once | `skill/ranking.rs` (`listed`, which `board` and `public_classes` now share) |
| Boards | `skill/boards.rs`, `skill/levels.rs` (`all_points`, the tier of points), `routes/boards.rs` |
| Own rating | `skill/explain.rs`, `skill/glicko.rs` (`idle_until`) |
| Sealed tokens | `sealed.rs` (card tokens, sessions and sign-in states), `cards.rs` (tokens built on it) |
| Sign-in and the signed-in routes | `routes/website.rs`, `config.rs` (`website`), `http.rs` (the cookie helpers the desk's sign-in shares) |
| Search, in steps | `routes/profiles.rs` (`shown_to`, `hit`, which a name on the site reuses) |
| Site | `pages/{points,me,players}.html`, `assets/{points,me,players,card,account}.js`, `assets/{points,card}.css`, nginx, `scripts/site/preview.py` samples, `scripts/site/test_site.py` |

## Left for later

- The SFV view: SFV's bridge keeps its own points and has no such routes yet.
- Titles and the shown award families on a card.
- A per-game history on the rating page: the bridge keeps no game once it is
  rated.
