# Player card

The card is how a player shows who they are: their name and flag, their
class, their main fighter's portrait with its level, the fighters they play
most, and the title they wear. It is the headline of 1.6.0 with fighter
levels (`FIGHTER_LEVELS.md`) and friends (`FRIENDS.md`). The screens follow
the card canvas boards (`Main`, `Room`, `After`, `Fighters`); where the kit
makes them easier or harder is said at each.

## Whose card, where

| Card | Shown on | Built from |
|---|---|---|
| Your own, full | Profile, Card tab | preferences (name, flag, main, worn title and icon), your standing (class), `levels/me`, the friend code |
| Your own, small | Home (the L tag, as today) | the same |
| A room member's | the member's page in a public room | the room snapshot (name, flag, class, main, the fighter picked) and the card look-up by their token |
| A friend's | Profile, Friends tab, beside the list | the friends state and the card look-up by friend id |

No new bridge table or route belongs to the card: it is put together from
`levels/me`, `cards/lookup` (`FIGHTER_LEVELS.md`) and the friends state
(`FRIENDS.md`). Migrations 057 and 058 are in those documents.

## What a card shows

| Part | Own card | Another's card |
|---|---|---|
| Name, flag | yes | yes, as the room or the friend's game sends it |
| Class emblem (game's own, `RankingEmblem`) | yes, from the standing | yes, as the room shows it, or the friend's last shown class |
| Worn title on its tier plate, worn icon | yes | no: titles are not served yet (`ShellView::earnedAwards` is filled only in the UI lab), so nobody else's can be vouched for |
| Main's portrait with its level | yes | yes, while they show levels |
| The fighter picked for this set with its level | in a room | in a room |
| Three fighters: the main, then the two highest levels | yes | yes, while they show levels |
| Level progress of the main (meter, "Level 43") | yes | no |
| Counted games, win rate, fighters played (12 of 44) | yes | no |
| A fighter's games | yes | yes |
| A fighter's wins, points | Fighters tab | never |
| Friend code | yes | never |
| Network link (Cable, Wi-Fi) | yes | in a room, as today |

Decided against, against the boards:

- **The free-text line** (`Main`: "Tech the throw next time."; `Room`: "Good
  games only."). Words anyone else reads need moderation that the card has
  no path for; `tag.about` stays country and class.
- **"Take off card".** The strip is the main and the two highest levels,
  worked out; choosing the three would need a preference and a field in every
  friend's state. "Make main" stays, as the existing main pick.
- **The awards strip on others' cards.** Nothing awards titles yet.
- The `Main` board's header says "Ember ID 4F2A-91C"; that is the friend
  code (`FRIENDS.md`), labelled "Friend code". An Ember ID is never shown.

## View models

- `PlayerCardView` (`src/ui/GameMenu.hxx`) grows from name, country and the L
  tag to the whole card: `fighters` (up to three `{fighter, level, games}`,
  the main first), `picked` (the fighter of this set, -1 for none),
  `progress` (`{level, points, floor, next}` for the main; own card only),
  `facts` (own only: counted games, win rate, fighters played), `friendCode`
  (own only), `levelsShown` (false: draw no levels and say why on your own
  card). One view for every card; whoever fills it leaves out what the table
  above says they must.
- `ShellView` gains `levels` (`netplay::levels::Own`) and `memberCards`
  (token to `netplay::levels::Card`), mapped in `sf4e__Overlay.cxx` like
  `rankings`. `ApplicationShell.hxx` is 978 lines: P6 first moves `ShellView`
  into `src/ui/ShellView.hxx`, included by `ApplicationShell.hxx`, a move
  with no other change.
- `ApplicationShell::PublishPlayerCard` (`ApplicationShell.cxx`) fills the
  own card from `v.levels` as it fills the class from `v.rankings`.
- `RoomMemberCard` (`RoomPanel.cxx`) fills a member's card: the room's words
  as today, and the card look-up by `Member::card` (`FIGHTER_LEVELS.md`).

## Kit parts

Added to `V2.hxx` (P5), each used by at least two screens:

| Part | Draws | Used by |
|---|---|---|
| `v2::Level(d, at, height, level)` | the number in Ember's digits on dark ground, framed in its tier's stripe colour; returns its width | card, Fighters tab, toasts, friends list |
| `v2::LevelBand(d, tile, level)` | a dark band across a fighter picture's foot with the number centred in it, and the picture's edge in the tier's stripe colour; nothing for level 0 | card strip, room seats, cabinets, member rows, friends panel |
| `v2::Meter(d, box, part, fresh)` | a progress bar on `c.frame`, `part` in `c.accent`, the latest `fresh` of it in `c.accentLight` | card, Fighters panel, the set toast |

The boards' level chip beside a name (fighter name and number in one pill)
is not added: every place a member's level shows already draws that member's
fighter picture (`DrawSeat` in `RoomPanelTables.cxx`, `DrawMemberRow` in
`RoomCards.cxx`, the Arcade cabinets), so the level goes on the picture and
takes no width from the name. This is the main way the kit makes a board
easier than drawn: a narrow table card gives nothing up for the level.

## Screens

### Profile (new): Card, Fighters, Friends, Titles, Icons

One screen strip under the header (`GameMenu::tabs`): `card`, `fighters`,
`friends` (`FRIENDS.md`), and the awards wall's own two screens, `awards`
and `awards-icons`, which today form a strip of their own
(`ApplicationShell__Awards.cxx`, `Dress`). They join this one, so the awards
keep their rows and Profile has one strip, not tabs inside tabs. The header's
eyebrow is "Profile · {name}". LB and RB step the strip (`StepTab`).

Home's card opens Profile on Card (today its Select opens the awards wall,
`GameMenu::DrawHomePane`, `GameMenuDraw.cxx`). Settings, Player, Profile
(name, main, Steam profile) stays where it is and is what Edit card opens.

The panel is a new `ProfilePanel` (`src/ui/ProfilePanel.{hxx,cxx}`, a
`ScreenPanel`) that owns its own state (focused fighter, last levels seen),
so `ApplicationShell` gains one panel line and no members.

### Card tab

Board (`Body`), the `Main` board's order: the title on its plate, the hero
(the main drawn as Settings, Room layout, Pictures says: portrait or figure,
`SelectionArt::Portrait` or `Character`, large), with the level plate
(`v2::Level`, the fighter, "1,284 games") at its foot left and the class
plate (emblem, "Class", letters) at its foot right; the name line (flag,
name, link); the main's meter and "Level 43"; the facts row; the fighters
strip.

Navigation: the strip's three tiles are the rows (`PressEntry`, placed);
Select on one opens Fighters focused on it. X is Edit card
(`ShortcutHints({{MenuInput::Options, …}})`), opening Settings, Player,
Profile. With friends offered, a fourth row, "Friend code", under the strip:
Select shows the code and offers New code (`FRIENDS.md`). Back is Home.
Under 900 px (`v2::Narrow`) the plates stack under the hero and the facts
take two lines, as the L tag's do.

Without a Discord account the hero says "Connect Discord in Settings, Online
to earn levels" in place of the level plate; with a bridge that does not
count levels, "This service does not count levels yet".

### Fighters tab

Board: on the left the played fighters, highest level first (then most
games), two columns, each a row with its portrait tile (`LevelBand`), name,
games and a `Meter`; one info row after them, "32 fighters not played yet.
Each starts at level 1 with its first rated public game." On the right the
focused fighter's panel (`v2::Panel`, titled with the fighter, "Main" chip
when it is): a large tile, "Level 42", the meter, "Level 43 is about 14 games
away" (points to go over this fighter's own points per game), Games, Won,
Since, "How a level is earned", the five frames 1, 13, 25, 37, 49, and, when
`shown` is false, "Others do not see your levels while you do not take part
in ranking."

