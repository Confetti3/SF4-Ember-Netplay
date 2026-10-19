# BluMint mode

Status: concept. Branch `fraccie/blumint-mode` on `nightly`. Not reviewed.

BluMint runs its own tournaments and brackets; Ember is its game service
(`docs/guides/BLUMINT_QUICKSTART.md`). BluMint mode is how Ember's screens
look when a match or tournament comes from BluMint rather than from a TO's
start.gg or Challonge event.

## What BluMint runs

- **King of the hill, 1 vs All.** A streamer holds the crown. Viewers type
  `!join` in the stream's chat to queue; the next one gets a lobby code and
  plays the crown holder live. The winner of the set takes the crown and
  defends it against the next challenger or cashes out. BluMint pays the pot.
- **Open tournaments.** A bracket run by BluMint and its event host.

On Ember's side both are BluMint matches over the partner API: a lobby code
is an Ember match link, a whole best-of set plays in one room, Ember reports
presence, progress after each game and the set result, and a game cut off by
a disconnect is an abort BluMint can retry, never a result. Money never
touches Ember.

## Mockups

An Arcade room (`fraccie/arcade-cabinet-mode`, the Real Arcade rows view) in
BluMint's colours and type, read from blumint.com: black, `#33CCFF`,
Montserrat with Geist Mono for codes, a red LIVE mark. Ember's own layout and
button shapes stay, so it reads as Ember with a BluMint theme. Names and
amounts are placeholders.

| Mode | Mockup |
|---|---|
| 1 vs All: the crown cabinet, the challenger, the `!join` queue | [1-vs-all](blumint-mode/1-vs-all.webp) |
| Result: new crown holder, defend or cash out | [results](blumint-mode/results.webp) |
| Open tournament: one cabinet pair per set | [open-tournament](blumint-mode/open-tournament.webp) |

## Todo

- [ ] BluMint's brand kit (logo, colours, type, usage rules), with their
      permission to use it in Ember. The mockups use the name in their font,
      not their logo.
- [ ] Mockups for the rest: match call and join, the player's linked
      BluMint account. The TO desk screens stay Ember's: BluMint runs its
      own.
- [ ] Theme as data: BluMint's colours and logo as a second palette beside
      `src/ui/Theme.cxx`, picked by the match's source, so no screen is
      written twice.
- [ ] King of the hill as a room mode on top of the Arcade room.
- [ ] Agree each step with Katie, who owns the BluMint integration (#97,
      #99), and with BluMint.
