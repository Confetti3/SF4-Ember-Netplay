# Classic room tables

The TABLES column of a Classic room's board: rows in `RoomPanel.cxx`
(`TableCardRows`), offers in `RoomControls`, boxes in `TableCardShape.hxx`
(`LayOutTableCard`), cards in `RoomTableCards.cxx`, heading and bar in
`RoomPanelTables.cxx`. Arcade rooms draw cabinets (ARCADE_CABINET_MODE.md).

## What a card shows

The heading says once what every table shares: the room's own rules (Practice
match, Own delay, Wired only) and the first table's round rules ("Rounds 3 /
99 sec", or a set's length and rotation), then "Playing 2 · Watching 1",
never a zero. The counts give way first; rules that do not fit beside TABLES
take a line of their own under it.

A card's line (28 px at scale 1) holds TABLE n and its state, its name, what
differs from the room ("Queue 2 · Watching 1", its rules where they are not
the first table's), and at its right its buttons. Heights:

- Empty, nothing on offer but its seats: one 44 px line, the line's start
  and the two seats beside it (68 px where too narrow: seats under it).
- One waiting, playing, full: a versus line, 84 px: a 48 px picture, name
  and fighter at each end, vs or the score between, a win streak by the name.
- Your own place: the same; Leave seat (Leave queue, Cancel the start) is its
  line's button, Ready is the bar's.
- Closed: its state, no score.

A banner (held start, Ready countdown, Training call) adds 18 px. A card too
narrow for the versus line stacks its seats (112 px). Cards stand 6 px apart;
a grid's row shares its tallest card's height.

## Offers

A member with no place is offered a table's open seats, else its queue, and
Watch (Unwatch) while anyone sits there (`SeatChoices`). They are the row's
segments of `SegmentKind::Places`, lit by `MenuNavigation`. An offered seat
is its own button, a plus and "Play as P1"; the others stand on the card's
line, always drawn. A seat the member cannot take reads "Open seat", muted,
with no box.

- Pad: a direction arriving on a card lights the offer nearest its side.
  Left and Right step the light; past either end they move on as from any
  row. A sends the lit offer. A focus the board moved (a fresh room) or the
  pointer set lights nothing until a first A. Lit is fill and ink; the card's
  ring is the only ring.
- Mouse: a moving pointer lights an offer, a click sends it; a resting one
  leaves the light. A double click's second click is ignored.
- One request a table until the room answers (a new revision, or 2 s).
- A seat taken under the light sends nothing and says "That seat was just
  taken"; the card lights what is left.
- While this PC watches a game, an offer that ends the view asks on the card
  (Cancel, or the offer), as leaving a scored seat does.
- Table options: Y on a card, or the bar's (your seat's or queue's table,
  else the last card you were on).

## Fitting

Cards are as tall as their content. The list scrolls only when they do not
fit, beside its bar, landing on a card's top or the floor's end. At 1280x720
the cards have about 375 px under a seated player's two-line bar: four games
take 354, your seat and three busy tables with one banner 372. At 2438x1330
(scale 1.218) about 790: eight games take 714.

## The line under the list

Only what the focused card does not show: an offer's fuller words, why it is
refused, a row's detail away from the cards. Speech reads each table's rules
in its row's detail.