Navigation: the grid's rows are placed; the D-pad moves by geometry; the
panel follows the focus and holds nothing focusable. Select is Make main
(hint `menu.hint.save_main`, the main page's own words; it saves like
`MainCharacters` in `ApplicationShell__Settings.cxx`). The board's Make main
and Take off card buttons are therefore not drawn: a button in a panel that
follows the focus would be reached by moving the focus off the fighter it
belongs to. Under 900 px one column, and the panel's words go under the
board (`v2::Words`).

### A room member's card

The member popout (`RoomPanel.cxx`, the `row.choices` of a member row) gains
"Player card", which opens the member's page (`room-member`) as Kick and
Make host do, and "Add friend" (`FRIENDS.md`). The page draws the small card
above its rows: tile with the picked fighter's `LevelBand` (else the main's),
flag, name, "Cammy · class A · 612 games", and the other two fighters with
their levels. Its rows are today's (mute, Steam profile, kick, make host)
plus Add friend and Block (`FRIENDS.md`).

The `Room` board's hint bar ("X Add friend", "Y Chat") cannot be: X, Y and
View are the room's own shortcuts on every room screen (Fighter, Table
options, Chat; `ApplicationShell::RoomShortcut`, `RoomActions.cxx`). Add
friend lives in the popout and on the member's page instead.

### Levels on the room board

- A seat (`DrawSeat`): `LevelBand` on the seat's picture, for the fighter
  picked (`Member::fighter`), else the main. Nothing for a player without a
  card, one who does not show levels, or a private room.
- The Members list (`DrawMemberRow`) and Arcade cabinets
  (`RoomPanelArcade.cxx`): the same on their pictures.
