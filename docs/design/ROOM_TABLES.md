# Classic room tables

The TABLES column of a Classic room's board (`RoomPanelTables.cxx`, sizes in
`TableCardShape.hxx`, offers in `RoomControls`). Arcade rooms draw cabinets
(ARCADE_CABINET_MODE.md).

## What a card shows

The heading says once what every table shares: the room's own rules (Practice
match, Own delay, Wired only), the first table's round rules ("3 rounds / 99
sec", or a set's length and rotation), and how many play and watch.

A card's head (32 px at scale 1) holds TABLE n, its state and its name, and at
its right only what differs from the room: "Queue 2", "1 watching" (never a
zero), and its rules where they are not the first table's.

- Empty (nobody seated, queued or watching): one 48 px line, the head and the
  two seats beside it. Too narrow for that, the seats go under the head.
- One waiting, playing, full: the versus line, 96 px: a 56 px picture, name
  and fighter at each end, vs or the score between. Counts in the head.
- Your own place: the same and a 40 px strip, Ready and Leave seat (Leave
  queue), always shown; a costly leave asks there.
- Closed: its state in the head, no score.

A banner (held start, Ready countdown, Training call) adds an 18 px line. A
card too narrow for the versus line stacks its seats. Cards in a grid's row
share the tallest one's height.

## Offers

A member with no place is offered each table's open seats, else its queue and
watching (`SeatChoices`). An offered seat is itself the button, a plus and
"Play as P1"; one the member cannot take reads "Open seat". A full table's
Join queue and Watch stand at its head's right while it is focused.

- Pad: the focused card lights one offer, P1 first or the only open seat, by
  fill and ink; the card's ring is the only ring. Left and Right move the
  light, and past the first or last go on to the row that way. Up and Down go
  table to table. Select sends the lit offer as last drawn; a seat taken under
  the light sends nothing, and the card lights what is left.
- Mouse: a click on an offer sends it, a moving pointer lights it, a click
  elsewhere on the card focuses it. Right click is Back.
- Table options stay on the bar and on Y.

## Fitting

Cards are as tall as their content, from the top. The list scrolls only when
they do not fit, and lands on a card's top: the focus shows whole with the
least move, and the wheel or the bar settles on the nearest card. Columns stay
the player's choice: a narrower card stacks and grows, so more columns help
only quiet tables.

At 1280x720 the cards have about 375 px with a seated player's two-line bar,
435 with one line: four quiet tables take 216, your seat, a game and two quiet
tables 352, four games 408. At 2438x1330 (scale 1.218) they have about 790:
any four fit, eight quiet take 440, eight games 824 and scroll.

## The line under the list

Only what the focused card does not show: an offer's fuller words, why it is
refused or waiting, a row's detail away from the cards. A table card's own
first line is the card.
