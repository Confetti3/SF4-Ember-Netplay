# SF4 Ember Netplay v0.9.5

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes

- **Fixes desyncs from throws and grab ultras, such as C. Viper's Burst Time.** The game updates both fighters on separate threads at the same time. During a throw, the thrown fighter's position is read from the thrower's hand while the thrower's own update may still be changing it, so the two PCs could place the fighter a tiny bit differently. The match then ended with "the two games diverged". Fighter updates now run one after another on the game thread, so both PCs always get the same result. This is not specific to Viper and may also fix other random desyncs.
- **Clearer desync reports for developers.** The offline rollback test mode (`SF4E_ROLLBACK_STRESS`) no longer crashes when a fight starts, and when a replay goes wrong it now logs which values differed.

Everyone in a room should use the same release. Validated with the local test suite and repeated offline rollback tests of Burst Time, which went from several mismatches per session to none.

## Install and play

Requires Windows 10 or later (x64), Ultra Street Fighter IV on Steam, and the Microsoft Visual C++ x86 runtime. Extract the entire `sf4-ember-netplay-0.9.5` ZIP, run `preflight.cmd`, then `Launcher.exe`. Both players should use the same package. Players on v0.9.4 can use the in-app updater or the upgrade ZIP.

The fixed in-game menu provides private Iroh rooms, fighter selection, spectators and offline play. Host copies a private invitation; Join pastes it. Both players select Ready. Graphics and button mappings are configured in the native game Options menu.

See [the player guide](../guides/USER_NETPLAY.md) and [troubleshooting](../guides/TROUBLESHOOTING.md). Keep invitations private.
