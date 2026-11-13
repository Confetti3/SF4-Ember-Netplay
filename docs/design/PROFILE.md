# Profile

Profile is where a player sees their card, their fighters, their sets, their
friends and their awards, and changes them; and, the same way, another
player's. 1.6.0 shipped the four tabs (Card, Fighters, Friends, Awards). This
revision (1.6.1) answers the owner's two asks after playing it: "the card is
the main social feature so flesh it out more", and "people should be able to
select a user's profile and search their sets".

It replaces the screen sections of `PLAYER_CARD.md` and `FRIENDS.md` as 1.6.0
did, and, once package W1 lands, two privacy paragraphs elsewhere: "Who sees
what" in `FIGHTER_LEVELS.md` (others now see rated games, wins and best
streak, through the awards stream's counters) and "Privacy" in `FRIENDS.md`
(a request's sender and a friend code now open a public card). W1 edits those
two paragraphs. The static mockup is
`Desktop/sf4/mockups/profile-161/index.html` (outside the repository).

Built elsewhere and taken as given:

| Data | From | Holds |
|---|---|---|
| `ShellView::levels` | `netplay::levels::Own` (1.6.0) | own points: account and 44 fighters, `{points, games, wins, tier, floor, next, since}` |
| `ShellView::memberCards` | `netplay::levels::Card` (1.6.0) | a room member's card by token: account and fighters' points and tiers, `relation` |
| `ShellView::friends` | `netplay::friends::State` (1.6.0) | friends with presence and record, requests, invites, blocks, code |
| award counters and catalog | awards stream (`wip/n1113-awards`, migration 063) | per account, own and on a looked-up card: rated games, wins, best win streak, fighters played, top fighter frame, peak class; own friends count. `netplay::awards`: about 7 to 10 **families** (Matches, Wins, Win streak, Highest rank, Fighter mastery, Cast, Friends, perhaps more), each with **levels** at rising thresholds; each level has its own title and a tier (Ash, Copper, Silver, Gold, Ember) that means the same difficulty in every family. One icon per family, drawn in the tier of the highest level reached. The viewer computes a family's level from the counters; wearing a family wears its current level's title and its icon in that level's colour, both moving up by themselves |
| archive browse | archive stream (`wip/n1113-archive`) | the helper's `archive_list` op, sets grouped from games, Replays > Archive and its filter page, download-then-watch, `ReplaysPanel::OpenArchiveFor(name, nav)` |
| Fighter select on Card | menu stream (`wip/n1113-menu`) | Home loses Fighter select; Card has it under Edit card, X / F |

New here: the fuller card, another player's profile, finding a player, the
opponent's card on the post-match prompt, and the data behind them (Data and
wire, migration 064).

## Decisions

| Decision | Why |
|---|---|
| One card in three sizes: **Full** (a Card tab, own or another's), **Panel** (beside a list: Friends, a room's Members, Edit card's preview, Awards' preview) and **Row** (a list row). Each smaller size is the larger one with parts dropped, never a different arrangement. | A player learns one card. The Panel is what 1.6.0 drew as the "other card", now no longer a mostly empty box. |
| The Full card uses the width: the hero, then the player's words and numbers, then a right column of awards, recent sets and (for another player) the record between the two of you. | At 1920x1080 the 1.6.0 card ended its words column at 960 px and left 450 px and the lower third empty. SF4's own player data screen is one dense sheet, not a page of air. |
| Progress meters join the points lines (own card only): `1986 FP`, its meter, "Gold · 514 points to Ember". The PROGRESS heading goes. | The meter belongs to the number it measures. Another player's card is then the same shape with the meters left out. It also pays for the new rows at 1280x720. |
| Record is four cells: Rated games, Win rate (own) or Wins (another's), Best streak, Fighters played. No current streak. | The awards stream makes games, wins and best streak public; a percentage on someone else's card reads as a skill score, which the class already is (see Open questions). A current streak is about the last hour; the room board already shows sets won in the room. |
| "How they play" is preset chips, never free text: Device, Connection, Region, and up to two "Looking for". | Free text needs moderation Ember has no path for (`PLAYER_CARD.md`). Chips are short, translate, and filter nothing out. |
| The awards showcase is up to four award **families**, each its one icon in the tier colour of the level reached, with level pips; the level's title shows for the focused or hovered one. The worn title and icon and the shown families travel as family ids; the card carries the counters and the viewer works out each level. | The owner's model: one icon per family whose colour is how hard the level was. Levels move up by themselves, so nothing a player stores can go stale, and a made-up claim never shows: a family with no level reached by the counters is not drawn. |
| Another player's profile is a page of its own (`player`) with tabs Card, Fighters, Sets, Awards; the room popout's "Player card" opens it. | "Select a user's profile" from anywhere: one page whatever opened it. |
| Sets are found by the exact name the archive stores, and the page says so. | The archive's listing is by name (`n:` keys). Doing it by account needs a stored column and a backfill (Sets by name, below); the page works now and does not pretend otherwise. |
| Adding the opponent after a set is a button on their own line of the post-match prompt, on View / C, never on A, B, X or Y, and never moves the focus or the countdown. | A, B, X and Y are Rematch, Back to room, Character select and New opponent there; mashing through the WIN must never send a request, and sending one must never answer the rematch. |
| Players met in public rooms stay findable for a week (Recent opponents, on this PC only). | Kate's report: the moment around a rematch is short. The card token already lives 7 days. |
| Own profile gains a Sets tab too (Card, Fighters, Sets, Friends, Awards). | "All sets" on the own card needs somewhere to go; the same list on both pages. |

## Points: the SF4 line

Unchanged from 1.6.0: the number, a space, the two-letter label, white with a
dark outline, the label's capitals as tall as the digits, no plate, box,
ground or coloured frame (`v2::Level`, `v2::LevelLines`). FP is a fighter's
points, BP the account's. They are a grind count, never a rank; the class
emblem is the rank. Two lines stack under the name, numbers right-aligned to
the wider one so the labels line up.

```
Player 1
1986 FP   [meter]  Gold · 514 points to Ember      own card only: meter and words
4012 BP   [meter]  Silver · 988 points to Gold
```

| Where | Lines | Digit height (1080 / 720) |
|---|---|---|
| Full card, own or another's | FP of the main (another player in a room: the fighter they play now), BP; own card adds each line's meter and words | 30 / 24 |
| Panel card | the same two lines, no meters | 26 / 22 |
| Home's card (L tag) | unchanged | 22 / 20 |
| A fighter picture's foot (`v2::LevelBand`) | unchanged | fits the picture |

The meter beside a line: `v2::Meter`, 10 tall, starting 24 px after the
label, 320 wide (1080) or 180 (720); its words 13 px muted after it,
"{tier} · {n} points to {next tier}", "{tier} · Top frame", or "Not played
yet" for a main at 0. The tier still never colours a number; it shows as a
picture's 2 px edge and in these words (1.6.0's rule).

## Three sizes

