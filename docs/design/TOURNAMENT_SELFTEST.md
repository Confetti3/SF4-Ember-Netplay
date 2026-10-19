# Tournament self-test

Two PCs play one bridge tournament match end to end with nobody at either
game: each game creates its Ember ID, trusts a local test bridge, links to a
provider connection with a code, then claims and plays the match through the
real tournament flow (claim, host, publish, bind, permit, game, report). The
runner then checks on the bridge that the match completed and that the
signed result was delivered. A second mode, `-Site challonge`, plays a set of
a real Challonge tournament through the sign-in site and bracket connector
instead of the mock provider ("Challonge mode", below).

It builds on the in-game self-test (`docs/development/IN_GAME_SELFTEST.md`)
and is compiled in only by `scripts\build-current.ps1 -SelfTest` (CMake
option `SF4E_SELFTEST=ON`). No other build has it, and a package or Nightly
publication refuses a build that does.

## Reaching one bridge from both PCs

The helper trusts a bridge origin only over https, or over http on loopback
(`check_origin` with `AllowLoopbackHttp` in `tournament/bridges.rs` and
`tournament/client.rs`). A Tailscale address over http is refused, and a
`tailscale serve` https name needs tailnet certificates and a bridge whose
public origin is that name.

So the bridge runs on the PC that runs the runner, listening on
`http://127.0.0.1:<port>` (a port other than the default 8787), and the
runner holds `ssh -N -R <port>:127.0.0.1:<port>` to the other PC for the
run. Both games then approve the same loopback origin, the one the bridge
names itself by, and nothing outside the two PCs or the tunnel's lifetime is
configured. The bridge is `ember-bridge` from this checkout's `server/ember`.

The bridge's own `mock-local` connection, from `ember-bridge init`, is the
platform: the runner issues its provider credential, and points its
`results_url` at a loopback receiver (`allow_private_webhooks` lets the
bridge send there) with a fresh result secret in the bridge's
`integration_secrets` file.

## What the game does

