# SF4 Ember Netplay v1.6.4

v1.6.4 is a patch for 1.6.3 that improves relayed connections. Ember now measures every relay before choosing one, so players on the US West Coast can use the US West relay instead of going through the middle of the country. It also stops your relay changing every few minutes.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.3, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.4)** with their `.sha256` files.
- **Everyone in a room needs 1.6.4,** spectators included.

## Changes

### Connection

- **Every relay is measured before one is chosen.** When two players can't connect directly, their match goes through a relay. Ember lists six relays, but the networking library it uses only measured the first five, so the US West relay was never measured and never chosen. West Coast players who couldn't connect directly were sent through the central US relay, which added a lot of ping. All six are measured now, and you get the nearest one.
- **Your relay stays the same unless a better one is clearly faster.** Every five minutes Ember checks all the relays again. Before, that check forgot which relay you were on, so you could switch between two relays in the same region that answered at almost the same speed. Now Ember keeps your relay unless another one is clearly faster.

## Known issues

- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. The cause is still being looked into. Playing borderless or restarting the game after a long session may avoid it. If it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through a farther relay** when n0's Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