- The match HUD: no level (`FIGHTER_LEVELS.md`, Open questions).

### After a set

There is no "Set over" board: a Classic set returns to the board, and the
Arcade decision is answered over the WIN (`ApplicationShell__PostMatch.cxx`),
whose ten seconds end before the bridge has counted the set (it polls rooms
every 5 seconds). So the `After` board becomes two toasts, shown over Ember's
menus only, as every toast is (`ApplicationShell__Toasts.cxx`):

- **The set**: when `levels/me` after the set differs from the one before,
  one toast per fighter that gained: the fighter's tile with its level, a
  `Meter` whose fresh part is this set, "3 games this set, 2 won".
- **Level up**: "Cammy reached level 25", with the frame's new tier named
  when the level is 13, 25, 37 or 49 ("Silver frame"). It goes to
  Notifications and speech like any toast.

## Strings

New keys, each added to all fourteen catalogs in one commit, as the newest
string commits did (`716a08d25` touched every `locales/*.po`):

| Key | English |
|---|---|
| `profile.eyebrow` | Profile · {0} |
| `profile.tab.card` | Card |
| `profile.tab.fighters` | Fighters |
| `card.edit` | Edit card |
| `card.class` | Class |
| `level.word` | Level |
| `level.value` | Level {0} |
| `level.games` | {0} games |
| `level.games_away` | Level {0} is about {1} games away |
| `level.top` | Highest level |
| `level.reached` | {0} reached level {1} |
| `level.frame` | {0} frame |
| `level.set` | {0} games this set, {1} won |
| `level.hidden` | Others do not see your levels while you do not take part in ranking. |
| `level.no_account` | Connect Discord in Settings, Online to earn levels. |
| `level.unavailable` | This service does not count levels yet. |
| `card.counted_games` | Rated games |
| `card.win_rate` | Win rate |
| `card.fighters_played` | Fighters played |
| `card.of` | {0} of {1} |
| `card.member_line` | {0} · class {1} · {2} games |
| `fighters.played` | {0} of {1} fighters played in rated public games |
| `fighters.unplayed` | {0} fighters not played yet. Each starts at level 1 with its first rated public game. |
| `fighters.how` | How a level is earned |
| `fighters.how_detail` | Every rated public game with a fighter counts, and a win counts double. It never goes down and has nothing to do with your class. Past eight games against the same player in one sitting, games stop counting. |
| `fighters.won` | Won |
| `fighters.since` | Since |
| `fighters.main` | Main |
| `room.member_card` | Player card |

Tier names come from the awards' own (`v2::TierName`).

## Work packages

Numbering is shared with `FIGHTER_LEVELS.md`, which has P0 to P5.

| Package | Owns | Depends | Tests | Only in the game |
|---|---|---|---|---|
| P6 Profile and own card | `src/ui/ShellView.hxx` (moved out), `src/ui/{ProfilePanel.hxx,ProfilePanel.cxx,PlayerCard.hxx,PlayerCard.cxx}`, `src/ui/GameMenu.hxx` (`PlayerCardView`), `src/ui/GameMenuDraw.cxx` (Home card), `src/ui/ApplicationShell.{hxx,cxx}` (panel line, `PublishPlayerCard`), `src/ui/ApplicationShell__Awards.cxx` (strip), `src/ui/ApplicationShell__Toasts.cxx` (set and level toasts), `src/sf4e/sf4e__Overlay.cxx` (view mapping), `src/ui/lab/*` (states: no account, unavailable, fresh, 60, hidden), the `sf4e_ui` source line, `locales/*.po` | P4, P5 | `uilab-v2-check.sh` on card, fighters at four sizes; toast choice from two `Own` values; strip order | the card's hero art from a real install; LB/RB through all five tabs on a pad and a stick |
| P7 Room | `src/ui/{RoomPanelTables.cxx,RoomPanelArcade.cxx,RoomCards.hxx,RoomCards.cxx,RoomPanel.cxx,RoomActions.cxx}`, `locales/*.po` | P6 | lab room cases with levels on seats, cabinets, members; a private room's snapshot with `card` draws none | levels on seats in a live public room, and fresh after a set |

**If 1.6.0 is cut short**: P6 alone on top of the levels slice of
`FIGHTER_LEVELS.md` shows the own card and the Fighters tab. P7 waits for
the card token.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| A free-text line on the card? | Not in 1.6.0. | A column, the chat filter on the bridge, and reports. |
| Let players choose the strip's three fighters? | No: main plus the two highest. | A preference and one field in the friends state. |
| Home's card opens Profile, not the awards wall? | Yes: the awards are a Profile tab now. | One row id. |
| Show a fighter's games on others' cards? | Yes, the level and the games; never wins. | A field left out of the look-up. |
| The hero: portrait or figure? | Follow Settings, Room layout, Pictures. | None. |
