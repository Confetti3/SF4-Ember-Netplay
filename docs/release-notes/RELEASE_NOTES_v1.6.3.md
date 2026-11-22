# SF4 Ember Netplay v1.6.3

v1.6.3 is a patch for 1.6.2. If Windows' speech voice crashes the game, Ember now turns menu speech off at the next start and says so. Controller setup's Back dialog has clearer buttons, Public Rooms' "Paste room link" joins a full invitation link, and the logs carry better details of crashes and stalls.

Experimental unofficial rollback netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Updating

- **From 1.6.2, update in Ember.** Ember offers the update at launch. Your settings, recordings and replays are kept.
- **The zip and setup.exe are on the [release page](https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v1.6.3)** with their `.sha256` files.
- **Everyone in a room needs 1.6.3,** spectators included.

## Changes

### Menu speech

- **A crash in Windows' speech voice turns menu speech off at the next start.** On some PCs the voice Windows provides for menu speech crashes the game soon after the menu opens, and it did so on every start. Ember now notices that crash, turns menu speech off the next time the game starts and tells you once. This is a problem in the Windows voice, not something Ember can repair: the game stays out of its way. To try speech again, turn it on in **Settings > Accessibility > Speech**.

### Controller setup

- **Back during controller setup has two different answers.** The dialog offered "Cancel" and "Cancel setup", which read as the same thing although "Cancel" kept the setup going. The first button now says "Keep setting up".

### Public rooms

- **Paste room link takes a full invitation link.** Copy an embernetplay.link invitation link, press **Paste room link** under Public Rooms and Ember joins that room. A bare code is not taken there; use Join Room for that.

### Crash reports

- **The log has better details of crashes and stalls.** It now records how much memory the system could still commit, and names Windows' in-page error when the game ends with it.

## Known issues

- **A fight can fail to load after a long session,** in Training or online: the game keeps drawing but the fight never starts. The cause is still being looked into; a 1.6.2 report showed plenty of memory space left, so running short of it is not the explanation we thought. Playing borderless or restarting the game after a long session may avoid it. If it happens to you, send your `sf4e.log` from that session.
- **Players in South-East Asia may be connected through a farther relay** when n0's Asia relay does not answer them. There is no Ember relay in Asia yet.
- **The Reply moves list has no EX version of Rolento's Mekong Delta Attack.**
