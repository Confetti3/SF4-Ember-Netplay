# Private test packages

A private test package is an unsigned development build for testing changes,
not an official release or an updater installation. Both players should use
the same package and each needs their own supported Steam copy of Ultra Street
Fighter IV. Review the specific change's pull request for its behavior, known
limitations and acceptance checklist.

## Start and share

After a successful private package build, `dist` contains:

- `PLAY.cmd`: opens the current launcher's normal entry point.
- `Game`: the current playable package.
- `Share with friend.zip`: the archive to give another tester.

Extract the entire archive into a writable folder before starting its launcher.
Do not run from the ZIP or mix files from separate builds. The internal
`BUILD_INFO.txt` and validation receipt identify the source and build label.
The package produced by this development route omits Discord support.

The game requires the supported Microsoft Visual C++ x86 runtime. Bundled
launcher DLLs do not replace the game's system runtime requirement. In native
Options, set **Frame Rate to FIXED** before testing rollback.

## Updates and preservation

`PRIVATE_TEST_BUILD.txt` marks this package as experimental. The launcher and
Updater refuse to install an official update over that marker, because doing
so would silently replace the changes under test. Use a separate official
installation when testing official releases. Recovery and uninstall remain
available; do not remove the marker to bypass the distinction.

Successful local publication replaces the stable `Game` folder and share ZIP
only after verification. One previous verified ZIP and its checksum remain in
`build/private-packages` for rollback. Unmodified extra user files follow the
new playable folder; modified packaged files or conflicting extras are
preserved separately and reported. Unknown or corrupt packages are not pruned.

## Test and report

Start with the pull request's acceptance cases, then exercise complete games,
round changes, rematches, room departure and re-entry on both PCs. Record each
player's settings and the package label. Automated synthetic/network fixtures
do not replace a two-player native game test and do not establish that an
experimental change is stable.

If a failure occurs, save both players' logs before launching again. Use
**Help & About > Export diagnostics** when available and follow the
[log guide](SAVING_LOGS.md). Review diagnostics before sharing them and send
them privately with the build label and reproduction steps. Keep crash dumps
when available. Do not commit player logs or dumps to the repository.

See the [player guide](USER_NETPLAY.md) and
[troubleshooting guide](TROUBLESHOOTING.md) for general help. Public-release and
Discord instructions in those guides do not apply to this private package.
