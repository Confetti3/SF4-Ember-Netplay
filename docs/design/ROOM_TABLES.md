# Classic room tables

The TABLES column of a Classic room's board: rows in `RoomPanel.cxx`
(`TableCardRows`), offers in `RoomControls`, boxes and the fill rule in
`TableCardShape.hxx` (`PlaceTableCards`, `LayOutTableCard`), cards in
`RoomTableCards.cxx`, heading and bar in `RoomPanelTables.cxx`. Arcade rooms
draw cabinets (ARCADE_CABINET_MODE.md).

## The fill rule

The cards fill the column, and the tables in play get the room. Every row
(one card, or the Grid's two or three across) keeps its head, its foot and
what stands under its seats (a banner's 18 px, a strip's 38), and what the
column has left goes to the seats, the part of a card that grows: a busy
table's (anybody at it, or more on offer than its seats) twice a quiet
table's while any table is busy, in whole pixels, at most the cap's (172 px
at scale 1: a 208 px card with neither banner nor strip). Once the busy
tables are at the cap the quiet ones take what is left, up to the same cap;
with no busy table every card is the same. So two busy tables' pictures are
one size whether or not one has a strip, and a strip adds to its card rather
than taking from its seats. What the caps leave stands under the last card.
A whole card's least is Compact: 84 px, 112 stacked, 18 more with a banner
and 38 more for a strip (122 px versus, 150 stacked). Where a quiet table's
share would be under that, the quiet tables keep their least and the busy
ones share the rest; where a busy table's would be too, each card takes its
own least; where that does not fit either, a quiet table (nobody seated,
queued or watching, nothing on offer but its seats) is one 44 px line (68
where too narrow: its seats under its head); where the list would still
scroll but does not without the strips, the strips stand aside (below); only
then does the list scroll, beside its bar, landing on a card's top or the
floor's end. Two games with their strips and two quiet tables in a 742 px
column (2438x1330 at scale 1.218) are two cards of 242 with 168 px pictures
and two of 120; a game and three quiet tables in 780 px are 208 and three of
184; at 1280x720 a game and three quiet tables are 105 and three of 84, a
card with a strip keeps its 122 and the quiet tables beside it are lines,
and four busy tables stand their strips aside and are four cards of 89.

## A card's strip

A card's buttons stand in a strip at its foot, under its seats, where 1.6.0
had them (`TableCardContent::strip`, `LayOutTableCard`): another table's Join
queue and Watch, and on the card of the table where you have a place its own
three. They stand at the strip's end, equal, as wide as the widest label
wants and at least 148 px (`StripButton`), a `Between` (6 px) apart, 32 px
tall at scale 1 (scaled with the card); on a card too narrow for that they
share its inner width. The strip is a `Between` under the seats and their
banner, with the foot (8 px) under it, so it is 38 px of the card's least
height. A button that can be pressed has its label in the text's ink; muted
ink is for one that cannot (`v2::CardButton`). On a seat they are Ready up
(Unready, Rematch; Unready short, as on the bar), Leave seat (Cancel the start),
Table options; on your queue place Leave queue and Table options. Ready is
the filled button it is on the bar. A card's head
line holds none of them. The seats' pictures and words are laid out above the
strip at every step and never over it. A question the card asks
(leaving a seat) puts its answers in the strip, so the card does not change
height while it is open.

- Ready is the bar's button, one source for both (`ReadyButtonOf`): the same
  label, enabled state and reason. Table options is the bar's too
  (`TableOptionsButton`: its label, its detail naming the table, its action
  "options"), so the strip, the bar and Y open the same page for your table.
  Neither is shown in a tournament room, which has no Leave to offer (the card
  is not your own place's there), except a held start's Cancel.
- Where the list would scroll with the strips and not without them (four busy
  tables at 1280x720), `PlaceTableCards` stands every strip aside: each card
  is a whole card again and its buttons stand on its head line, from its end
  leftwards. The strip is the card's own, not the column's: where the list
  scrolls anyway the cards keep theirs.
- A label too wide for its button is cut, the full words on hover.

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
256x384 figure, decoded large past 128 px, shown on a cabinet's screen as an
Arcade room shows its players (`DrawSeatScreen`, RoomPanelArcade.cxx): the
bezel fills the square, the fighter stands on the glass's foot in front of
one tone for every table (the accent while its game is played, the muted
tone otherwise: on a card the two players are already together, so a pair's
colour would say nothing), their Ultra in the corner once it is shared and
the screen is a Medium card's or larger (the fighter's line names it at
every size), scanlines over both. Only the screen, never the cabinet round
it. A player's lines stand directly beside their screen, starting at it on
the first seat and a stacked one and ending at it on a versus card's second
seat, so a player reads as one with their fighter and the score stands
alone in the middle. A banner (held start, Ready countdown, Training call) takes 18 px
from the body. The card is one hairline-bordered ground; TABLE n is a section
heading; the score is in the accent only while the game is played.

## What a card shows

The heading says once what every table shares: the room's own rules and the
first table's round rules, then "Playing 2 · Watching 1", never a zero; rules
that do not fit beside TABLES wrap in a paragraph under it. A card's head line
(28 px) holds TABLE n and its state, its name, what differs from the room, and
its buttons only where the strips stand aside (above); otherwise the card's
strip holds them and its head none. Where the line cannot hold its buttons at their
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
  it always did; Left and Right step across Ready, Leave seat and Table
  options (past either end moving on as from any row), A on Leave seat asks to
  leave where that gives something up, A on Table options opens the table's
  options as the bar's button does; Up and Down leave the card. On your queue
  place the strip's buttons are the pointer's: A opens the table's options as
  it always did, B leaves.
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
  occupied card's in its strip (on its line where it has none). The question closes when the watched game ends
  and is dropped on a new room or away from the board.
- A on an offer the room would refuse says why.
- Table options: Y on a card, the bar's button, or your own card's strip.

## The line under the list

Only what the focused card does not show: an offer's fuller words, why it is
refused, a row's detail away from the cards. Speech reads each table's rules
in its row's detail.
