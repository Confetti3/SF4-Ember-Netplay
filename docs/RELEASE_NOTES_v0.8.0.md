# SF4 Ember Netplay v0.8.0

SF4 Netplay Launcher is now **SF4 Ember Netplay**. This release brings the latest integrated Ember interface and networking work to the renamed repository. The `release` branch is the default; `main` preserves the legacy launcher and its history.

## What's included

- A fixed in-game Ember overlay with charcoal and orange artwork, controller prompts and controller-first menus.
- Private rooms for up to 16 members, four battle tables, queues, spectators, room chat and short invitations.
- Iroh room coordination and networking, connection checks, recovery handling and a compact rollback HUD.
- Fighter, edition, costume, color, Ultra and stage selection, plus a player profile and confirmed online record.
- SF4 controller assignment, including DirectInput sticks and leverless devices; physical A/B menu actions for assigned XInput pads.
- Discord presence and invitations through the bundled companion.
- Offline training tools, recording/playback controls and a frame meter.
- The latest integrated room, result handling, menu, training and startup changes from the designated source.
- Fresh README screenshots captured from the actual 0.8.0 UI renderer with sample session data.

## Install

Download **`sf4-ember-netplay-0.8.0.zip`** and its SHA-256 sidecar. Extract the entire ZIP into a **new writable folder**, run `preflight.cmd`, then `Launcher.exe`. Keep your legacy launcher folder separate; this is a fresh-install transition. All participants must use the same Ember release.

Requires Windows 10 or later (x64), an owned Steam installation of Ultra Street Fighter IV and the Microsoft Visual C++ x86 runtime. The game is not included. Existing Ember preferences remain under `%APPDATA%\sf4e`.

Ember's updater now uses `Confetti3/SF4-Ember-Netplay` and the branded `sf4-ember-netplay-` ZIP prefix. Legacy releases remain available; the old launcher's in-place upgrade path is not supported for this transition.

## Validation and limitations

The release package is built locally from the tagged source through the designated build and package scripts. Its provenance receipt, source fingerprint, binary hashes, dependency notices and package manifest are included. Local CTests, release asset/version/checksum parsing checks, UI rendering and package preflight validate the corresponding automated paths.

This remains experimental software. UI screenshots use sample data, and synthetic/helper tests do not prove native SF4 gameplay. Host/join, results and repeated rematches, spectators, disconnect recovery, different-network play and clean-machine behavior still require recorded gameplay acceptance. No game installation or launch is performed as part of publishing this release.

## Attribution

Based on **[sf4e](https://codeberg.org/adanducci/sf4e)** by **Anthony Danducci and contributors**, under the MIT license. This is an unofficial port; Anthony Danducci does not maintain, endorse or support this build. The original license, dependency notices and artwork attribution are preserved. Street Fighter imagery belongs to Capcom; Steam belongs to Valve.