Two tests, `tournament-host` (the PC with the bridge's tunnel end, Player 1)
and `tournament-guest` (Player 2), share one file,
`sf4e__SelfTest__Tournament.cxx`. They talk to the runner through files in
the folder `SF4E_SELFTEST_EXCHANGE` names, and find the bridge at
`SF4E_SELFTEST_BRIDGE` (its origin) and `SF4E_SELFTEST_CONNECTION`. In the
Challonge mode `SF4E_SELFTEST_SITE` names the sign-in site to connect an
account on instead of linking with a code.

Every bridge and identity step is a runtime command the Ember ID and Linked
accounts screens send (`RuntimeCommand::identity`, `RuntimeCommand::tournament`),
answered through the snapshot's identity view, matched by request:

1. Settings: a name of its own, a fixed input delay, one round of 30
   seconds. A tournament room is opened from the saved settings.
2. Ember ID: created (`IdentityOp::Enable`) in the test's own Ember folder
   (below), then `ember-id` written with the ID and the build.
3. Bridge: looked up at the origin (`BridgeInspect`) and approved
   (`BridgeApprove`), as Look up and Trust do. The test checks the
   connection is offered.
4. Link: waits for `link-code`, claims it (`LinkClaim`, as Link account
   does) and writes the claim's fingerprint to `claim`. It waits until
   `LinkList` shows the link.
5. Match: waits for `match`, refreshes the assignments until the match is
   listed and playable, and plays it (`tournament::Command::Op::Play`).
   From there the runtime's own state machine claims, hosts or joins,
   publishes and binds. The test presses Ready as Ryu at the bound table
   until the game is live.
6. The game: the host's fighter walks forward and attacks; the guest's
   stands still. One game of one round at first to 1 ends in a host win,
   by knockout or on time. The test waits for the match to finish
   (`Phase::Finished`) and for the room to be left.

### Seams

Approving the bridge, typing the link code and pressing Ready are the test
itself: the commands the screens send, from code built only with
`SF4E_SELFTEST` and run only when `SF4E_SELFTEST` names a tournament test.
The fingerprint comparison is the runner's, on the provider side.

Two changes reach outside the self-test files, both compiled out of a
normal build.

The helper keeps the Ember ID and trusted services under
`%LOCALAPPDATA%\Ember`. A test must never touch the player's own, so the
helper has a `selftest` cargo feature, which `cmake/sf4e_helper.cmake` turns
on only with `SF4E_SELFTEST`. With it, `identity::default_directory` takes
the folder `SF4E_SELFTEST_EMBER_DIR` names (an absolute path) as the `Ember`
folder; a value that is set but not absolute is an error, which leaves the
identity unavailable, never the player's folder. `run-selftest.ps1` points it
at a fresh temp folder for a tournament test and deletes it afterwards, so
each run creates its own Ember ID. The helper inherits the launcher's
environment.

The helper does not report which folder it opened, so the test proves it
twice. The folder is new, so an ID that is already ready when the game has not
yet asked for one is the player's: the game fails the step before it trusts
anything. And a run that passed is failed by `run-selftest.ps1` when the helper
never made the `Identity` folder inside the test's folder, as it does when it
makes the ID. A helper built without the feature fails the first.

The other is the fighters' input during the game. A game nobody plays ends in a draw on time, and an ember-room-v1
draw scores nobody and is played again, so the host's fighter has to deal
damage. After `ReadRuntimeMatchInput` has read the controller it calls
`selftest::MatchInput`, which replaces the input of the fighter this PC plays;
a build without the self-test has the inline stub that does nothing, so the
normal path is unchanged. The runtime reads the controller on every frame it
runs, including one GGPO then stalls and repeats, so the test counts reads
and not frames. The pattern (hold forward, press heavy punch for two reads out
of every twenty) is a pure function in `sf4e__SelfTest__Attack.hxx` with a unit
test, kept out of the header the game includes.

The folder rule has a unit test that exists only with the feature: a build
made with `-SelfTest` runs it in its `HelperRust` test, and the Windows suite
runs it on its own (`.github/workflows/windows-suite.yml`).

## The runner

`scripts/run-tournament-selftest.ps1`, on the PC the bridge runs on (the
desktop), with the other PC (the laptop) as the host:

1. Checks the ports are free on both PCs, the games closed and the other
   PC's package and host script there.
2. Starts the bridge side, `sdk/typescript/tools/selftest-bridge.ts`. It
   initialises a bridge in a fresh folder, sets its port and result
   delivery, issues the provider credential, starts the bridge and the
   result receiver, and holds the ssh tunnel, checking the other PC reaches
   the bridge through it.
3. Starts the host through the other PC's task, with a job and log of its
   own (`run-tournament-selftest-host.ps1`, which runs `run-selftest.ps1`),
   and the guest here, through `run-selftest.ps1`.
4. The bridge side, for each game: on `ember-id`, creates a link intent for
   subject `player1` or `player2` and writes `link-code`; on `claim`, reads
   the intent, checks its pending claim is by that game's Ember ID with the
   fingerprint the game showed, and approves exactly that claim.
5. With both linked, it creates the match (`ember-room-v1`, `games_to_win`
   1, the host in slot 0, the host's build) and writes `match` to both.
6. The bridge serves until both games have closed, when the runner writes
   `stop`: each game learns the match is over from the bridge's answer to
   its next claim. The bridge side then checks the match is `completed`
   with the host ahead, and that the receiver verified the signed result
   (`verifyResult`, `webhook-id` `res_<match id>`) naming the host.
7. It stops the bridge, receiver and tunnel and deletes the bridge's
   folder. The runner prints every side's steps.

The other PC's exchange files go through ssh. The decisions the bridge side
makes (an `ember-id` line, the claim to approve, a finished match) are
pure functions in `tools/selftest-checks.ts`, tested in
`test/selftest-checks.test.ts`.

```powershell
scripts\run-tournament-selftest.ps1 -PackageDir <stage> -BridgeExe <ember-bridge.exe> `
    -RemoteHost <ssh name> -RemoteCheckout <checkout there> -RemoteJobs <folder there>
```

The runner assumes nothing of one person's machines: the other PC, its
checkout (this branch, `scripts\build-current.ps1 -SelfTest` built, staged in
its `build\current\stage`) and the jobs folder are all arguments.

### The other PC's task

A process started over ssh has no desktop, so the host game starts through a
scheduled task the other PC provides. The runner needs of it:

- a task named `-RemoteTask` (default `Ember Interactive`) that runs
  `<jobs>\job.cmd` as the signed-in user, in that user's session and without
  elevation, with the output going wherever the task puts it (for example
  `cmd /d /c <jobs>\job.cmd > <jobs>\job.log 2>&1`);
- `<jobs>` writable by the ssh user, and `pwsh` (PowerShell 7) on its `PATH`;
- the task is `Ready` when idle. The runner writes its own job as
  `tourney-host.cmd` and copies it to `job.cmd` only in the ssh call that has
  just seen `Ready`, so another job cannot be taken over; it stops with an
  error when the task is busy.

The job runs `scripts\run-tournament-selftest-host.ps1` from the other PC's
checkout. It writes its own PID to `tourney-host.pid`, and its last line to
`tourney-host.log` is `tourney-host done`, which the runner waits for. A job
that died without that line is noticed when the task is `Ready` again after the
PID file was written. A failed ssh call while waiting is tried again, six times
in a row at most.

### An aborted run

Every wait is bounded: each game stops itself after `-TimeoutMinutes`, the
runner after two minutes more and the bridge side after five.

- A timeout, an error or Ctrl+C in the runner ends the games it started by
  PID, not by name: the guest's game is ended and its script then restores the
  settings, saves and Ember folder (the runner waits two minutes for that), and
  the host's game is ended through the PID in `tourney-host.pid`, found with the
  parent links Windows keeps (a child counts only when it was created after its
  parent, as PIDs are reused). A run killed outright, with no `finally`, leaves
  the games to their own timeouts; the other PC then reports busy until its game
  closes.
- Ctrl+C reaches the bridge side with the runner's; it stops its bridge,
  tunnel and receiver and deletes its work folder, which holds the provider
  token and the result secret, before it exits. A killed one leaves that folder
  in `%TEMP%\ember-tourney-*\bridge`: delete it.
- A failed run keeps its logs, and says where, in `%TEMP%\ember-tourney-*`.
- Settings are restored by each game's own script from `*.selftest` copies next
  to `settings.json` and `ui-preferences.json`; if one is left, the script
  that died did not finish and the copy is the original.

## Challonge mode

`-Site challonge` runs the path a real event uses (`docs/guides/INTEGRATIONS.md`,
"Sign-in sites" and "Bracket sites"): the bridge offers Challonge's sign-in
site, each game connects its Challonge account to its Ember ID in the browser
of its PC, and the `ember-brackets` connector (its `challonge` adapter) makes
the Ember match from the tournament's open set and reports the score back. Only
the platform differs; the games claim, host, bind and play the match as above,
with the host winning.

```powershell
scripts\run-tournament-selftest.ps1 -Site challonge -ChallongeConfig <run configuration> `
    -ConnectorExe <ember-brackets.exe> -PackageDir <stage> -BridgeExe <ember-bridge.exe> `
    -RemoteHost <ssh name> -RemoteCheckout <checkout there> -RemoteJobs <folder there>
```

`ember-brackets.exe` is built from the same checkout as the bridge
(`cargo build --locked -p ember-bridge -p ember-brackets` in `server/ember`).

### What it needs of Challonge

A tournament of the organizer's with one open set between two Challonge
accounts (single elimination, started), the organizer's v1 API key, and a
developer app whose client ID, client secret and redirect URI are known. The
redirect URI has to be `http://127.0.0.1:<-Port>/v1/sites/challonge/callback`:
the bridge names itself by that origin, and each PC reaches it through the same
tunnel as before, so the browser on the other PC is sent to its own loopback.
Each PC's browser has to be signed in to the account of the player that PC
stands for (the games sign in as different accounts; the run stops if both
connect the same one).

### The run configuration

A JSON file kept outside the repository, since it names the organizer's
tournament and where the secrets are. The secrets themselves are in the files
it names, read when they are used and never printed.

```json
{
  "clientId": "<the developer app's client ID>",
  "clientSecretFile": "C:\\path\\to\\client-secret.txt",
  "apiKeyFile": "C:\\path\\to\\organizer-v1.key",
  "tournament": "<the tournament's ID or URL slug>",
  "match": "<the open match's ID>",
  "redirectUri": "http://127.0.0.1:18787/v1/sites/challonge/callback"
}
```

### What differs in the run

- **Bridge.** The bridge side (`tools/selftest-bridge.ts --site challonge`)
  reads the site definition the repository ships
  (`server/ember/bridge/deploy/sites/challonge.json`) with the app's client
  ID, stores the client secret in the bridge's integration secrets, and
  replaces the mock connection with a `direct` one named `brackets`, with
  `lookup_sites` `challonge` and a `results_url` for the connector. It
  issues the connection's provider credential for the connector.
- **Before anything starts** it reads the set once with the organizer's key
  and stops unless it is open with both players in it.
- **Linking.** No code. Each game checks that the bridge lists a sign-in on
  `challonge`, then sends the command the site's Connect screen sends
  (`IdentityOp::SiteConnect`, `site_connect` to the helper), which opens the
  sign-in page in that PC's browser. The person at the PC signs in and
  approves. The game reads the account (`SiteStatus`, every four seconds, as
  the screen does) until one other than the earlier one shows, within five
  minutes, and writes `site-account` (`<user ID> <name>`). The step fails if
  nothing connects.
- **Connector.** With both accounts connected, the bridge side writes the
  connector's `brackets.json` (one Challonge tournament, first to 1, a new
  database and a request budget of 40 for the run) and starts it. Its first
  read finds both players by their accounts, which links them on the
  `brackets` connection, and makes the match. It is started late on purpose:
  the free tier allows 500 requests a month, and a scheduled read for every
  minute people spend signing in would be spent for nothing. The connector's
  secret files are in the bridge side's work folder, which is deleted at
  the end; the organizer's key is read from where it is.
- **Match.** The games link, then each plays the one match the bridge lists
  as playable, writes its ID to `match-id` (the two have to agree, and be the
  connector's, `external_match_id` `challonge:<tournament>:<match>:1`) and
  goes on as in the mock run.
- **Checks.** After both games have closed: the bridge holds the match as
  `completed` with the host ahead and its result `delivered` to the connector
  (which answers a delivery only once its signature verified); and Challonge,
  read once more with the organizer's key, has the match `complete` with
  the host's participant as the winner and 1 - 0. The participant is the
  one the bridge holds for the host's Ember ID, which is the ID the connector
  took from Challonge, so no account name is written down anywhere.
- **Output.** A failed run prints the bridge's and the connector's own logs.

### Request budget

Challonge's free tier is 500 requests a month. A run makes, here, one read of
the set before and one after (a few more only if the report is slow), and the
connector about ten (the open sets, the participants, the mark underway, a read
and the report for the result, a check that the tournament is not over, and at
most a scheduled read or two while the game is played), capped at 40. The two
sign-ins cost the bridge two each, from the bridge's own count.

The connector leaves a set it reported to the organizer, so to play the same set
again, reopen it first:

```powershell
node sdk\typescript\tools\selftest-challonge.ts reopen <run configuration>
```

which reads the set and, unless it is open, sends `change_state` `reopen`.
