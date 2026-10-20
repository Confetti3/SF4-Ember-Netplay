# Testing tournament mode

Three automated tests cover tournament mode, from the page's own logic to two
real games playing a set that reaches Challonge. None needs a person once it is
set up.

| Test | What it runs | Needs |
|---|---|---|
| Desk unit tests | The web desk's own logic (sets, tiers, counters) in Node | Node 22 |
| Desk end to end, local | A real bridge and connector with a fake Challonge and Discord, the desk in Chrome, the game's side from a terminal | Rust, Node, Chrome on Windows |
| Tournament on two PCs | Two self-test games, the desk in Chrome and a running bridge: the whole event | Two Windows PCs with the game, Steam signed in on both |

## Desk unit tests

```sh
node --test server/ember/bridge/desk/test/sets.test.js
```

## Desk end to end

`scripts/e2e-desk.mjs` drives a headless Chrome over the DevTools protocol and
checks every page at 390, 1440 and 1920 px: nothing scrolls sideways, every
button is at least 44 px tall, no page shows `null`, and each setup step fits
a desktop screen without scrolling. Pictures of every page go to its out
folder.

Local, from `server/ember` in WSL:

```sh
export CARGO_TARGET_DIR=/mnt/d/ember-build/cargo-target CARGO_PROFILE_DEV_DEBUG=0
EMBER_E2E_OUT=/mnt/d/ember-build/e2e-out cargo test -p ember-brackets --test desk_e2e_stack -- --ignored --nocapture &
cargo build -p ember-bridge --example desk_game
node ../../scripts/e2e-desk.mjs
```

It signs in from Ember (a wrong number turned down, the right one signed in),
creates a Challonge tournament in the four steps with tiers, sets the webhook
and sign-ups, readies it, starts the bracket from the desk, decides a disputed
game as a draw, gives one to player 1 and voids it, opens the bracket and the
public page, and stops it and deletes it on Challonge. Start a fresh stack for
each run: sign-in rate limits are per bridge.

A running bridge and the real Challonge are covered by the run on two PCs,
below, which does all of this with real games. (The desk test once had a
staging mode of its own, signed in as a terminal-made Ember ID; it could not
share the organizer's Challonge account with the two-PC run, since an account
belongs to one Ember ID.)

## Tournament on two PCs

`scripts/e2e-tournament.mjs` runs a whole event with nobody at either game:

1. Both games start their tournament self-test. Each keeps its Ember ID in a
   folder of its own, with its Challonge account connected.
2. The web desk shows a code; the organizer's game reads it, shows what asked
   and three numbers, and approves it with the number the page shows. The
   desk is signed in.
3. The desk creates a single elimination tournament on Challonge, first to 1,
   with sign-ups through Ember.
4. Both games sign up for it in Ember. The desk shows 2 signed up.
5. The desk marks it ready and starts the bracket on Challonge.
6. The bracket connector reads the bracket (within a few minutes), links both
   players by their Challonge accounts and creates the set's match.
7. Both games play it: the first game nobody plays ends on time as a draw and
   is played again after both send Rematch; the organizer's game wins the
   second. The desk shows the set done, 1 – 0, 1 draw, and the score is on
   Challonge.
8. The public page shows it, and the desk stops the tournament and deletes it
   on Challonge.

A run that fails deletes its tournament at the end; the next run also deletes
any `ember-2p` tournament an earlier one left.

### Set up once

- **A self-test build** of the game (`SF4E_SELFTEST=ON`, as
  `scripts\build-current.ps1 -SelfTest` makes it), pointed at the bridge
  (`SF4E_EMBER_SERVICE`, and `SF4E_STAGING_BRIDGE=ON` for a staging bridge).
  The runner copies it to the other PC when it changed.
- **The other PC**: ssh with a key, a folder for the run, and a user signed in
  at the desktop with Steam running: the game starts there through a
  scheduled task in that session.
- **Discord, optional**: link Discord to each game's kept Ember ID once (start the self-test package with only
  `SF4E_SELFTEST_EMBER_DIR` set to that folder, then Ember ID › Linked accounts › Discord). With the
  bridge's Discord bot, each player then gets the match link privately, and the run checks it.
- **Two Challonge accounts**: the organizer's (signed in to the browser on
  this PC) and a second player's (signed in to the browser on the other PC).
- **A config file**, by default `~/tournament-lab/e2e/games.json`:

```json
{
  "bridge": "https://ember-bridge.fraccie.be",
  "connection": "desk",
  "out": "/mnt/d/ember-build/e2e-games/out",
  "local": { "package": "C:\\...\\stage-tournament-selftest", "work": "D:\\ember-build\\e2e-games" },
  "remote": { "ssh": "user@other-pc", "package": "C:\\ember-ci\\tournament-selftest\\stage",
              "work": "C:\\ember-ci\\tournament-selftest", "task": "EmberSelfTest" }
}
```

The first run waits for three approvals in the browsers, five minutes each:
the player's Challonge account on each PC, then organizer access on this PC.
Each game keeps its Ember ID in `<work>\ember`, so later runs need nobody.

### Run

```sh
node scripts/e2e-tournament.mjs            # --keep leaves the tournament on Challonge
```

In the lab, `lab/tournament-2p.sh` takes the dev PC's game and the test PC
the way the other two-PC tests do, then runs it; the self-test package is
built with `lab/stage-tournament-selftest.sh`.

The out folder gets the desk at every step, both games while the set is
played (`*-organizer.png` and `*-player.png`), and each game's own output
(`organizer-game.txt`, `player-game.txt`).

### When it fails

| Message | What to do |
|---|---|
| `timed out waiting for this PC's account` (or the other PC's) | First run: approve Challonge in that PC's browser within five minutes, signed in to the right account. Make sure nobody closes the game meanwhile. |
| `organizer access was never allowed` | Approve the second Challonge page on this PC: it asks for the organizer permissions. |
| `the game offers …, the page shows …` | The number the page shows was not among the game's three: the code was read by another Ember ID first. Run again. |
| `sign up: …` | The event is not taking sign-ups (it was marked ready already), or the player's Challonge account is not connected. |
| `the connector never read the bracket` | Check the bracket connector runs next to the bridge and that its connection is the config's `connection`. |
| `the set never finished` | Look at `*-playing-*.png` and the two game outputs: a game that did not reach `match live` names the step that failed. |
| The other PC's game never starts Ember (`Game initialization failed`) | Its user session is disconnected, left by Remote Desktop. Sign in at its screen, or run `tscon <session id> /dest:console` as administrator inside the RDP session. `lab/tournament-2p.sh` checks this first. |
| `*-player.png` pictures are black | The other PC draws the game where a screen capture cannot see it, as without a monitor attached (its desktop at 1024×768 is the sign). Attach a monitor or a dummy display plug. |
| A private message says **Discord refused it** | The player's Discord shares no server with the Ember bot, or takes no messages from it. Join a server the bot is in (the Ember Discord). |
| A game window stays open | The game stops itself after 45 minutes; its script then puts the settings and saves back. |

The games change nothing of the player's own: each run uses its own Ember
folder, and puts Ember's settings, the saves and the replay archive back as
they were.
