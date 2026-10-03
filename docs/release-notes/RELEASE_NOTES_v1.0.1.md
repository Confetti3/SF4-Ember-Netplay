# SF4 Ember Netplay v1.0.1

v1.0.1 lets you choose how long room links last. In v1.0.0 a copied link stopped working within an hour, even while the room was still open.

Experimental unofficial netplay for Ultra Street Fighter IV, based on [sf4e by Anthony Danducci and contributors](https://codeberg.org/adanducci/sf4e). Preserve upstream attribution and bundled licenses.

## Changes since v1.0.0

- **Choose how long invite links last.** Create room and Settings > Gameplay defaults have a new Invite links last row: 1, 3, 6 or 12 hours, 1, 3, 7 or 30 days, or Until the room closes. The default is now 1 day instead of 1 hour.
- **Links keep their full time while the room is open.** Ember renews the link it shows as the room stays open, so a link you copy lasts close to the time you chose, not as little as half of it.
- **The setting covers the links you copy.** Hosts and guests each choose their own. A guest's copy is renewed to the guest's setting when that is longer than the link they joined with; a shorter setting never cuts a link short.

Everyone in a room needs v1.0.1. v1.0.0 and v1.0.1 cannot join each other's rooms, as with any other package change.

## Testing

The full build passed all 87 automated tests. The new setting has not yet had a two-PC test.
