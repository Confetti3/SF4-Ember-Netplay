# Profile (1.6.0)

Profile is where a player sees their card, their fighters, their friends and
their awards, and changes them. This document replaces the screen sections of
`PLAYER_CARD.md` (Screens: Profile, Card tab, Fighters tab, A room member's
card) and `FRIENDS.md` (Client: Friends tab, In a room, Toasts, Settings,
Online). Their data, wire and privacy sections stand. The static mockup is
`Desktop/sf4/mockups/profile-160/index.html` (outside the repository).

Data the screens read, built on other branches and taken as given:

| Data | From | Holds |
|---|---|---|
| `ShellView::levels` | `netplay::levels::Own` (`wip/160-bp-rust`) | `account {points, games, wins, tier, floor, next, since}`; per fighter the same; `kind` counted, no account, unavailable |
| `ShellView::memberCards` | `netplay::levels::Card` | `account {points, tier}`, `fighters [{fighter, points, tier}]`, `relation` (none, friend, sent, received, blocked) |
| `ShellView::friends` | `netplay::friends::State` (`wip/160-friends-rt`) | code, appear offline, friends with presence, incoming, outgoing, invites, blocked; loading, no account, unavailable, the last refusal |
| friends commands | `RuntimeCommands.hxx` (`wip/160-friends-rt`) | sync; request by card or code; accept; decline (+block); cancel; remove (+block); block; unblock; new code; appear offline; invite; dismiss; join; watch |

## Points: the SF4 line

Points are drawn as SF4's versus screen draws PP and BP: the number, a space,
the two-letter label, on one line, white with a dark outline, the label's
capitals as tall as the digits. No plate, box, ground or coloured frame
(`v2::Level(d, at, height, points, labelId)` after `wip/160-bp-rust`). Two
lines stack under a name, numbers right-aligned to the wider one so the labels
line up:

```
Player 1
1986 FP      the main's points (another player: the fighter they play now)
4012 BP      the account's points
```

| Where | Lines | Digit height (1080 / 720) |
|---|---|---|
| Card tab, under the name | FP of the main, BP | 30 / 24 |
| Home's card (L tag), under the about line | FP of the main, BP | 22 / 20 |
| Another player's card (room member page, friend panel, Awards preview) | FP of the current fighter, BP | 26 / 22 |
| A fighter picture's foot (`v2::LevelBand`: strip, Fighters tiles, room seats) | `1986 FP` on a short dark shade | fits the picture |

**Where the tier shows.** The tier never colours a number. It shows in two
places only:

1. As the 2 px edge of a fighter's picture wherever that fighter's FP is drawn
   on or beside it (the hero, the strip, Fighters tiles, the friend panel, the
   member page, room seats), in `v2::TierColor(...).stripe`. A fighter at 0
   (not played) has the plain `c.border` edge.
2. In words: the Card tab's progress rows ("Ryu · Gold", "Account · Copper")
   and the Fighters panel's five frames. BP's tier is shown nowhere else.

## Structure

One tab strip, four tabs (today five: Titles and Icons merge into Awards).
LB/RB (PgUp/PgDn) step and wrap. Eyebrow "Profile · {name}".

| Screen id | Tab or page | Title | Owner | Opened from |
|---|---|---|---|---|
| `card` | tab 1 | Card | `ProfilePanel` + `PlayerCard.cxx` | Home's card, Home's Profile row |
| `fighters` | tab 2 | Fighters | `ProfilePanel` | a strip tile (focused on it), LB/RB |
| `friends` | tab 3 | Friends · {requests} | `FriendsPanel` (new) | LB/RB, Home's Profile row when requests wait, a request toast |
| `awards` | tab 4 | Awards | `ApplicationShell__Awards.cxx` | LB/RB, Edit card's Title and Icon rows |
| `card-edit` | page over Card | Edit card | Settings `PageOf` (`CardEdit` builder) | Y on Card, the Edit card button |
| `friends-add` | page over Friends | Add a friend | `FriendsPanel` | X on Friends, the Add a friend row |
| `friends-sent` | page over Friends | Sent | `FriendsPanel` | the Sent row |
| `friends-blocked` | page over Friends | Blocked | `FriendsPanel` | the Blocked row |
| `room-member` | room page (exists) | the member's name | `RoomPanel` | a member's popout, Player card |

