# SF4 Ember Netplay v1.5.7

v1.5.7 gives Europe an Ember relay. Europe was the last region besides Asia still on a public relay, on the same domain that some US routers' security filters have blocked for the US and Asia relays since November 8.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.5.6, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.5.7)** with their `.sha256` files.
- **Everyone in a room needs 1.5.7,** spectators included.

## Changes

### Connections

- **Ember's own relay for Europe.** Players in Europe now meet through relay-eu.embernetplay.link, hosted by FRaccie, in place of the public Europe relay. If the filters on US routers (Xfinity Advanced Security, Spectrum Security Shield, Cox Panoramic Advanced Security) had extended their block to that relay, players behind them could no longer have joined a host in Europe. Asia keeps its public relay for now.
- **Nothing to set up.** Ember still picks the nearest relay that answers. If the Europe relay goes offline, Ember moves to the next nearest one for new connections.
- **The relay only helps players find each other.** Matches still go direct, player to player, whenever your connection allows it. Like any relay, it sees the internet addresses of the players who connect through it; it cannot read a match, which is encrypted between the players.

## Thanks

FRaccie for hosting the Europe relay and setting it up.
