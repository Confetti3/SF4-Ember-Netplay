# SF4 Ember Netplay v0.8.1

This update improves connection checks, room reconnection, host handoff and spectator reliability. It keeps Iroh: gameplay uses unreliable QUIC datagrams over direct IP paths when available, with relay fallback. A direct connection is not guaranteed for every match.

## What's changed

- Incoming gameplay handshakes no longer block room control. One deadline covers admission, and ending a match cancels incomplete handshakes immediately.
- Authenticated members can reconnect to their existing room after its invitation expires, provided their membership remains current. Expired invitations still cannot admit new members.
- Check connection now measures actual datagrams. The optional **30-second connection benchmark** sends 600 gameplay-sized packets and reports actual sends, replies, missed replies, RTT and variation. Local scheduling overload is distinguished from missing network replies. Selected input delay is never changed automatically.
- Route changes update native diagnostics. Unexpected bridge failures retain a specific cause instead of collapsing into a generic disconnect.
- Host departure can discover a valid elected successor even when another remaining voter is stalled.
- GGPO's polling capacity now supports a fighter hosting all 14 room spectators. Acceptance tests require explicit completion as well as a successful exit code.

## Install or update

Download **`sf4-ember-netplay-0.8.1.zip`** and its `.sha256` sidecar. Extract the complete package into a new writable folder, run `preflight.cmd`, then `Launcher.exe`. Existing Ember users can also use Help & About to check for updates. Keep legacy launcher installations separate. **Everyone in a room must use the same Ember release.**

Requires Windows 10 or later (x64), an owned Steam copy of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime. Preferences remain under `%APPDATA%\sf4e`.

## Validation and known limitations

The final release is built locally from its tagged source and passes the 37-test local native suite, package preflight and native updater inventory validation. The package includes its source fingerprint, build provenance, binary hashes, dependency notices and manifest.

Before the version-only release rebuild, the networking candidate passed normal-route and forced-relay 1,800-frame-per-player synthetic GGPO runs with matching final state, 600/600 replies in both connection benchmarks, six matches with 14 verified spectator streams under each route policy, active-match leader recovery, and bridge failure/retry checks. Four simultaneous tables also passed in an earlier scoped build. These records establish the tested paths; they are not whole-release or native-gameplay proof for every scenario.

An earlier debug recovery run timed out waiting for result acknowledgments. Subsequent release-build recovery checks passed, but the earlier timeout does not have a proven root cause or fix. Public N0 relay configuration and invitation encoding are unchanged.

Ember remains experimental. Native SF4 FPS, teleporting, international two-PC play, long-duration stability and clean-machine behavior still need gameplay acceptance. Publishing does not install or launch the game.

## Attribution

Based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci and contributors**, under the MIT license. This is an unofficial port; Anthony Danducci does not maintain, endorse or support this build. Original licenses and dependency and artwork attribution are preserved.
