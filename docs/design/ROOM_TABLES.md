# Classic room tables

The TABLES column of a Classic room's board: rows in `RoomPanel.cxx`
(`TableCardRows`), offers in `RoomControls`, boxes and the fill rule in
`TableCardShape.hxx` (`PlaceTableCards`, `LayOutTableCard`), cards in
`RoomTableCards.cxx`, heading and bar in `RoomPanelTables.cxx`. Arcade rooms
draw cabinets (ARCADE_CABINET_MODE.md).

## The fill rule

The cards fill the column. With R rows (one card a row, or the Grid's two or
three across) every card is `floor((area - gaps) / R)` tall, at most the cap
(208 px at scale 1), at least a whole card's least (Compact: 84 px, 112
stacked, 18 more with a banner). Every card in play is that one height,
whatever it holds: a calm row of tables. What the cap leaves stands under the
last card. Where that height is under the tallest card's least, each card
takes its own least (a banner's card its line more); where that does not fit
either, a quiet table (nobody seated, queued or watching, nothing on offer
but its seats) is one 44 px line (68 where too narrow: its seats under its
head); only then does the list scroll, beside its bar, landing on a card's
top or the floor's end. Four tables take 190 px each in a 780 px column
(1950x1070), about 193 at 2438x1330 (scale 1.218), 89 at 1280x720; four
games and four quiet tables at 1280x720 are four versus cards and four lines.

## Steps

What a card shows steps with the height of one seat's place (a versus card's
body under its head, a stacked card's row), `TableCardStep`:

- Line (a quiet table only): TABLE n, its state and its two seat buttons.
- Compact (from 84 px): pictures 48 px and up, the player's tag (flag, class,
  name, ping, link) over the fighter, costume and Ultra (13 px), a win streak
  beside the name where it fits, else their idle time in the same place (as
  1.6.0 showed one or the other on every card), the score 18 px.
- Medium (from 120): the streak, else the idle time or record, on its own
  line under the fighter (14 px); the score 20 px.
- Roomy (from 160): the name in the heading size (`TagSize::M`, 22 px), the
  fighter 15 px, the streak, then the idle time or record; the score 22 px;
  seat buttons' labels 17 px.

At every step both seats show the fighter, costume and Ultra (the Ultra's
short name where the long one does not fit), as 1.6.0 did: the steps change
the sizes and the lines under them, not what a seat names.

A picture is square, at the seat's outer end, as tall as the body (172 px at
the cap), never leaving a versus line's words, or a stacked card's, their
200 px least (`FaceSize`, `SeatTextLeast`; a Compact picture is the exception,
48 px whatever the width). It is the
pick, else the main, in Settings' art: the game's 1024 px portrait or the
256x384 figure, decoded large past 128 px, standing on the card's ground with
no tile. A banner (held start, Ready countdown, Training call) takes 18 px
from the body. The card is one hairline-bordered ground; TABLE n is a section
heading; the score is in the accent only while the game is played.

## What a card shows

The heading says once what every table shares: the room's own rules and the
first table's round rules, then "Playing 2 · Watching 1", never a zero; rules
that do not fit beside TABLES wrap in a paragraph under it. A card's head line
(28 px) holds TABLE n and its state, its name, what differs from the room, and
its buttons: Join queue and Watch, or on your own seat Ready (Unready,
Rematch) then Leave seat (Cancel the start), on your queue place Leave queue.
Ready is the bar's button again, one source for both (`ReadyButtonOf`): the
same label (Unready short), the same enabled state and reason, shown wherever
the bar shows it (you are seated at that table), never in a tournament room,
which has no Leave to offer. Where the line cannot hold its buttons at their
width the name gives way first, then the buttons share what is left and cut
their labels (the full words are on hover). An open seat the member may take
is the seat's whole half, one button with a plus and "Play as P1"; one they
cannot take reads "Open seat", muted, centred, with no box.

## Offers

A member with no place is offered a table's open seats, else its queue, and
Watch (Unwatch) while anyone sits there (`SeatChoices`): the row's segments of
`SegmentKind::Places`, lit by `MenuNavigation`.

- Pad: a direction arriving on a card lights the offer nearest its side; Left
  and Right step the light, past either end moving on as from any row; A sends
  it. A focus the board or the pointer set lights nothing until a first A. Lit
  is fill and ink; the card's ring is the only ring.
  Your own seat's card (`defaultPlace`): a direction arriving on it lights
  Ready, and A with nothing lit takes Ready, so A on your own seat readies as
  it always did; Left and Right reach Leave seat, A on it asks to leave where
  that gives something up.
- Mouse: a moving pointer lights, a click sends; a double click's second click
  is ignored. Line buttons are pressed on the line's whole height.
- The same press: after an offer was sent to a table, that offer again, or any
  A on that card, does nothing for half a second and until the table's
  revision changes (2 s at most). Ready is sent as an offer: a double click
  readies once, never readies then unreadies.
- A lit place that goes away is refused once by the room's words ("That seat
  was just taken", "This table changed"), then the light goes out.
- While this PC watches a game, an offer that ends the view asks on the card,
  as leaving a scored seat does (one `TableAsk`): Cancel or the offer. A quiet
  card's answers stand where its seats were, across the card and as tall; an
  occupied card's on its line. The question closes when the watched game ends
  and is dropped on a new room or away from the board.
- A on an offer the room would refuse says why.
- Table options: Y on a card, or the bar's.

## The line under the list

Only what the focused card does not show: an offer's fuller words, why it is
refused, a row's detail away from the cards. Speech reads each table's rules
in its row's detail.