`awards-icons` goes. Settings, Player, Profile stays as it is and edits the
same preferences as `card-edit`.

## Card tab

### Layout, 1920x1080 (scale 1, content 1840 wide, body about 838 tall)

| Part | Box | Contents |
|---|---|---|
| Hero | left, 420 x 420 | the main large (`SelectionArt` portrait or figure, as Settings, Room layout, Pictures); 2 px edge in the main's tier; a 56 px shade across the foot with "MAIN" (eyebrow, muted) and the fighter's name (18 px). No number on the hero. |
| Edit card | under the hero, 420 x 48, 16 gap | `v2::Button` Secondary, Y glyph and "Edit card" |
| Title row | right column (x +452, at most 960 wide so the meters stay near their words), 34 tall | worn title on `v2::TitlePlate`; worn icon (40 px, tier stroke) at the row's right end. No title worn: the row is left out. |
| Name | 44 tall | flag, name (36 px heading), class emblem (`DrawBadge`, 32 px) after the name |
| About | 22 tall | "{country} · class {x}" (today's `tag.about`), 15 px muted |
| Points lines | 2 x 30 + 8 | the SF4 line: main's FP, then BP |
| Progress | heading + 2 rows of 40 | "PROGRESS". Each row: label 200 wide ("Ryu · Gold", "Account · Copper"), `v2::Meter` 10 tall, then "514 points to Ember" right-aligned (220 wide); "Top frame" at Ember; "Not played yet" with an empty meter for a main with no points |
| Facts | 52 | `v2::FactCells`, 3 across: Rated games, Win rate, Fighters played ("12 of 44") |
| Top fighters | heading + tiles 150 x 202 | "TOP FIGHTERS": the main, then the two highest others by FP; picture with `LevelBand`, name, games |

Total right column about 650: fits with room to spare.

### 1280x720 (scale 1, content 1232 wide, body about 500 tall)

Hero 300 x 300 with Edit card under it (356 in all); right column 908 wide.
The facts stand beside the points lines (lines on the left 260, facts cells on
the right) so the column is about 474 tall; digits 24; progress rows 32;
strip tiles 112 x 160. Rule: when the body is under 620 logical px tall, use
these sizes.

### Narrow (`v2::Narrow`, under 900 logical px)

One column, scrolling: a band 200 tall with the hero (200 x 200) on the left
and title, name, about and the points lines on its right (under 560 wide, they
go under the hero); progress; facts 2 across (as today); the three tiles
across the width; Edit card full width. The focused tile scrolls into view.

### States

The card is always whole. Every state draws the hero, name, both points lines,
progress, facts and strip; only the numbers and one line under the facts
change.

| State | Numbers | Line under the facts | Extra row |
|---|---|---|---|
| Counted, played | the answer's | none | - |
| Counted, nothing played (new player) | `0 FP`, `0 BP`, empty meters ("Not played yet"; Account shows its `next`: "500 points to Copper"), facts 0, 0%, 0 of 44; strip = the main at 0 FP | "Play a rated public game to start earning points." (`level.none`) | - |
| No Discord account | 0, empty meters | "Connect Discord in Settings, Online to earn points." | Connect Discord (Primary, under the strip) |
| Service without points | 0 | "This service does not count points yet." | - |
| Loading, nothing read before | 0, digits at 40 % alpha | "Reading your points." | - |
| Loading with an earlier answer | the earlier answer | none | - |
| Unreachable (read failed, nothing before) | 0 | "Could not reach the service. Your points show when it answers." (new) | - |
| No main chosen | hero ground `c.frame` with "Choose a main" Primary button inside it; FP line `0 FP` | - | the button is a row; opens `main-character` |

### Navigation

Rows in order: the three strip tiles, Edit card, Connect Discord (when shown).
The D-pad moves by geometry (placed rows, `PressEntry`).

| Input (pad / keys) | On a strip tile | On Edit card | On Connect Discord |
|---|---|---|---|
| A / Enter | Fighters, focused on that fighter | opens `card-edit` | opens `discord-connect` |
| B / Esc | Home | Home | Home |
| Y / T | opens `card-edit` | same | same |
| LB RB / PgUp PgDn | tabs | tabs | tabs |

Hint bar: `A Fighters` (or `A Edit card`), `B Back`, `Y Edit card`, `LB RB Pages`.

### Edit card (`card-edit`)

A list page with the detail pane on the right; the pane draws the player's
card as others see it (the other-card part below, with the player's own
values), so a change shows at once.

| Row | Value | Select | Builder reused |
|---|---|---|---|
| Name | the name | on-screen keyboard (`TextRow`) | Settings `Profile` |
| Flag | country or Automatic | country picker | Settings `Country` |
| Main | fighter name | `main-character` page | Settings `Profile` |
| Title | worn title or "None" | Awards tab, the worn award focused | new |
| Icon | worn icon's name or "None" | Awards tab, the worn award focused | new |
| Show my Steam profile | switch | flips | Settings `Profile` |

In a room the rows are disabled with `profile.leave_room_to_edit`, as today.
Hint bar: `A` (the row's word), `B Back` (to Card).

## Fighters tab

Every fighter, so the main is chosen here. Two groups under section headings:
"PLAYED · 12" (most FP first, then the lower id) and "NOT PLAYED YET · 32"
(roster order, `selection::RosterDisplayOrder`, pictures at 50 % alpha, `0 FP`).
The main is first in its group and carries a "Main" chip.

| Size | Grid | Panel |
|---|---|---|
| 1920x1080 | tiles 120 x 168 (picture 120, `LevelBand`; name 14 px under), gap 12, 10 across in 1416 | right, 400 wide |
| 1280x720 | the same tiles, 6 across in 808 | right, 400 wide; the frames wrap |
| Narrow | 4 across, full width | a 120 tall band above the grid: picture 96, FP line, meter, "514 points to Ember"; the rest as an info row after the grid |

The panel follows the focus and holds nothing focusable (today's panel):
titled with the fighter and the "Main" chip; picture 112 with its tier edge;
the FP line (26); meter; "514 points to Ember" or "Top frame"; Games, Won,
Since; "How points are earned" and its paragraph; the five frames by tier
name. Last line: "A  Save main" glyph and words, or "Your main" when it is.

| State | Shows |
|---|---|
| Counted | both groups |
| Nothing played, no account, unavailable, unreachable | all 44 under "NOT PLAYED YET · 44" at `0 FP`, and above the grid the Card tab's line for that state (and Connect Discord when no account) |
| Loading | the grid with digits at 40 % alpha |
| In a room | tiles focusable; Select disabled, detail `profile.leave_room_to_edit` |

| Input | Does |
|---|---|
| A / Enter | Save main (`menu.hint.save_main`; saves as `MainCharacters` does) |
| B / Esc | Home |
| LB RB | tabs |

## Friends tab

### Layout, 1920x1080

List on the left (1296 wide), the selected entry's panel on the right (520).

List, top to bottom (each a placed row, `v2::CardGround`):

| Row | Height | Contents | A does | A's hint |
|---|---|---|---|---|
| Add a friend | 56 | "Add a friend"; value "Your code 4F2A-91CX" (muted, code in heading font) | opens `friends-add` | Open |
| "REQUESTS · 2" heading, then a request each | 64 | flag, name (16 heading), "Played you 6 games, last 1 d ago" or "You have not played each other yet" (14 muted); value "Accept" (accent) | accept | Accept |
| "IN A ROOM · 3", friends | 64 | presence mark, main's picture 48 (tier edge), flag, name, class emblem; status ("Playing at table 1 · Player 1's room"); value "Join" when joinable, else "Invite" when you have a room to offer, else none | the value's action, else More | Join / Invite / More |
| "ONLINE · 2", friends | 64 | as above; status "At the menus", "Playing offline", "On another version" | Invite when you can, else More | Invite / More |
| "OFFLINE · 4", friends | 64 | dashed mark; status "Last online 3 h ago"; no value | More | More |
| Sent (3) | 48 | value chevron | opens `friends-sent` | Open |
| Blocked (1) | 48 | value chevron | opens `friends-blocked` | Open |
| Appear offline | 48 | switch | flips | Change |

Groups with no entries are left out. Order inside a group: joinable first,
then by name (in a room); by name (online); most recently seen first (offline).
Presence marks differ in shape: filled disc in a room, ring online, dashed ring
offline, all `c.ready` except offline (`c.muted`).

Panel (`v2::Panel`, titled with the name), for the focused row:

| Focused | Panel |
|---|---|
| A friend | the other-card part (main's picture 112, name, flag, class, title when served, `FP` of the main and `BP` from the card look-up by friend id); the presence line with its mark; facts: "You and Player 3" 14 - 9, Last played, Friend since; buttons: Primary (A glyph) the row's action, Secondary (Y glyph) "More". The buttons take the pointer; the pad uses A and Y from the row. |
| A request | name, flag, the played line; Primary "Accept", Secondary "More". No points: a receiver sees nothing else until they accept (`FRIENDS.md`, Privacy). |
| Add a friend, Sent, Blocked, Appear offline | the row's detail (`v2::Words`) |

**More** (Y, or the Secondary button) opens the row's actions as a popout at
the row (`MenuEntry::popout`, the room member's pattern). Each is listed even
when it cannot be done, with the reason in its detail.

| Row | More |
|---|---|
| Request | Accept, Decline, Decline and block |
| Friend | Join their room, Watch (fighting in a joinable room), Invite to my room, Remove friend, Remove and block |

Decline, Remove and both blocks ask first (`ConfirmRow`). Invite while
appearing offline says `friends.invite_reveals` in its detail.

### 1280x720 and narrow

| Size | Change |
|---|---|
| 1280x720 | list 808, panel 400 (picture 80, digits 22); rows keep 64; the list scrolls |
| Narrow | the list alone, full width; no panel; the focused friend's presence line, record and both points lines go in `v2::Words` under the list; A and Y as wide |

### States

| State | Shows |
|---|---|
| No Discord account | one card: "Friends need a Discord account. Connect it in Settings, Online." and Connect Discord (Primary, focused) |
| Service without friends | one card: "This service does not offer friends yet." |
| Loading, never read | Add a friend row, then an info row "Reading your friends." |
| Read, no friends, no requests | Add a friend row (focused), then info row "Add friends from a public room: open a member's Player card. Or send them your friend code." |
| Unreachable (sync failed) | the last state kept; header chip "Not updated" (muted); actions disabled, detail "Could not reach the service. Friends show when it answers." |
| Appearing offline | header chip "Appearing offline" (muted); the switch on |
| An action in flight | its row's value "..." and its actions disabled until the state comes back |
| A refusal | the refusal's words as a toast (`notice_`), e.g. "No player has that friend code." |

The tab's label is "Friends · 2" while requests wait (`friends.tab_count`),
else "Friends".

### Navigation

| Input | Does |
|---|---|
| A / Enter | the row's action (table above) |
| Y / T | More (popout) on a request or friend |
| X / F | Add a friend (`friends-add`) |
| Delete | on a friend: Remove friend (asks) |
| B / Esc | Home |
| LB RB | tabs |

Hint bar: `A {row hint}`, `Y More` (on requests and friends), `X Add a friend`,
`B Back`, `LB RB Pages`.

### Pages

| Page | Rows | Notes |
|---|---|---|
| `friends-add` | "Your friend code" (info, value the code, 22 px heading font); "New code" (`ConfirmRow`, detail "The old code stops working. One new code a day."); "Their friend code" (`TextRow`, 9 chars, on-screen keyboard, any case, dash optional); "Send request" (Primary, enabled at 8 symbols) | after sending: notice "Request sent to {code}." and Back to Friends |
| `friends-sent` | each outgoing request: label, "Sent 2 d ago"; Select asks "Cancel request" | hidden requests look the same (`FRIENDS.md`) |
| `friends-blocked` | each block: name, date; Select asks "Unblock" | |

## Another player's card

One part, `DrawOtherCard(d, box, OtherCardView)` in `PlayerCard.cxx`, used by
the room member's page, the friend panel and Edit card's preview (and the
Awards tab's "As others see it", which replaces today's L-tag preview there).

| Shows | Source |
|---|---|
| Picture of the current fighter, 2 px tier edge (`c.border` at 0) | room: `Member::fighter`, else their main; friend: their main; own preview: the main |
| Flag, name, class emblem | room snapshot / friends state / preferences and standing |
| Title on its plate | only when the card carries one: titles are not served yet, so others' cards show none (see Open questions) |
| `{FP} FP` | the current fighter's points from the card (0 when it is not listed) |
| `{BP} BP` | `account.points` (0 when the card has none) |
| Relation chip (room page only) | "Friends" (`c.ready`), "Request sent" (muted) |

Nothing else: no games, win rate, top three or meter. A member without a card
(no Discord) in a public room shows `0 FP` and `0 BP` and offers no Add
friend; a private room shows no points lines (unchanged rule).

### In a public room

The member popout (`RoomPanel.cxx`, a member row's `choices`) becomes:
**Player card**, **Add friend** (by relation, below), Mute, Steam profile,
Kick, Make host. Block is on the page only.

The member's page (`room-member`) draws the other-card part (full width of the
list, 160 tall) above its rows: the relation row, Mute, Steam profile, Kick,
Make host, Block (Danger, asks).

| `relation` | Relation row | Select |
|---|---|---|
| none | Add friend | `request` with the card token and the member's room name as label |
| sent | Request sent (disabled) | - |
| received | Accept request | `request` (the bridge accepts a mutual request) |
| friend | Friends (disabled) | - |
| blocked | Blocked (disabled; detail "Unblock them in Profile, Friends, Blocked.") | - |
| no token (no card, private room) | left out | - |

A friend in the same room needs no Join or Invite; those are on the Friends
tab. Hint bar on the page: `A {row}`, `B Back`.

## Awards tab (Titles and Icons merged)

The wall as today (`DrawAwardsBoard`), one tile per award (11), each showing
its icon and title. Left column: "YOUR CARD · AS OTHERS SEE IT" with the
other-card part (not the L tag), then "WHERE IT SHOWS" with the S and XS tags.
Select on an earned award opens a popout: **Wear title**, **Wear icon**,
**Wear both** (the one already worn disabled, detail "On your card."). A locked
award is info: its detail says how to earn it. Header chip "Earned 6 of 11".

## Home

| Part | Change |
|---|---|
| Profile row (`profile`) | opens `card` (today it opens Settings' Profile page); detail "Your card, fighters, friends and awards."; value "Requests: 2" in the accent while requests wait, and then Select opens `friends` |
| The card in the pane (`card` row, L tag) | gains the two points lines under the about line, or under the name when there is none (`TagView::points`, own card only); Select opens `card` as today |
| Navigation | the card beside the list is a column of its own (`MenuNavigation::SetAside`): Right from any row reaches it, Left goes back to the list's top, Up and Down on it keep it; the list's Up and Down pass over it, so Down from the last row goes round to the first (a fresh press) instead of across to the card. Narrow, where the card is the list's last card, it is one of the list's rows |

## Toasts

Over Ember's menus only (as every toast). A = first action, B = second.

| Toast | Words | A | B |
|---|---|---|---|
| Request | "Player 9 wants to be friends" / "Played you 6 games, last 1 d ago" | Accept (sends `accept`; stays in place) | Later (the request stays in the list) |
| Invite | "Player 3 invites you to their room" / their room's name | Join (the runtime's join; asks first when in a room or a game) | Dismiss (sends `dismiss`) |
| Refusal or done | the words ("Request sent to Player 4.", "Invite sent to Player 3.") | - | - |

`ToastAction` gains an optional command submitted as it is, without opening a
screen; View and the request's other answers are in Notifications and the
Friends tab. No toast when a friend comes online.

## Settings, Online: Friends panel

A third panel on `tab-online` after Discord and Rankings, with a new line
icon `Users` (Tabler `users`, added to `LineIcons.inc` as the others were):

| Row | Value | Select |
|---|---|---|
| Appear offline | switch | flips (`appear_offline`) |
| Friend code | the code | asks "New code", detail as on `friends-add` |
| Turn off friends | - | Danger, asks; detail "Deletes your friends, requests, blocks and friend code." |

Without a Discord account or on a service without friends, the rows are
disabled with that state's words.

## Hint bar, every screen

| Screen | A | B | X | Y | LB RB |
|---|---|---|---|---|---|
| Card | Fighters / Edit card / Connect Discord | Back | - | Edit card | Pages |
| Edit card | the row's word | Back | - | - | - |
| Fighters | Save main | Back | - | - | Pages |
| Friends | Accept / Join / Invite / More / Open / Change | Back | Add a friend | More | Pages |
| Add a friend, Sent, Blocked | the row's word | Back | - | - | - |
| Awards | Wear (earned) | Back | - | - | Pages |
| Room member page | the row's word | Back | - | - | - |

Keys: A Enter, B Esc, X F, Y T, LB RB PgUp PgDn, Delete. The mouse presses
every row, button and hint.

## Strings

New, each in all fourteen catalogs in one commit per package, English copied
where a translation is not ready (as `716a08d25` and `a31c8b66b` did). Keys
marked [rt] may already exist on `wip/160-friends-rt`; keep its id and words.
`points.account_label` (BP) comes from `wip/160-bp-rust`.

| Key | English |
|---|---|
| `profile.tab.friends` | Friends |
| `friends.tab_count` | Friends · {0} |
| `home.profile_requests` | Requests: {0} |
| `card.account` | Account |
| `card.progress` | PROGRESS |
| `card.top_fighters` | TOP FIGHTERS |
| `card.choose_main` | Choose a main |
| `card.edit_detail` | Your name, flag, main, title and icon. |
| `card.title_row` | Title |
| `card.icon_row` | Icon |
| `card.none` | None |
| `card.award_detail` | Opens Awards. Select an earned award there to wear it. |
| `level.not_played` | Not played yet |
| `level.unreachable` | Could not reach the service. Your points show when it answers. |
| `fighters.played_heading` | PLAYED · {0} |
| `fighters.unplayed_heading` | NOT PLAYED YET · {0} |
| `fighters.your_main` | Your main |
| `friends.requests_heading` | REQUESTS · {0} |
| `friends.in_room` | IN A ROOM · {0} |
| `friends.online` | ONLINE · {0} |
| `friends.offline` | OFFLINE · {0} |
| `friends.playing_table` | Playing at table {0} · {1} |
| `friends.watching_table` | Watching table {0} · {1} |
| `friends.waiting_room` | In {0} |
| `friends.training_room` | In Training · {0} |
| `friends.private_room` | In a private room |
| `friends.menus` | At the menus |
| `friends.offline_play` | Playing offline |
| `friends.last_online` | Last online {0} |
| `friends.hours_ago` | {0} h ago |
| `friends.days_ago` | {0} d ago |
| `friends.other_version` | On another version |
| `friends.join` | Join |
| `friends.join_room` | Join their room |
| `friends.watch` | Watch |
| `friends.invite` | Invite |
| `friends.invite_room` | Invite to my room |
| `friends.more` | More |
| `friends.accept` | Accept |
| `friends.accept_member` | Accept request |
| `friends.decline` | Decline |
| `friends.decline_block` | Decline and block |
| `friends.later` | Later |
| `friends.remove` | Remove friend |
| `friends.remove_block` | Remove and block |
| `friends.block` | Block |
| `friends.unblock` | Unblock |
| `friends.cancel_request` | Cancel request |
| `friends.add` | Add a friend |
| `friends.add_member` | Add friend |
| `friends.request_sent` | Request sent |
| `friends.are_friends` | Friends |
| `friends.blocked_member` | Blocked |
| `friends.blocked_member_detail` | Unblock them in Profile, Friends, Blocked. |
| `friends.played_you` | Played you {0} games, last {1} |
| `friends.not_played` | You have not played each other yet. |
| `friends.record` | You and {0} |
| `friends.last_played` | Last played |
| `friends.since` | Friend since |
| `friends.your_code` | Your code {0} |
| `friends.code` | Friend code |
| `friends.code_detail` | Give this code to a player so they can add you. |
| `friends.their_code` | Their friend code |
| `friends.new_code` | New code |
| `friends.new_code_detail` | The old code stops working. One new code a day. |
| `friends.send` | Send request |
| `friends.sent` | Sent ({0}) |
| `friends.sent_ago` | Sent {0} |
| `friends.blocked` | Blocked ({0}) |
| `friends.appear_offline` | Appear offline |
| `friends.appear_offline_detail` | Friends see you as offline. You still see them and can still invite. |
| `friends.appearing_offline` | Appearing offline |
| `friends.invite_reveals` | Your invite shows this friend where you are. |
| `friends.turn_off` | Turn off friends |
| `friends.turn_off_detail` | Deletes your friends, requests, blocks and friend code. |
| `friends.no_account` | Friends need a Discord account. Connect it in Settings, Online. |
| `friends.unavailable` | This service does not offer friends yet. |
| `friends.unreachable` | Could not reach the service. Friends show when it answers. |
| `friends.not_updated` | Not updated |
| `friends.loading` | Reading your friends. |
| `friends.empty` | Add friends from a public room: open a member's Player card. Or send them your friend code. |
| `friends.toast_request` | {0} wants to be friends |
| `friends.toast_invite` | {0} invites you to their room |
| `friends.request_sent_to` | Request sent to {0}. |
| `friends.invite_sent` | Invite sent to {0}. |
| `friends.unknown_code` [rt] | No player has that friend code. |
| `friends.full` [rt] | That friend list is full. |
| `friends.request_limit` [rt] | You have sent enough requests for today. |
| `friends.code_too_soon` [rt] | You can make one new code a day. |
| `friends.card_expired` [rt] | That card has expired. Meet them in a room again. |
| `friends.own_card` [rt] | That is your own card. |
| `friends.invite_unavailable` [rt] | That friend cannot be invited now. |
| `room.member_card` | Player card |
| `awards.wear_title` | Wear title |
| `awards.wear_icon` | Wear icon |
| `awards.wear_both` | Wear both |

Changed English: `home.profile_detail` "Your card, fighters, friends and
awards."; `awards.card_detail` "Your card as others see it. Select opens
Profile."

No longer used, removed in the package that stops using them: `awards.titles`,
`awards.icons` (tab labels), `fighters.unplayed` (now a heading).

## Implementation packages

Both data branches (`wip/160-bp-rust`, `wip/160-friends-rt`) merge first. Each
package is one branch, small commits, no file past 1,000 lines. Lab shots use
`--size 1920x1080`, `--size 1280x720` and `--size 1280x800 --scale 1.5`
(narrow), each with `--check report.txt` reporting nothing.

| Package | Owns | Depends | UI lab | Tests |
|---|---|---|---|---|
| **PR1 Card and the strip** | `src/ui/ProfilePanel.{hxx,cxx}` (four tabs, Y opens `card-edit`, Home's row target), `src/ui/PlayerCard.{hxx,cxx}` (Card layout, three sizes, progress rows, states, `DrawOtherCard` and `OtherCardView`), `src/ui/GameMenu.hxx` (`PlayerCardView::account`), `src/ui/V2.hxx` + `V2.cxx` (`TagView::points` on the L tag), `src/ui/GameMenuDraw.cxx` (nothing beyond the tag), `src/ui/ApplicationShell.cxx` (Home's Profile row: target, value), `src/ui/ApplicationShell__Settings.cxx` (`card-edit` page, `CardEdit` builder), `src/ui/MenuPresentation.hxx` (titles), `locales/*.po` | bp-rust | `--profile played\|fresh\|noaccount\|unavailable\|loading\|unreachable --screen card`; `--screen card-edit`; `--screen home` | `player_card_test.cxx`: `Fill` takes `account`, the unreachable words, progress row texts at 0/next/top; `profile_journey_test.cxx`: LB/RB order card, fighters, friends, awards, Y opens and Back closes `card-edit`, Home's Profile row opens `card`; `ui_render_profile.hxx`: card shots in every state at 1920x1080 and 1280x720 |
| **PR2 Fighters** | `src/ui/ProfilePanel.cxx` (Fighters tab: all 44, two groups, narrow band) | PR1 | `--profile played\|fresh\|noaccount --screen fighters` | `profile_journey_test.cxx`: an unplayed fighter can be made main; order played-then-roster; `ui_render_profile.hxx` fighters shots |
| **PR3 Awards merge** | `src/ui/ApplicationShell__Awards.cxx` (one tab, popout, other-card preview), `src/ui/ApplicationShell.hxx` (`AwardsScreen`), `src/ui/MenuPresentation.hxx`, `locales/*.po` | PR1 | `--screen awards` | `controller_navigation_test.cxx` (screen list without `awards-icons`), `shell_feedback_journey_test.cxx` (the LP check names `awards`), `profile_journey_test.cxx` (wear title, icon, both) |
| **PR4 Friends tab** | `src/ui/FriendsPanel.{hxx,cxx}` (new: rows, board, panel, More, pages), `src/ui/ProfilePanel.cxx` (the tab and its count), `src/ui/ApplicationShell.cxx` (one panel line), `src/ui/lab/UiLabSocial.cxx` (new: friends and member-card cases, so `UiLab.cxx` stays under 1,000), `cmake/sf4e_test_targets.cmake` (lab source), `locales/*.po` | friends-rt, PR1 | `--friends full\|requests\|empty\|noaccount\|unavailable\|loading\|unreachable\|offline --screen friends`; `--screen friends-add`, `friends-sent`, `friends-blocked` | `friends_panel_test.cxx` (new): grouping and order, each row's A action and hint from presence and `joinable`, More's entries and their enabled states; journey: A on a joinable friend sends join, Y opens More, X opens Add a friend, Send request with a typed code sends `request`; `ui_render_friends.hxx` (new) shots |
| **PR5 Room, toasts, Settings** | `src/ui/RoomPanel.cxx` (popout, page rows by relation), `src/ui/RoomActions.cxx` (Player card, Add friend, Block), `src/ui/ApplicationShell__ShellScreens.cxx` (the page's card above its rows, in place of `DrawMemberPoints`), `src/ui/RoomCards.{hxx,cxx}` (drop `DrawMemberPoints`), `src/ui/ApplicationShell__Toasts.cxx` + `ApplicationShell.hxx` (`ToastAction` command; request and invite toasts), `src/ui/ApplicationShell__Settings.cxx` (Friends panel), `src/ui/LineIcons.inc` (`Users`), `locales/*.po` | PR4 | `--room --points public --screen room-members --keys enter` (each relation); `--friends requests` with a toast; `--screen tab-online` | `room_points_test.cxx`: the page's FP is the picked fighter's; journey: Add friend sends `request` with the token and room name, relation rows disabled; toasts: Accept sends `accept` without changing screen; `ui_render_room_points.hxx` page shot |

**In the game, after the merged build** (two PCs): open Profile from Home and
step all four tabs on a pad and a stick; make an unplayed fighter the main;
add a friend from a public room's member page, accept it from the toast on the
other PC, join and watch from the Friends tab, invite into a private room and
join from the toast, appear offline, remove. At 1280x720 nothing is cut on
any tab.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| Merge Titles and Icons into one Awards tab? | Yes: same eleven awards, one tab fewer. | Splitting again is today's code. |
| Others' titles: the card look-up serves none, so another player's card has no title. | Add the worn award id to the card look-up once titles are awarded by the service; the card draws one when present. | One answer field, sent only to a client that asks. |
| Tier as the picture edge as well as words? | Yes: the frame is literally the tier's name, and it is off the number. | Drop the edge: one line in `LevelBand` and the hero. |
| Accept a request from the toast without seeing more? | Yes: the toast says who and whether you played; Decline needs the list. | Make A open the tab instead. |
| Friend code on the Card tab? | No: it lives on Friends (Add a friend row and page) and in Settings. | A row. |
| A toast when your request is accepted? | Not in 1.6.0: an outgoing request carries only its label, so the client cannot tell which friend it became. | A bridge field. |
