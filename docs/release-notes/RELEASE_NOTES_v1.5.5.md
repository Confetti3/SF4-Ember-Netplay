# SF4 Ember Netplay v1.5.5

v1.5.5 fixes rooms that would not connect for many US players since November 8. Some internet providers' router filters started blocking the relay servers Ember used for the US, so players behind them could see public rooms but not make or join one. Ember now runs its own US relay.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.4, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.5)** with their `.sha256` files.
- **Everyone in a room needs 1.5.5,** spectators included. A 1.5.4 player who opens a 1.5.5 Discord invitation sees it as damaged rather than from another version.

## Changes

### Connections

- **Ember's own relay for the US.** Since November 8, the security filters on some US routers (Xfinity Advanced Security, Spectrum Security Shield, Cox Panoramic Advanced Security) block the public relays Ember used for US East, US West and Asia. Ember connected to whichever relay still answered, often Europe, and joining a room whose host sat on a blocked relay failed with "could not reach the host directly or through a relay". Ember now uses its own relay, relay.embernetplay.link, for both US regions, so you no longer need to turn those filters off. Europe and Asia keep their public relays for now.
- **The relay only helps players find each other.** Matches still go direct, player to player, whenever your connection allows it, as before.

## Thanks

Rbullwinkl3, Austin_game, orca, humaneshadow, jaiwhite, ArgosForsaken and everyone who posted their network check reports; FRaccie for the network check and for tracking the block down.