| Part | Full (Card tab) | Panel (beside a list) | Row (a list row) |
|---|---|---|---|
| Fighter picture, tier edge | hero 440 / 260 | 112 / 80 | 48, where the row has a picture |
| Flag, name, class emblem | 36 px name | 22 px name | 16 px name |
| Worn title on its plate, worn icon | title row | title plate under the name; no icon | dropped |
| About: country, "On Ember since 2026-03" | yes | dropped | dropped |
| Presence (friends only) | a line under the about line | the line above the facts (Friends tab) | the row's status line (as 1.6.0) |
| How they play (chips) | all, one line, wraps once | one line, the rest as "+2" | dropped |
| FP and BP lines | 30 / 24, own with meters | 26 / 22 | dropped |
| Record | four cells | one muted line: "2840 rated games · 1164 wins · best streak 12" | dropped |
| Top fighters | three tiles | dropped (the picture is the one fighter) | dropped |
| Awards | four family tiles: icon in its level's tier, level pips, the focused one's title on a line under them; "14 of 34" | four family icons 32 / 28 in their tier, "14 of 34" | dropped |
| Recent sets | three rows and All sets | dropped | dropped |
| You and them (another's) | three cells in the right column | Friends tab: its facts (1.6.0); a room: the relation chip | dropped |
| Actions (another's) | Primary and More under the hero | the panel's own buttons (Friends tab) or the room's rows | the row's value word (1.6.0) |

The Panel is `DrawOtherCard` grown (`PlayerCard.cxx`), its height its
content's, not a fixed 160. The Row is `v2::Tag` S with the picture, as the
friend, member and rankings rows draw it today.

## Structure

Tabs step with LB / RB (PgUp / PgDn) and wrap. Eyebrow "Profile · {name}" on
both pages.

| Screen id | Tab or page | Title | Owner | Opened from |
|---|---|---|---|---|
| `card` | own tab 1 | Card | `ProfilePanel` + `PlayerCard.cxx` | Home's card, Home's Profile row |
| `fighters` | own tab 2 | Fighters | `ProfilePanel` | a strip tile, LB / RB |
| `sets` | own tab 3 (new) | Sets | `PlayerPage` (the archive list, own name) | All sets, a recent set's row's Y, LB / RB |
| `friends` | own tab 4 | Friends · {requests} | `FriendsPanel` | LB / RB, Home's Profile row when requests wait, a request toast |
| `awards` | own tab 5 | Awards | `ApplicationShell__Awards.cxx` | an award tile, LB / RB |
| `card-edit` | page over Card | Edit card | Settings `PageOf` (`CardEdit`) | Y on Card, the Edit card button |
| `card-pins` | page over Edit card (new) | Awards on card | Settings `PageOf` | Edit card's Awards on card row |
| `card-looking` | page over Edit card (new) | Looking for | Settings `PageOf` | Edit card's Looking for row |
| `selection` | page over Card | Fighter select | (menu stream) | X on Card, the Fighter select button |
| `friends-add` | page over Friends | Find a player (was "Add a friend") | `FriendsPanel` | X on Friends, its first row |
| `friends-sent`, `friends-blocked` | pages over Friends | as 1.6.0 | `FriendsPanel` | as 1.6.0 |
| `player` | another's tab 1 (new) | Card | `PlayerPage` (new) | every place in "Where it opens from" |
| `player-fighters` | another's tab 2 | Fighters | `PlayerPage` | a strip tile, LB / RB |
| `player-sets` | another's tab 3 | Sets | `PlayerPage` | All sets, View sets, LB / RB |
| `player-awards` | another's tab 4 | Awards | `PlayerPage` (the wall, read only) | an award tile, LB / RB |
| `player-pick` | page | Players named {name} | `PlayerPage` | a name that more than one card carries |
| `room-member` | room page (exists) | the member's name | `RoomPanel` | Kick or Make host asking (1.6.0); its card is the Panel |

`player*` screens are pushed on the navigation stack, so B returns to
whatever opened them. A profile opened from inside a profile (a set's
opponent, a result of Find a player opened from there) replaces it
(`MenuNavigation::Replace`), so the stack holds one profile at a time.

## Card tab (own)

### Layout, 1920x1080 (content 1840 wide, body about 838 tall)

Three columns, top aligned: A 440, gap 32, B 840, gap 32, C 496.

| Part | Box | Contents |
|---|---|---|
| Hero | A, 440 x 440 | the main large (`SelectionArt` portrait or figure, Settings, Room layout, Pictures); 2 px edge in the main's tier; 56 px shade at the foot with "MAIN" (12 px muted) and the fighter's name (18 px). No number on the hero. No main: `c.frame` ground with Choose a main (Primary) inside. |
| Edit card | A, under the hero, 440 x 48, 16 gap | `v2::Button` Secondary, Y glyph, "Edit card" |
| Fighter select | A, under Edit card, 440 x 48, 8 gap | Secondary, X glyph, "Fighter select" (menu stream) |
| Title row | B, 34 tall, 10 under | worn title on `v2::TitlePlate`; worn icon 40 px on its tier stroke at the row's right end; no title worn: the row is left out |
| Name | B, 44 | flag, name (36 px heading), class emblem (`DrawBadge`, 32 px) |
| About | B, 22, 12 under | "Germany · On Ember since 2026-03" (`card.about`), 15 px muted. The class is the emblem; speech reads "class A+" from the row's words |
| How you play | B, 30, 16 under | `v2::Chip` each, muted tone, 8 apart: device, connection, region, then up to two Looking for. None chosen or known: the row is left out. Wraps to a second line rather than cut |
| Points | B, 2 x 30 + 8, 16 under | FP of the main and BP, each with its meter and words (Points) |
| Record | B, `v2::FactCells` 4 across, 52, 18 under | Rated games · Win rate · Best streak · Fighters played ("12 of 44") |
| Top fighters | B, heading 24 + tiles 184 x 248, 12 apart | "TOP FIGHTERS": the main, then the two highest others by FP; picture 168 with `LevelBand` and tier edge, name 15 px, games 12 px muted |
| Awards | C, heading 24 + four tiles 112 x 112, 16 apart, then a title line 22; 24 under | "AWARDS" with "14 of 34" (`card.of`: levels reached of all levels) muted at the heading's right. A tile is one family: `c.frame` ground with a 2 px edge in the tier of the level reached, the family's icon 56 drawn in that tier's stroke colour (`v2::TierColor(...).stripe`), and under it the level pips: one 6 px square per level, the reached ones filled in the same colour, the rest `c.border`. The title line under the four tiles names the focused or hovered tile: "World Warrior · Matches, level 4 of 5" (`card.award_line`), the title in the heading font; nothing focused there, it names the first tile. A family with no level reached is not shown. None reached: one line "No awards yet. The first comes with your first rated game." |
| Recent sets | C, heading 24 + three rows 496 x 72, 8 apart | "RECENT SETS". A row: this player's fighter and the opponent's, 48 px each, side by side; "vs" + the opponent's flag, name and class (16 px); under it 14 px muted "3 – 1 · Online · 2 d ago" or "3 – 1 · Weekly 12 · Top 8 · 2026-11-10". The score is from this player's side; the fighters are the set's last game's |
| All sets | C, under the rows, 496 x 48, 8 gap | a link row: "All sets" and a chevron; opens the Sets tab |

Column heights: A 560, B about 594, C about 500. The body's lower part is
air, as SF4's sheet has a margin; nothing on the card scrolls.

### 1280x720 (content 1232 wide, body about 500 tall)

Columns A 260, gap 24, B 524, gap 24, C 400. When the body is under 620
logical px tall, these sizes (1.6.0's rule):

| Part | Change |
|---|---|
| Hero | 260 x 260; Edit card and Fighter select 260 x 44, 12 and 8 gaps (368 in all) |
| Title, name, about | 30, 40 (name 30 px), 20 |
| Chips | 28 |
| Points | digits 24; meter 180 wide from 150 px after the line's start; words 13 px |
| Record | 4 cells, 52 |
| Top fighters | heading 22, tiles 112 x 160 |
| Awards | heading 22, four tiles 88 x 88 (icon 44, pips 5 px), title line 20 |
| Recent sets | heading 22, rows 400 x 56 (pictures 40), 6 apart; All sets 40 |

Column B is about 458 tall; C about 380 (own) and 442 (another's).

### Narrow (`v2::Narrow`, under 900 logical px)

One column that scrolls, the focused row scrolled into view:

1. A band 200 tall: the hero 200 x 200; on its right, when the width allows
   560, the title, name, about; else they go under the hero.
2. Chips, then the points lines (meters under each line, full width).
3. Record, 2 across.
4. Top fighters, three tiles across the width.
5. Awards, four tiles across the width.
6. Recent sets rows, then All sets.
7. Edit card, then Fighter select, full width.

### Awards shown

Up to four families. "Automatic" (the default) shows the families at the
highest tiers: ordered by the tier of the level reached, highest first, then
by how far along the family's levels it is, then in the catalog's family
order; the first four. A player may instead choose up to four families on
`card-pins` (families, never single thresholds: a chosen family keeps moving
up by itself). A chosen family with no level reached on the viewer's catalog
is skipped and the gap is filled in the automatic order. Others see the same
families at the levels their own catalog works out from the counters.

### States

The card is always whole (1.6.0's rule): every state draws the hero, name,
points lines, record, strip, awards and recent sets; only numbers and one line
change. The points states are 1.6.0's (`level.*` words under the record). New:

| Part | State | Shows |
|---|---|---|
| Awards | counters not read yet / no account / service without points | the tiles' place holds the points state's line; "0 of 34" |
| Recent sets | reading | three empty rows' grounds, "Reading sets." in the first |
| Recent sets | none under this name | "No archived sets under this name yet." (`card.sets_none`); All sets stays |
| Recent sets | the service keeps no archive | "This service does not keep sets." and All sets is left out |
| Recent sets | unreachable | "Could not reach the archive. Sets show when it answers." |
| Chips | nothing chosen and nothing known | the row is left out |

### Navigation

Rows, placed (`PressEntry`), the D-pad moving by geometry: the three strip
tiles, the four family tiles, the three set rows, All sets, Edit card, Fighter
select, Connect Discord (when shown).

| Input (pad / keys) | On a strip tile | On an award tile | On a set row | On All sets | On Edit card / Fighter select |
|---|---|---|---|---|---|
| A / Enter | Fighters, on that fighter | Awards, on that family | Watch (the archive's download-then-watch) | Sets tab | opens it |
| X / F | Fighter select | same | same | same | same |
| Y / T | Edit card | same | same | same | same |
| B / Esc | Home | Home | Home | Home | Home |
| LB RB | tabs | tabs | tabs | tabs | tabs |

Hint bar: `A Fighters` / `A Awards` / `A Watch` / `A All sets` / `A Edit card`
/ `A Fighter select`, `X Fighter select`, `Y Edit card`, `B Back`, `LB RB
Pages`.

### Edit card (`card-edit`)

A list page; the detail pane on the right draws the Panel card as others see
it, with the player's values, so a change shows at once.

| Group | Row | Value | Select / Left Right | Filled by |
|---|---|---|---|---|
| - | Name | the name | on-screen keyboard (`TextRow`) | player |
| - | Flag | country or Automatic | country picker | player (Automatic: Windows' region) |
| - | Main | fighter | `main-character` | player |
| AWARDS | Title | the worn family's current title ("World Warrior") or None | Awards tab, the worn family focused | player chooses the family; the level moves up by itself |
| AWARDS | Icon | the worn family's name ("Matches") or None | the same | the same; the icon's colour is the level's tier |
| AWARDS | Awards on card | "Your highest" or "3 chosen" | opens `card-pins` | player; Automatic by default |
| HOW YOU PLAY | Device | "Automatic (Stick)", Pad, Stick, Leverless, Keyboard, Not shown | Left / Right step; Select steps | the game by default, the player may override |
| HOW YOU PLAY | Connection | "Shown (Wired)" or Not shown | Left / Right or Select flips | the game; the player only shows or hides it |
| HOW YOU PLAY | Region | "Shown (Europe)" or Not shown | Left / Right or Select flips | the game; the player only shows or hides it |
| HOW YOU PLAY | Looking for | "Long sets, Happy to teach" or None | opens `card-looking` | player only |
| - | Show my Steam profile | switch | flips | player |

What the game fills by itself:

- **Device, Automatic**: the controller this PC plays with now, as
  `ShellView::inputDevice` and the pad's brand say: the keyboard is Keyboard;
  a pad that only Ember's pad setup made usable (`GamepadBrand`
  `Brand::Stick`) is Stick; any other pad is Pad. Leverless cannot be told
  from a pad or a stick, so it is only ever chosen. No controller known: the
  chip is left out.
- **Connection**: `NetworkLink` as the room's link mark measures it (Wired,
  Wi-Fi); Unknown is not shown.
- **Region**: the relay region Ember picked (`PublicRoomsPanel::KnownRegion`),
  named by `network.region_*` (US East, US West, Europe, Asia Pacific, Other
  region); none known: not shown.

`card-pins`: a first row "Show my highest" (switch, on by default; on, the
rest are disabled with its detail); then each family with a level reached, as
a row with its icon in its tier, its current title as the detail and a check
value, at most four on. A fifth says "Four at most. Take one off
first." (`card.pins_full`).

`card-looking`: six switches (Casuals, Long sets, Ranked, Tournament
practice, New to the game, Happy to teach), at most two on; a third says "Two
at most. Turn one off first." (`card.looking_full`).

In a room every row is disabled with `profile.leave_room_to_edit`, as 1.6.0.
Hint bar: `A` (the row's word), `‹ › Change` on a stepping row, `B Back`.

## Another player's profile

### Their Card tab (`player`)

The Full card, the same boxes as the own card, with these differences:

| Part | Another's card |
|---|---|
| Hero | their fighter: in a room the one they picked (`Member::fighter`), else their main |
| Under the hero | **Primary** (no glyph, as the menu stream's fix for Primary panel buttons), then **More** (Secondary, Y glyph), the same boxes as Edit card and Fighter select |
| About | "France · On Ember since 2026-06" |
| Presence | friends only, under the about line: the friend's mark and words as the Friends tab has them ("Playing at table 1 · Player 5's room"); a member of the room this player is in: "In this room"; anyone else: no line |
| Points | FP of the hero's fighter, BP; no meters |
| Record | Rated games · Wins · Best streak · Fighters played |
| Right column, first | three `FactCells`: "You and Player 2" "3 – 5" (or "-"), "Last played" "2 h ago" (or "-"), and "Friend since" "2026-11-12", or "Friends" with "Not yet", "Request sent", "They asked you" or "Blocked"; then 24 under, Awards |
| Awards | their shown four families, each at the level your catalog works out from their counters; "14 of 31" counts without the Friends family, whose counter is not served for others |
| Recent sets | their last three under their name |

### The Primary action and More

The Primary is the one thing most worth doing now; the page opens with the
focus on it when it is enabled, else on the first strip tile.

| Relation and presence | Primary (A) | Sends |
|---|---|---|
| none, they have a card | Add friend | `FriendRequest` with their card token and their name as label |
| received (they asked you) | Accept request | `FriendRequest` with the token (the bridge accepts a mutual request) |
| sent | Request sent (disabled; detail "Cancel it in Friends, Sent.") | - |
| friend, in a joinable public room | Join their room | `FriendJoin` |
| friend, online, you can offer a room | Invite to my room | `FriendInvite` |
| friend, otherwise | View sets | opens `player-sets` |
| blocked (you blocked them) | Unblock (asks first) | `FriendUnblock` |
| no card, or opened by a name with no card | View sets | opens `player-sets` |
| reading | the reading words, disabled | - |

**More** (Y, or the button) opens a popout at the button (`MenuEntry::popout`).
Every entry is listed even when it cannot be done, with the reason in its
detail (1.6.0's rule):

| Entry | Offered when | Detail when disabled |
|---|---|---|
| Add friend / Accept request / Request sent | not friends, not blocked | "Cancel it in Friends, Sent." |
| Join their room | a friend in a joinable public room | the friend's room words |
| Watch | a friend fighting in a joinable public room | as the Friends tab |
| Invite to my room | a friend, and you have a room to offer | `friends.invite_unavailable` |
| View sets | always | - |
| Steam profile | opened from a room member who shares it | `room.steam_profile.none` |
| Remove friend, Remove and block | a friend | - (ask first) |
| Block / Unblock | always (Block asks first) | - |

Room moderation (Mute, Kick, Make host) stays in the room's own popout.

### Their Fighters tab (`player-fighters`)

The own Fighters tab's grid (PLAYED first by FP, NOT PLAYED YET in roster
order at 50 % alpha) from their card's fighters; no games under a tile
(others' games per fighter are never served). The panel: their fighter's
picture 112 on its tier edge, the FP line (26), the tier in words ("Gold"),
the five frames with theirs outlined, "Main" chip on their main. A on a tile
does nothing (`info` rows); the hint bar names no A.

### Their Sets tab (`player-sets`), and the own Sets tab (`sets`)

The archive stream's Replays > Archive list, with one filter fixed: the name.

| Part | Contents |
|---|---|
| Filters row | first row, as the Archive tab's: value "{name} · Any fighter · Newest"; Select opens the archive's filter page with the Player row locked to the name (Fighter, Opponent typed into `q`, Event, Round, Sort open) |
| The name note | one 14 px muted line under it: "Sets found by the name Player 2. Someone else who used this name shows here too, and sets played under an earlier name do not." (`profile.sets_note`) |
| Rows | the archive's set rows, newest first, this player's side first |
| Side panel | the archive's: the set's games and Watch now (Primary) |
| Paging, states | the archive's ("Loading more", reading, not offered, unreachable with Retry, nothing matches with Clear filters) |

Navigation is the Archive tab's: A Watch, X Filters, Y Retry when failed, B
Back, LB RB tabs. On a row its popout adds "Player card: {opponent}"
(`profile.card_of`), which replaces this profile with the opponent's.

### Their Awards tab (`player-awards`)

The awards stream's wall, read only: one tile per family, its icon in the
tier of the level their counters reach (a family with none reached drawn
muted, `c.border`), the level pips under it, the current title, and progress
to the next level ("37 of 100"). No Wear popout; the
Friends family is left out (their friends count is never served). Header chip
"Levels 14 of 31" (the awards stream's own words for the count).

### States

The page opens at once with what its opener knew (name, flag, class, fighter
from the row, the room or the friend), and fills in as the card arrives.

| State | Card tab | Tabs | Primary |
|---|---|---|---|
| Reading | the opener's words; points lines and record at 40 % alpha with 0; "Reading Player 2's card." (`profile.reading`) under the record; awards and recent sets as their reading states | all four | disabled, "Reading" |
| Card read | whole | all four | by relation |
| No card (the look-up answers null: no Discord account, an expired token, or a player who blocked you; the page never says which) | name line as the opener knew it; no points lines, record or awards; "Player 2 has no card to show." and "Their sets are still listed by name." | Card, Sets | View sets |
| Blocked by you | name line; "You blocked Player 2. Unblock them to see their card." | Card | Unblock |
| A name, nobody | name line; "No player on Ember goes by Player 2 now." | Card, Sets | View sets |
| A name, several | `player-pick` opens instead: "Players named Player 2", one Row each (picture, flag, class, name, "On Ember since 2026-03"), newest active first; A opens that one | - | - |
| Unreachable | as Reading, words "Could not reach the service. The card shows when it answers." | all four | Try again |
| Service without profiles (an older bridge: no `profiles` feature) | 1.6.0's fields only: points, fighters, relation; no chips, record, awards or since | Card, Fighters, Sets | by relation |
| Your own name or token | the own Profile opens instead, on Card | - | - |

### Where it opens from

| Place | Input | Selector | Notes |
|---|---|---|---|
| A room member (Members list, a seat, a cabinet) | popout, "Player card" (first entry, 1.6.0) | card token | in a private room: no token, the entry opens a name-only profile (Sets) |
| A friend row (Friends tab) | A on a friend with no Join or Invite (offline, or nothing to offer); More, "Player card" (first entry); the pointer on the panel's card | friend id | |
| A request row | More, "Player card" (first entry); the pointer on the panel | request id (new) | a received request only |
| Rankings board row | A (the row is no longer info) | exact name, then country among several | |
| Replays > Archive row | the row's popout: "Player card: {name}", one per side, beside the archive's "Find this player's sets" | exact name | |
| Replays > Matches or Sets row (local replays) | the row's popout, one per side | exact name | |
| The post-match prompt | not opened from there: the prompt adds the opponent (After a set) | - | the opponent is in Recent opponents afterwards |
| Find a player, a result | A | the result's card token | |
| Find a player, Recent opponents | A | the stored card token (7 days) | |
| A set row on a Sets tab | popout, "Player card: {opponent}" | exact name | replaces the profile |

### Sets by name: what it means

The archive lists games by the names their players' games wrote into them,
and finds a player's sets by that name (`n:{name}` keys). So, plainly:

- Two players who use the same name share one list: the Sets tab shows both
  players' sets. The rows show each side's flag and class, which usually tell
  them apart, but nothing guarantees it.
- A player who changed their name has their earlier sets under the earlier
  name. Their Sets tab shows only sets under the name they use now.
- A name typed by someone else (an older build that named the other side)
  stays as typed.

To do it by account later, the bridge would store, for each side of each
archived game, the Discord account that played it, and list by it:

1. `archive_games.account_a`, `account_b` (TEXT, the `discord_user_id` behind
   `player_a` / `player_b` when the game is stored; `player_a` / `player_b`
   are Ember IDs already kept since migration 055, but an Ember ID is cheap and
   a Discord account moves between them).
2. A key `a:{keyed hash of the account}` in `archive_game_keys`, so the
   listing pages by account like any other key, and no raw Discord ID sits in
   the key table.
3. A listing parameter `card={card token}` that the bridge opens to the
   account and lists by that key, so the account itself never leaves the
   bridge.
4. A backfill: online games from `player_a` / `player_b` through
   `discord_accounts` as it stands then (a game whose Ember ID has since moved
   or has no Discord account stays name-only), set games from the roster.

Until then the Sets tab sends both `name` and `q` (Data and wire) and keeps
only sets in which a side's name equals the name.

### Find a player (`friends-add`, retitled)

The Friends tab's first row becomes "Find a player", value "Your code
4F2A-91CX" (the code in the heading font, as 1.6.0). X / F opens it from the
tab. Its rows:

| Row | Contents | A |
|---|---|---|
| Name or friend code | `TextRow`, on-screen keyboard, 32 bytes at most | opens the keyboard |
| Search | Primary; enabled at two characters | sends `PlayerSearch` |
| Send request | shown only when the text is a whole friend code (8 symbols, 1.6.0's reading); detail "Sends a friend request to that code." | `FriendRequestByCode`, as 1.6.0 |
| "PLAYERS · {n}" then a Row each | picture of their main 48, flag, class, name, and "On Ember since 2026-03" muted | opens their profile |
| "RECENT OPPONENTS" then a Row each | the fighter they played 48, flag, class, name, "Played 2 h ago · Player 5's room" | opens their profile |
| Your friend code, New code | as 1.6.0, last | as 1.6.0 |

Search states: "Looking." while in flight; "No player goes by that name."
(`find.none`); "More players match. Type more of the name." when the bridge
says `more`; the refusals' words as a notice. Results are exact matches
first, then names that begin with the text, most recently active first.

Recent opponents: the last 20 players this PC played in public rooms, newest
first, each kept 7 days (the card token's life), one entry per name and room
visit. Empty: "Players you meet in public rooms show here for a week."
(`find.recent_empty`).

Hint bar: `A {row}`, `B Back`.

## The Panel card in its places

### Friends tab

1.6.0's tab, with:

| Change | Detail |
|---|---|
| First row | "Find a player" (was "Add a friend"); X / F hint "Find a player" |
| A on a friend | Join / Invite as 1.6.0; on a friend with neither, **Player card** (was More) |
| More on a friend or a request | "Player card" first |
| The panel | the Panel card (picture 112 / 80, name, title plate, FP and BP, the record line, chips, the four family icons in their tiers), then presence, the three facts, and the buttons (1.6.0); the pointer on the card part opens their profile |

Panel height at 1080: head 44, card 112 + 12 + record 20 + 10 + chips 28 +
10 + icons 32 (about 230), presence 22, facts 52, buttons 48, gaps: about
470 of the 838 body.

### A room's Members

The Members screen's detail pane draws the focused member's Panel card
(public room, a card read) above the row's words, where today it has words
only. A private room draws the name line alone. The member popout is 1.6.0's:
**Player card** (now opens `player`), Add friend (by relation), Mute, Steam
profile, Kick, Make host. The `room-member` page (Kick, Make host asking)
draws the Panel above its rows.

### Rankings board

Rows stay Rows (flag, class, name); A opens the player's profile by name
(the board lists names; Data and wire). No panel beside the board: a look-up
per focused row would spend the search budget on scrolling.

### Awards tab preview

"YOUR CARD · AS OTHERS SEE IT" draws the Panel with the player's own values.
It is what 1.6.0 drew as a 160 px box, now holding the record line, chips
and the four family icons.

## After a set: the post-match prompt

1.6.0's prompt (`ApplicationShell__PostMatch.cxx`, 720 wide over the WIN,
10 s ring) with the two players' rows grown from 52 to 64 px to carry each
fighter's picture, and the opponent's row carrying Add friend.

| Row | Contents |
|---|---|
| Mine | the fighter I played 48 (tier edge), S tag (flag, class, name), my answer chip |
| Theirs | the fighter they played 48, S tag, then **Add friend** (Secondary, 40 tall, View glyph), then their answer chip |

| Their relation | Their row shows |
|---|---|
| none (a card) | Add friend |
| received | Accept request |
| sent, after pressing | "Request sent", muted words where the button was |
| friend, blocked, no card (a private room, no Discord account) | nothing |

Rules:

- **View / C** sends it from anywhere in the prompt; it is also a placed row
  (`pm-add-friend`): Up from the bar reaches it and A presses it, so a stick
  without View reaches it with the D-pad; the pointer presses it.
- The focus opens on Rematch, as 1.6.0. Add friend never takes the first
  focus, a View press does not move the focus, and A, B, X and Y keep their
  meanings. Sending a request does not answer the prompt, does not change the
  answer shown, and does not touch the ring.
- What it sends: `FriendRequest` with the opponent's card token and their
  name as the label. The word turns to "Request sent" as soon as the request
  is in flight; a refusal comes back as the usual notice.
- Its detail when focused: "Sends Player 2 a friend request. Your answer to
  the rematch stays as it is." (`postmatch.add_friend.detail`)
- Hint bar gains `View Add friend` while the button shows.
- The prompt closes as 1.6.0. The opponent stays in Recent opponents for a
  week, and in the room's Members while they are there.

## Fighters tab (own)

Unchanged from 1.6.0.

## Friends tab

1.6.0's layout, states, pages and navigation stand, with the changes under
"The Panel card in its places". Hint bar: `A {row hint}`, `Y More`, `X Find a
player`, `B Back`, `LB RB Pages`.

## Awards tab (own)

The awards stream's wall (families, progress, Wear popout). The Panel preview
on its left (above).

## Home, Toasts, Settings

Unchanged from 1.6.0, except Home's Fighter select row, which the menu stream
moves to the Card tab.

## Hint bar, every screen

| Screen | A | B | X | Y | View | LB RB |
|---|---|---|---|---|---|---|
| Card | the row's word | Back | Fighter select | Edit card | - | Pages |
| Edit card, `card-pins`, `card-looking` | the row's word | Back | - | - | - | - |
| Fighters | Save main | Back | - | - | - | Pages |
| Sets (own or theirs) | Watch | Back | Filters | Retry (failed) | - | Pages |
| Friends | Accept / Join / Invite / Player card / More / Open / Change | Back | Find a player | More | - | Pages |
| Find a player | the row's word | Back | - | - | - | - |
| Awards | Wear (earned) | Back | - | - | - | Pages |
| Their Card | the row's word | Back | - | More | - | Pages |
| Their Fighters, Awards | - | Back | - | - | - | Pages |
| `player-pick` | Player card | Back | - | - | - | - |
| Post-match prompt | Rematch (the focused button) | Back to room | Character select | New opponent (Arcade) | Add friend | - |

Keys: A Enter, B Esc, X F, Y T, View C, LB RB PgUp PgDn, Delete. The mouse
presses every row, button and hint.

## Data and wire

### What the screens need

| Field | Shown on | Today | New |
|---|---|---|---|
| name, flag | every size | own: preferences; member: room snapshot; friend: friends state | `PlayerCard.name`, `.country` for openers without them (search, request, name) |
| class emblem | every size | own: standing; member: ticket; friend: `badge` | `PlayerCard.badge` (`SkillBadge`, as the friends state carries it) |
| main / hero fighter | Full, Panel, Row | own: preferences; member: `Member::fighter`, `mainFighter`; friend: `main` | `PlayerCard.main` |
| On Ember since | Full | - | `PlayerCard.since`: `player_cards.since`, served as the first second of its month (UTC) |
| presence | Full, Panel | friends state | - (friends only) |
| worn title and icon | Full, Panel | own: preferences | `PlayerCard.worn {title, icon}` (family ids); `player_cards.worn_title`, `worn_icon`. The viewer draws the family's current level: its title, its icon in that level's tier; a family with no level reached is not drawn |
| shown families | Full, Panel | - | `PlayerCard.pins` (0 to 4 family ids; empty is Automatic); `player_cards.pins` |
| award counters | record, awards | awards stream (063): own and `PlayerCard`. The card carries counters only, never levels or tiers: the viewer's catalog works them out | - |
| FP and BP, tiers | Full, Panel | `points/me`, `cards/lookup` | - |
| meters | own Full | `points/me` | - |
| fighters' points | Full strip, their Fighters | `cards/lookup` | - |
| device, connection, region, looking for | Full chips, Panel chips | - | `PlayerCard.style`; `player_cards.device`, `link`, `region`, `looking`; own: preferences and the game's measures |
| you and them | Full (another's) | friends' `record` only | `PlayerCard.versus {won, lost, last_at}` from `pair_records`, for any card |
| relation | actions | `cards/lookup` | - |
| friend id | actions (Join, Invite, Remove) | friends state | `PlayerCard.friend` (when `relation` is `friend`) |
| friend since | Full | friends state | - |
| recent sets, Sets tab | Full, Sets | archive stream's `archive_list` (by `q`) | `name` (exact) on `GET /v1/archive` |
| a card from a name or code | openers by name, search | - | `POST /v1/cards/search` |
| a card from a request | request openers | - | `CardSelector::Request` |
| recent opponents | Find a player | - | client only |

### Migration 064 (`064_player_cards.sql`)

After the awards stream's 063, named in the list in `src/db/migrations.rs`.

```sql
-- What a Discord account's card shows anyone (docs/design/PROFILE.md): the
-- name, flag and main its game last sent, when it came to Ember, the award
-- families it wears and shows (family ids: the viewer's catalog works out
-- each family's level from the card's counters), and how it plays: device, connection,
-- region and what it is looking for, as small numbers. Written by
-- `PUT /v1/cards/me`, and the name, flag and main also by a friends sync
-- (1.6.0 games). One row per account, for good.
CREATE TABLE player_cards (
    discord_user_id TEXT PRIMARY KEY,
    name TEXT NOT NULL COLLATE NOCASE,
    country TEXT,
    main_fighter INTEGER CHECK (main_fighter BETWEEN 0 AND 43),
    since INTEGER NOT NULL,
    worn_title TEXT,
    worn_icon TEXT,
    pins TEXT NOT NULL DEFAULT '',
    device INTEGER NOT NULL DEFAULT 0 CHECK (device BETWEEN 0 AND 4),
    link INTEGER NOT NULL DEFAULT 0 CHECK (link BETWEEN 0 AND 2),
    region TEXT,
    looking INTEGER NOT NULL DEFAULT 0 CHECK (looking BETWEEN 0 AND 63),
    updated_at INTEGER NOT NULL
) WITHOUT ROWID;
-- Find a player: exact and leading matches of a name, any case.
CREATE INDEX player_cards_name ON player_cards (name COLLATE NOCASE, updated_at);

-- Every account friends or rankings already know, with what they sent last.
INSERT INTO player_cards (discord_user_id, name, country, main_fighter, since, updated_at)
SELECT s.discord_user_id, s.name, s.country, s.main_fighter,
    COALESCE((SELECT d.connected_at FROM discord_accounts d WHERE d.user_id = s.discord_user_id), s.seen_at),
    s.seen_at
FROM social_accounts s;
INSERT OR IGNORE INTO player_cards (discord_user_id, name, country, since, updated_at)
SELECT r.discord_user_id, r.name, r.country,
    COALESCE((SELECT d.connected_at FROM discord_accounts d WHERE d.user_id = r.discord_user_id), CAST(strftime('%s','now') AS INTEGER)),
    COALESCE(r.changed_at, CAST(strftime('%s','now') AS INTEGER))
FROM ranking_choices r WHERE r.name IS NOT NULL;
```

| Column | Rule |
|---|---|
| `name` | 1 to 32 bytes, the rankings listing's rules (`skill::Listing`) |
| `since` | the account's first connection to an Ember ID; set once when the row is made, never moved by a later move |
| `worn_title`, `worn_icon` | a family id (1 to 32 bytes of `a-z`, `0-9`, `-`) or NULL; the bridge checks the shape only, so families the catalog adds later need no bridge change |
| `pins` | 0 to 4 distinct family ids, comma-separated; empty is Automatic |
| `device` | 0 not shown, 1 Pad, 2 Stick, 3 Leverless, 4 Keyboard |
| `link` | 0 not shown, 1 Wired, 2 Wi-Fi |
| `region` | a relay region code the bridge knows (`use1`, `usw1`, `euc1`, `aps1`), or NULL |
| `looking` | bits: 1 Casuals, 2 Long sets, 4 Ranked, 8 Tournament practice, 16 New to the game, 32 Happy to teach; at most two set |

Read by account (key), and by name for search (index). Rows are deleted
where the bridge forgets an account's other rows. A rollback to a bridge
before 064 leaves the table unread.

### Wire contracts

Capability `profiles` (`ember_protocol::levels::PROFILES_FEATURE`), offered
beside `points`. A build sends the new routes, selectors and the archive's
`name` only to a bridge that lists it. Requests refuse unknown fields;
answers ignore them; closed enums travel as small integers and an unknown
value reads as "not shown".

**`PUT /v1/cards/me`** (new). Player session; a Discord account (403
`no_discord_account` otherwise); bucket `cards-me:{account}` 10 per 10
minutes. Answers 204.

```json
{"listing":{"name":"Player 1","country":"DE"},"main":3,
 "worn":{"title":"matches","icon":"streak"},"pins":["rank","wins"],
 "style":{"device":2,"link":1,"region":"euc1","looking":34}}
```

| Field | Rule |
|---|---|
| `listing` | `skill::Listing` |
| `main` | 0 to 43, optional |
| `worn` | `title`, `icon`: optional family ids |
| `pins` | 0 to 4 distinct family ids |
| `style.device` | 0 to 4; `style.link` 0 to 2; `style.region` a known code or absent; `style.looking` under 64 with at most two bits |

**`POST /v1/cards/search`** (new). Player session; a Discord account; bucket
`cards-search:{account}` 20 a minute.

```json
{"name":"Play"}
{"code":"4F2A-91CX"}
{"players":[{"card":"c1.…","name":"Player 2","country":"FR","main":5,
  "badge":{"band":7,"settled":true},"since":1748736000}],"more":false}
```

| Field | Rule |
|---|---|
| request | exactly one of `name` (2 to 32 bytes after trimming) or `code` (as `request` by code reads it) |
| `players` | at most 20: names equal to `name` (any case) first, then names that begin with it, each most recently updated first; a code gives its one account |
| left out | the caller; an account that blocked the caller; an account with friends turned off is still listed (cards are public) |
| `card` | a fresh card token (`c1.`, `FIGHTER_LEVELS.md`), valid 7 days |
| `more` | true when more than 20 matched |

**`POST /v1/cards/lookup`** (extended). New selectors, sent only to a bridge
listing `profiles`: `{"request":"rq_…"}` (a request the caller received) and
`{"code":"4F2A-91CX"}`. `PlayerCard` gains, each written only when there is
something to say:

| Field | Rule |
|---|---|
| `name`, `country`, `main` | from `player_cards` |
| `badge` | `SkillBadge`, as the friends state's |
| `since` | unix seconds, the first of the month |
| `worn` | `{title, icon}`, family ids |
| `pins` | 0 to 4 family ids |
| `style` | `{device, link, region, looking}`; `device` and `link` absent at 0 |
| `versus` | `{won, lost, last_at}` from the caller's side (`pair_records`); absent before their first counted game |
| `friend` | the friendship's id, when `relation` is `friend` |
| (counters) | the awards stream's fields |

The answer stays one entry per selector, `null` as before. 16 full cards are
under 8 KB.

**`GET /v1/archive`** (extended). `name`: a side's exact name (1 to 32 bytes),
listed through the `n:{name}` key; combines with the other filters. An older
bridge ignores it, so the game always sends `q` with the same text beside it
and keeps only sets in which a side's name equals it.

**`POST /v1/friends/sync`** (1.6.0): the bridge also writes the listing's name
and flag and `main` into `player_cards` (making the row when there is none),
so 1.6.0 games keep their cards current.

### Protocol types (`server/ember/protocol/src/levels.rs`)

`PROFILES_FEATURE`, `CARDS_ME_PATH`, `SEARCH_PATH`; `CardSelector::Request`,
`CardSelector::Code`; `Worn`, `Style`, `Versus`; `PlayerCard`'s optional
fields; `CardSave` (`deny_unknown_fields`, `check`), `CardSearch` (exactly one
of the two, `check`), `SearchHit`, `SearchAnswer`; `MAX_PINS = 4`,
`MAX_SEARCH = 20`, `MAX_FAMILY_ID = 32`. Region codes from the rooms module's
list.

### Helper ops (`rust/sf4-net/src/tournament/levels.rs`)

| Op | Sends | Answer |
|---|---|---|
| `card_save` | `PUT /v1/cards/me` with `card` | `{}`; refusals worded `profiles_unavailable` (404), `no_discord_account`, `rate_limited` |
| `cards_search` | `POST /v1/cards/search` with `name` or `code` | `SearchAnswer`, checked |
| `cards_lookup` | as 1.6.0, with the two selectors | `CardAnswers` with the new fields |
| `archive_list` | the archive stream's, plus `name` | the archive stream's |

### Runtime and view

| Piece | Where | Holds or does |
|---|---|---|
| `netplay::profiles::Key` | `src/netplay/Profiles.hxx` (new) | how a profile was opened: card token, friend id, request id, code, or a name (with a country to choose by); and what the opener knew (name, country, badge, fighter, Steam ID) |
| `netplay::profiles::Style`, `Worn`, `Versus`, `Hit`, `Recent` | the same | the wire's shapes; `Device`, `Link`, `Looking` as enums over the wire's numbers, unknown read as none |
| `levels::Card` | `src/netplay/Levels.hxx` | gains `name`, `country`, `main`, `badge`, `since`, `worn`, `pins`, `style`, `versus`, `friend` (`has*` flags, as `friends::Friend` does) |
| `commands::ProfileOpen{key}`, `ProfileClose{}` | `src/netplay/RuntimeCommands.hxx` | look up one card (a name: search, then look-up of the one hit); keep it fresh 8 s after a set in a room as member cards are |
| `commands::PlayerSearch{text}` | the same | one search in flight; a newer text drops the older answer |
| card save | `src/sf4e/sf4e__NetplayRuntime__Levels.cxx` | the runtime sends `card_save` when the bundle (listing, main, worn, pins, style with the measured link and region) differs from the last one saved for this bridge and Ember ID, at most once a minute, at start and after Edit card saves |
| recent opponents | `src/sf4e/sf4e__NetplayRuntime__Social.cxx` | at each fought public game's end: the opponent's card token, name, country, fighter, room name and time; 20 newest, 7 days; saved beside the preferences, never sent |
| `ShellView::profile` | `src/ui/ShellView.hxx` | the open profile: key, `Answered<levels::Card>`, the name's hits |
| `ShellView::playerSearch` | the same | `Answered<SearchAnswer>` and the text it answers |
| `ShellView::recentOpponents` | the same | the list |
| `ShellView::profileSets` | the same | a second slot of the archive stream's browse state, so a profile's sets never replace the Archive tab's |
| preferences | `src/netplay/PlayerPreferences.hxx` | `cardDevice` (0 Automatic, 1-4, 5 Not shown), `cardShowLink`, `cardShowRegion`, `cardLooking` (bits), `cardPins` (ids, empty Automatic); validated on load, unknown values dropped |

### Privacy

| Who | Sees |
|---|---|
| Anyone with a selector (a room member's token, a friend id, a received request, a friend code, a search result, a name) | name, flag, class, main, On Ember since (month), worn and shown award families, the award counters (rated games, wins, best streak, fighters played, top frame, peak class), FP and BP with tiers, how they play, and their own relation and record with that player |
| Friends only | presence and friend since (`FRIENDS.md`), invites |
| Never served | a Discord ID or name, an Ember ID, a rating, the friends count of another, per-fighter games and wins of another, anything to a player the account blocked |
| Never leaves the PC | recent opponents, the search texts, the controller's name or model (only the device class), the measured latency and route (only Wired / Wi-Fi), anything of the region beyond its code |

A player who wants less exposure hides each chip in Edit card and blocks
whoever bothers them; a block also hides them from that player's search. A
player found by name is found because they play on Ember: there is no
opt-out of search (see Open questions).

### Older peers

| Pair | What happens |
|---|---|
| 1.6.1 game, bridge without `profiles` | no `card_save`, no search, no new selectors, no `name`; a profile opens from tokens and friend ids with 1.6.0's fields (States); Find a player is code only, as 1.6.0; name openers (rankings, archive, local replays) are not offered |
| 1.6.0 game, 1.6.1 bridge | its look-ups get the new fields and ignore them (decoders skip unknown members); its friends sync keeps its `player_cards` row's name, flag and main |
| Archive without `name` | the game's `q` and exact filter give the same rows, fewer per page |
| A later bridge's new device or looking value | read as not shown |

## Strings

Each new id goes into `locales/en.po` only, next to its area's ids; the
translation pass does the other catalogs after the merge. Reused ids are
named where they are used above (`friends.*`, `room.*`, `network.region_*`,
`replays.watch`, `card.of`).

| Key | English |
|---|---|
| `card.about` | {0} · On Ember since {1} |
| `card.since` | On Ember since {0} |
| `card.wins` | Wins |
| `card.best_streak` | Best streak |
| `card.tier_to` | {0} · {1} points to {2} |
| `card.tier_top` | {0} · Top frame |
| `card.awards` | AWARDS |
| `card.award_line` | {0} · {1}, level {2} of {3} |
| `card.no_awards` | No awards yet. The first comes with your first rated game. |
| `card.recent_sets` | RECENT SETS |
| `card.all_sets` | All sets |
| `card.set_vs` | vs {0} |
| `card.set_online` | Online |
| `card.sets_loading` | Reading sets. |
| `card.sets_none` | No archived sets under this name yet. |
| `card.sets_unavailable` | This service does not keep sets. |
| `card.sets_unreachable` | Could not reach the archive. Sets show when it answers. |
| `card.record_line` | {0} rated games · {1} wins · best streak {2} |
| `card.relation` | Friends |
| `card.not_yet` | Not yet |
| `card.they_asked` | They asked you |
| `card.how_heading` | HOW YOU PLAY |
| `card.awards_heading` | AWARDS |
| `card.pins_row` | Awards on card |
| `card.pins_auto` | Your highest |
| `card.pins_chosen` | {0} chosen |
| `card.pins_detail` | Up to four award families on your card, each at the level you reach. Your highest shows the hardest ones. |
| `card.pins_auto_row` | Show my highest |
| `card.pins_full` | Four at most. Take one off first. |
| `card.device_row` | Device |
| `card.device_auto` | Automatic ({0}) |
| `card.device_detail` | What you play on, shown on your card. Automatic follows the controller you use. |
| `card.link_row` | Connection |
| `card.link_detail` | Wired or Wi-Fi, as Ember measures it. You can hide it. |
| `card.region_row` | Region |
| `card.region_detail` | Your region, from the relay Ember picked. You can hide it. |
| `card.shown` | Shown ({0}) |
| `card.not_shown` | Not shown |
| `card.looking_row` | Looking for |
| `card.looking_detail` | Up to two, shown on your card. |
| `card.looking_full` | Two at most. Turn one off first. |
| `style.pad` | Pad |
| `style.stick` | Stick |
| `style.leverless` | Leverless |
| `style.wired` | Wired |
| `style.wifi` | Wi-Fi |
| `style.casuals` | Casuals |
| `style.long_sets` | Long sets |
| `style.ranked` | Ranked |
| `style.practice` | Tournament practice |
| `style.new` | New to the game |
| `style.teach` | Happy to teach |
| `profile.tab.sets` | Sets |
| `profile.view_sets` | View sets |
| `profile.card_of` | Player card: {0} |
| `profile.reading` | Reading {0}'s card. |
| `profile.no_card` | {0} has no card to show. |
| `profile.sets_by_name` | Their sets are still listed by name. |
| `profile.blocked` | You blocked {0}. Unblock them to see their card. |
| `profile.nobody` | No player on Ember goes by {0} now. |
| `profile.unreachable` | Could not reach the service. The card shows when it answers. |
| `profile.try_again` | Try again |
| `profile.pick_title` | Players named {0} |
| `profile.in_this_room` | In this room |
| `profile.request_sent_detail` | Cancel it in Friends, Sent. |
| `profile.sets_note` | Sets found by the name {0}. Someone else who used this name shows here too, and sets played under an earlier name do not. |
| `find.title` | Find a player |
| `find.text_row` | Name or friend code |
| `find.text_detail` | Part of a name, two letters at least, or a friend code. |
| `find.search` | Search |
| `find.send_detail` | Sends a friend request to that code. |
| `find.players` | PLAYERS · {0} |
| `find.searching` | Looking. |
| `find.none` | No player goes by that name. |
| `find.more` | More players match. Type more of the name. |
| `find.recent` | RECENT OPPONENTS |
| `find.recent_line` | Played {0} · {1} |
| `find.recent_empty` | Players you meet in public rooms show here for a week. |
| `postmatch.add_friend.detail` | Sends {0} a friend request. Your answer to the rematch stays as it is. |

Changed English: `card.edit_detail` "Your name, flag, main, title, awards and
how you play." The Friends tab's first row label moves from `friends.add` to
`find.title`; `friends.add` stays for the Settings and toast uses that still
name it, and is removed by W5 if none is left.

## Work packages

Merge order: the awards and archive streams first (063, counters, catalog,
`archive_list`, the Archive list and its filter page, `OpenArchiveFor`), and
the menu stream (Fighter select on Card). Then W1; W2; W3 and W6 side by side;
W4; W5. Each package is one branch, small commits, no file past 1,000 lines,
nothing built on Kate's PC. Lab shots use `--size 1920x1080`, `--size
1280x720` and `--size 1280x800 --scale 1.5` (narrow), each `--check
report.txt` clean.

| Package | Owns | Needs from the streams | Tests |
|---|---|---|---|
| **W1 Bridge, protocol, helper** | `server/ember/bridge/migrations/064_player_cards.sql`, `src/db/migrations.rs` (one line), `src/routes/profiles.rs` (new: `cards/me`, `cards/search`), `src/cards.rs` (selectors `request`, `code`; the new card fields), `src/routes/levels.rs`, `src/routes/{mod.rs,discovery.rs}` (routes, `profiles`), `src/routes/friends.rs` (sync writes `player_cards`), `src/routes/archive.rs` + `src/archive/listing.rs` (`name`), `tests/profiles.rs` (new), `tests/levels_migrations.rs` (064); `server/ember/protocol/src/levels.rs`; `rust/sf4-net/src/tournament/{levels.rs,mod.rs}` (`card_save`, `cards_search`, selectors); `docs/design/FIGHTER_LEVELS.md` and `FRIENDS.md` (the two privacy paragraphs) | awards: 063 and the counters in `PlayerCard` (W1 adds its fields beside them); archive: none (the listing is the bridge's) | migration from 063 backfills from `social_accounts` and `ranking_choices`; save checks (five pins, three looking bits, bad region, bad id refused); search: exact before leading, case, 20 and `more`, a blocker hidden, the caller left out, code finds one; look-up by request only for its receiver, by code; `versus` from the caller's side; `since` served as the month; no Discord ID in any answer (scan, as `tests/friends.rs`); `name` lists only that name; older bridge 404 worded `profiles_unavailable`; helper ops round trip, unknown members ignored |
| **W2 Runtime and view** | `src/netplay/Profiles.hxx` (new), `src/netplay/Levels.hxx` (`Card` fields), `src/session/SocialAnswers.{hxx,cxx}` (decode card fields, search), `src/netplay/RuntimeCommands.hxx` (`ProfileOpen`, `ProfileClose`, `PlayerSearch`), `src/sf4e/sf4e__NetplayRuntime__Levels.cxx` (look-up of one profile, search, card save cadence), `src/sf4e/sf4e__NetplayRuntime__Social.cxx` (recent opponents), `src/netplay/PlayerPreferences.hxx` + `src/sf4e/sf4e__OverlayPrefs.cxx` (card preferences), `src/ui/ShellView.hxx`, `src/sf4e/sf4e__Overlay.cxx` (mapping), the netplay and session library lines of `CMakeLists.txt` | archive: the browse state type and its second slot | `social_answers_test.cxx`: card fields, unknown enum values as none, search hits; `card_requests_test.cxx`: one profile at a time, a newer search drops the older, a name with one hit looks it up; new `profiles_test.cxx`: recent opponents (20, 7 days, one per name and visit), card save only on change and at most once a minute; preferences round trip with bad values dropped |
| **W3 The card in three sizes** | `src/ui/PlayerCard.{hxx,cxx}` (Full layout, three sizes, meters on the lines, record cells own and another's, `OtherCardView` to the Panel), `src/ui/CardParts.{hxx,cxx}` (new, so `PlayerCard.cxx` stays under 1,000: chips, award tiles and icons, set rows), `src/ui/ProfilePanel.{hxx,cxx}` (Card rows, the Sets tab's place in the strip), `src/ui/FriendsBoard.cxx` (the Panel in the friend panel), `src/ui/ApplicationShell__ShellScreens.cxx` (Members detail and `room-member` draw the Panel), `src/ui/ApplicationShell__Awards.cxx` (the preview only), `src/ui/lab/UiLabSocial.cxx`, the `sf4e_ui` source line, `locales/en.po` | awards: the family catalog, the level a family's counters reach, the family icons in their tiers; archive: set grouping and the watch path | `player_card_test.cxx`: chips from style and preferences (Automatic device by controller), shown families (Automatic order by tier then progress, a pinned family with no level reached skipped and filled, a viewer's level from the counters, never stored), record cells own and another's, meter words at 0, mid and top, the Panel's chip overflow; `ui_render_profile.hxx`: Full card at three sizes in every points state, the Panel in friends, members and awards |
| **W4 Another player's profile** | `src/ui/PlayerPage.{hxx,cxx}` (new `ScreenPanel`: `player`, `player-fighters`, `player-sets`, `player-awards`, `player-pick`, and the own `sets` tab), `src/ui/ApplicationShell__Screens.cxx` (routing), `src/ui/MenuPresentation.hxx` (titles), `src/ui/lab/UiLabSocial.cxx` (`--player` cases), `cmake/sf4e_test_targets.cmake`, `locales/en.po` | awards: the family wall drawn read only, the Friends family left out; archive: the list and filter page with a locked Player row | new `player_page_test.cxx`: the Primary by relation and presence (the table above), More's entries and their enabled states, the states' words and which tabs show, focus on open; `profile_journey_test.cxx`: open from a member popout, LB RB through four tabs, A Add friend sends `FriendRequest` with the token, Back returns to the room, a profile from a profile replaces it; `ui_render_player.hxx` (new) shots |
| **W5 Entry points and the prompt** | `src/ui/RoomPanel.cxx` + `RoomActions.cxx` (Player card opens `player`), `src/ui/FriendsPanel.{hxx,cxx}` (A on a friend with no live action, More's Player card, Find a player page with search and recent opponents), `src/ui/RankingsPanel.cxx` (A on a row), `src/ui/ReplaysPanel.cxx` (per-side Player card in the row popouts), `src/ui/ApplicationShell__PostMatch.cxx` + `PostMatchPrompt.hxx` (64 px rows, pictures, Add friend, View / C), `locales/en.po` | archive: the row popouts of Archive and Matches; the friends stream owns the prompt this round: W5 lands after it, on its file | `friends_panel_test.cxx`: an offline friend's A is Player card, More lists it first, a whole code shows Send request; `rankings_journey_test.cxx`: A opens by name; `room_post_match_answer_test.cxx`: View sends `FriendRequest`, the focus stays on Rematch, the answer and the ring are unchanged, the button by relation, nothing for a private room; a replay row's popout opens by name |
| **W6 Edit card** | `src/ui/ApplicationShell__Settings.cxx` (`CardEdit` rows, `card-pins`, `card-looking`, the Panel preview), `locales/en.po` | awards: families with a level reached, their current titles and tiers | settings journey: device steps through six values, Looking for refuses a third, pins list families only and refuse a fifth, Automatic disables the list, every row disabled in a room; `ui_render_profile.hxx` Edit card shot |

**In the game, after the merged build** (two PCs): open Profile and step five
tabs on a pad and a stick at 1920x1080 and 1280x720; set Device, Looking for
and two pinned award families and see them on the other PC's Player card in
the same colours; finish a
set in a public room and add the opponent from the prompt with View while the
ring runs, then Rematch; find the other PC by name and by code; open a
ranking row and an archive row's player; watch a recent set from a card.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| Show win rate on other players' cards? | No: others see Rated games and Wins; the percentage stays on your own card. The class is the skill mark. | One cell's key and value. |
| Add the opponent on the post-match prompt with View / C? | Yes: it is the only face-side button the prompt does not use, and the button is also a D-pad row for sticks. | A different shortcut is one line. |
| Can every player be found by name, with no opt-out? | Yes: cards are public; blocking hides a player from the blocked one's search. | A switch and a column. |
| Match sets by account now (archive columns, key and backfill) rather than by name? | Later, after this release: by name works today, and the page says what it means. | The four steps under "Sets by name"; no client change beyond the selector. |
| Give the own Profile a Sets tab (five tabs)? | Yes: All sets needs a home, and it is the same list as another player's. | Point All sets at Replays > Archive instead. |
