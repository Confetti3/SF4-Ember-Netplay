# SF4 Ember Netplay v1.5.6

v1.5.6 fixes matches that would not start in 1.5.5. Two players could ready up, the game began to load, and both were sent back to the room with "This table's game was cancelled".

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.5, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.6)** with their `.sha256` files.
- **Everyone in a room needs 1.5.6,** spectators included.

## Changes

### Connections

- **Matches start again for players behind router security filters.** 1.5.5 let players on Xfinity, Spectrum and Cox connections join rooms again, but a match also has to find the opponent, and Ember looked them up through a public address service on the same blocked domain as the old relays. When either player was behind such a filter, the lookup came back empty and the match was cancelled after ten seconds. Ember now also looks for the opponent through its own relays, so the match starts whether that service is reachable or not.

## Thanks

ManualCrowcaine for the report and everyone who sent logs of the cancelled matches.
