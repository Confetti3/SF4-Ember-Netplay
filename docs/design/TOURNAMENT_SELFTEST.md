# Tournament self-test

Two PCs play one bridge tournament match end to end with nobody at either
game: each game creates its Ember ID, trusts a local test bridge, links to a
provider connection with a code, then claims and plays the match through the
real tournament flow (claim, host, publish, bind, permit, game, report). The
runner then checks on the bridge that the match completed and that the
signed result was delivered.

It builds on the in-game self-test (`docs/development/IN_GAME_SELFTEST.md`)
and is compiled in only with CMake option `SF4E_SELFTEST=ON`.

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
`SF4E_SELFTEST_BRIDGE` (its origin) and `SF4E_SELFTEST_CONNECTION`.

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
folder. `run-selftest.ps1` points it at a fresh temp folder for a
tournament test and deletes it afterwards, so each run creates its own
Ember ID. The helper inherits the launcher's environment.

The other is the fighters' input during the game. A game nobody plays ends in a draw on time, and an ember-room-v1
draw scores nobody and is played again, so the host's fighter has to deal
damage. `ReadRuntimeMatchInput` asks `selftest::MatchInput` first; a build
without the self-test has the inline stub that returns false, so the
normal path is unchanged. The pattern (hold forward, press heavy punch for
two frames out of every twenty) is a pure function with a unit test.

## The runner

`scripts/run-tournament-selftest.ps1`, on the PC the bridge runs on (the
desktop), with the other PC (the laptop) as the host:

1. Checks the ports are free on both PCs, the games closed and the other
   PC's package there.
2. Starts the bridge side, `sdk/typescript/tools/selftest-bridge.ts`. It
   initialises a bridge in a fresh folder, sets its port and result
   delivery, issues the provider credential, starts the bridge and the
   result receiver, and holds the ssh tunnel, checking the other PC reaches
   the bridge through it.
3. Starts the host through the other PC's "Ember Interactive" task, with a
   job and log of its own, and the guest here, both through
   `run-selftest.ps1`.
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
