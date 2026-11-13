# Nightly feedback batch: accessibility and copy review (2026-11-13)

Scope: `nightly` at `2abac8e9a`. Screens drawn with `build/current/UiLab.exe`, which was up to date: the UiLab target rebuild reported "no work to do", and the exe is newer than the card sources. Shots are in `D:\sf4-scratch\n1113-shots\` as PNG files. They cover:

- 1920x1080 at 1.00x for every screen and state listed in the lab.
- 1280x720, `--scale 0.70` and `--scale 1.50` for the card, Edit card, player page, Notifications, Awards and Archive.
- 880x1080 and 1600x900 for the card.
- The card in German, Russian and Polish, reached by changing the Language row in the lab.

Contrast is measured from the pictures: the strongest ink against the box's commonest colour, WCAG relative luminance. Text sizes are calibrated against Inter at the same pixel size, and they match ImGui's sizes.

`--check` reported one problem across all runs (Accept request, A9). It does not catch overlap or words split in the middle. Both happen (A1, A2).

## 1. Accessibility findings, most severe first

No finding blocks a task outright, so nothing is rated Critical. A1 is the one to fix first: it affects every game in a private room.

| # | Sev. | Screen, state, shot | What is wrong (measured) | Where | Smallest fix |
|---|---|---|---|---|---|
| A1 | Moderate | Post-match prompt, private room (no cards), a blocked opponent, any opponent without a card. `postmatch_private_fhd.png`, `postmatch_blocked_fhd.png`, `crop_pm_private.png` | The opponent's answer chip ("Choosing") is drawn on top of their ping chip ("Direct 46 ms"), so neither can be read. When the Add friend box is absent, `Tag` gets the whole row width and right-aligns the ping and link at its far end. The chip is then placed after the tag's natural width. This is new with `PostMatchRows.cxx` (ffcd06178), and private rooms always hit it. | `src/ui/PostMatchRows.cxx:50-52`; `src/ui/V2.cxx:666-667` | Pass `(std::min)(v2::TagWidth(tag, v2::TagSize::S), room)` as the width to `v2::Tag`, so the ping sits right after the name and the chip after it. |
| A2 | Moderate | Award tiles on the Compact card (1280x720, and any content under 1840 wide). `card_sixty_hd.png` ("Unstoppabl / e"), `loc_ru_sixty_1280x720.png` ("Бессменны / й", "Неудержи / мый"), `loc_de_sixty_1280x720.png` ("Unaufhalts / am") | A one-word title wider than the tile is split in the middle of the word across two lines. `CalcWordWrapPositionA` cuts inside any word wider than the line. The cut first half then fits, so the shrink added in e6d208253 never runs for the case it was written for. English titles at 13 px: Untouchable 81, Unstoppable 80, Journeyman 80 against 76 px of room. 39 titles across the Latin and Cyrillic catalogs overflow. | `src/ui/CardParts.cxx:143` (wrap), `:148` (shrink) | Break only at spaces. When one word is wider than `inner`, set that line smaller down to `TextFloor * s`, and only then shorten it with an ellipsis. Never split inside a word. |
| A3 | Moderate | Fighter pictures on the card strip and the Fighters tab. `card_played_fhd.png` (Zangief, unfocused), `fighters_sixty_hd.png`, `crop_tier_focus.png` | The Ember tier's stripe is exactly the focus accent: `#FF8738` against `#FF8738`, 1.00:1. An unfocused Ember fighter carries an accent-coloured rectangle next to the focused tile. PROFILE.md's own rule says three tier frames read as three focuses. Ember and Copper are also close: stripes 1.21:1, plates 1.12:1. On card tiles and Panel icons the tier is shown by colour alone. The pips count levels in the family, not the tier. | `src/ui/V2.cxx:519` (Ember stripe), `:40` (accent) | Give Ember a stripe that is not the accent, for example `#FF5A36`. Spell out the tier where a tile is focused: the card award row's detail already builds the word (`CardModel.cxx:392`). |
| A4 | Moderate | Off switches: Awards on card "Show my highest", Looking for, Text and language "TV mode", Friends "Appear offline". `card_pins_fhd.png`, `crop_pins_toggle.png`, `card_looking_fhd.png`, `text_panel_fhd.png` | The off track is invisible. Track against the focused row is 1.02:1, its hairline 1.24:1, and about 1.35:1 on an unfocused row. An off switch reads as a lone white dot, and on and off differ only by the orange fill. This fails 1.4.11 (3:1). | `src/ui/GameMenuDraw.cxx:155-156` | Draw the off track's outline in `col.muted`, 2 px (7.8:1). |
| A5 | Moderate | Fighters tab, the tier words row (Ash Copper Silver Gold Ember). `fighters_sixty_hd.png` | The four tiers that are not current are drawn at 40 %: Ash 3.29, Copper 3.15, Silver 3.05, Gold 2.91:1 for 13 px text. They are the only written legend for the tier colours. | `src/ui/FightersTab.cxx:84` | Keep the word at full strength and dim only the plate. Mark the current tier with the full plate plus an outline. |
| A6 | Moderate | Archive failed, empty and unsupported; Find a player with no recent opponents. `archive_failed_fhd.png`, `archive_empty_fhd.png`, `find_empty_fhd.png`, `sheet_archive_states.png` | Focus starts on Filters, so the side panel explains Filters. "Could not reach the archive" appears with no reason and no next step on screen. The reason and "Select to try again" show only after moving down to the row. | `src/ui/ArchiveTab.cxx` (state rows); `src/ui/FindPlayer.cxx` | Start the focus on the state row when the list holds only that row. Or draw its detail under the title inside the row. |
| A7 | Moderate | Another player's card and the Friends panel, the "You and Player 2" cell. `player_stranger_hd.png`, `player_stranger_s150.png`, `friends_panel_hd.png` | At 1280x720 and at 1.50x the key wraps to "You and Player" / "2", which pushes the value down. In the Friends panel at 720p, "Friend since 2026-12-..." is cut, so the date cannot be read. | `src/ui/CardModel.cxx:310-312` (cells); fact-cell drawing in `V2.cxx` | Key "Head to head" (copy C14). Never shorten a date value: wrap the key instead. |
| A8 | Minor | Interface size 0.70x, everywhere on the card. `card_played_s070.png`, `card_edit_s070.png`, `notifications_s070.png` | The 13 px floor becomes 9.1 px: cap height 6 to 7 px, for example "World Warrior" and the record line. The shrink floors become 6.3, 7.0 and 7.7 px. Contrast stays at 6.4 to 7.6:1. This is readable at a desk, not on a TV. | `CardModel.hxx:26`; every `TextShrunk(..., 9/10/11 * s)` | See "TextShrunk" below. Say in the Interface size detail that text gets small (C19). |
| A9 | Minor | Post-match, opponent asked first. `postmatch_received_fhd.png` | The box reads "Accept requ...": the label needs 123 px and has 116. This is the only `--check` hit. | `src/ui/PostMatchFriend.cxx:39` (`friends.accept_member`) | "Accept friend" is 109 px and fits (C8). |
| A10 | Minor | Card buttons, Panel "More", player page "More" when the keyboard is in use. `card_played_fhd.png`, `player_stranger_fhd.png` against `card_pad_fhd.png` | The buttons always show the pad's Y and X glyphs, while the hint bar shows T and F. With a pad they match. | `src/ui/PlayerCard.cxx:166` | Draw the glyph from the same device-aware prompt the hint bar uses. |
| A11 | Minor | Award pips, unreached families. `zoom_card_played_fhd_1405_284.png`, `awards_fhd.png` | Unreached pips (`c.border` `#443A31`) are 1.61:1 against the tile, so "2 of 5" reads as "2". | `src/ui/CardParts.cxx:124` | Draw unreached pips as a 1 px outline in `muted` (5:1 or more). |
| A12 | Minor | News list and detail, "Update" chip. `news_all_fhd.png` | Accent text on the accent-tinted chip, 13 px: 4.19:1 in the list and 3.97:1 on the detail header's gradient. | chip in the News list and detail | Use `accentLight` text, or a darker chip fill under the text. |
| A13 | Minor | Top fighters tile, focused. `zoom_card_played_s150_456_896.png` | The name and games line start 4 px from the tile edge. The 2 px focus edge touches the first glyph at 1.00x and 1.50x. | `src/ui/PlayerCard.cxx:302` | `edge = 8 * s`, the same as the picture's pad. |
| A14 | Minor | Toasts for a friend request and an invite. `toasts_fhd.png` | "just now" is 11 px. Requests and invites use the amber warning triangle, which reads as a warning. | toast drawing | 13 px age; a person or room icon for these two kinds. |
| A15 | Minor | Friends panel at 1.50x. `friends_panel_s150.png` | The Panel's "Join their room" and "More" buttons are cut off at the bottom. The hint bar still offers both. | Friends panel | Let the Panel scroll with the focused row, or drop the button pair when it does not fit. |
| A16 | Minor | Card at 880 wide in de and ru. `loc_ru_played_880x1080.png` | The hint bar drops every label and shows bare glyphs. | hint bar layout | Keep the labels of the first three hints and drop the Pages pair first. |
| A17 | Minor | Own and others' cards while loading or unreachable. `card_loading_fhd.png`, `player_reading_fhd.png`, `player_unreachable_fhd.png` | Rated games 0, Win rate 0 %, Wins 0 are shown as if they were real. | `src/ui/CardModel.cxx` | Show "-" until the read succeeds. |

Checked and passing:

- Muted text `#B5A99B` on every ground: 5.6 to 8.1:1.
- Accent text: 5.7:1 on focused rows, 7.5:1 on unfocused rows.
- Gold, Silver and Ember title plates under light text: 6.9:1 or more.
- Tier icons on their plates: Ash 3.39, Copper 3.22, Silver 5.88, Gold 4.39, Ember 4.34:1, all above 3:1.
- Presence marks: in a room is a filled dot, online a ring, offline a dashed ring. Shape carries the meaning, not colour alone.
- A focused primary button turns lighter with a light outline, distinct from the unfocused one.
- Every action shown has a hint. The ranked-play notice is reachable and offers Connect Discord.

### TextShrunk (e6d208253)

In about 180 shots at 1.00x, 0.70x and 1.50x, in English, German, Russian and Polish, I found no line that `TextShrunk` actually set smaller. The Panel's record line measures 14 px at 720p and at 1080p. The points words, fact values, games lines and title plate all fit at full size.

The one place built for long words, the award title, never reaches its 9 px floor (A2). So today the floors of 9, 10 and 11 px are latent. They go against PROFILE.md, "it never shrinks text under the floor".

Recommendation: `least = player_card::TextFloor * s` at all five call sites (`PlayerCard.cxx:264, 276, 305, 553`, `FightersTab.cxx:32`) and in `CardParts.cxx:148`. This is 13 px at 1.00x; anything still too wide wraps or drops a part. At 0.70x that floor is 9.1 px, and nothing should go under it.

## 2. Copy findings

The `#` column is my numbering (C1 to C26) for referring to findings.

| # | Id | Now | Proposed | Why |
|---|---|---|---|---|
| C1 | `level.loading`, `level.unreachable` (shown on another player's card) | "Reading your points." / "Could not reach the service. Your points show when it answers." | Add `level.loading_other` "Reading {0}'s points." and `level.unreachable_other` "Could not reach Ember. Their points show when it's back." | Another player's page says "your points" (`player_reading_fhd.png`, `player_unreachable_fhd.png`; `CardModel.cxx:227-232`, `PlayerCard.cxx:344`). |
| C2 | `friends.toast_invite` | "{0} invites you to their room" | "{0} invites you to a room" | The line under it says "Player 5's room": "their room" is wrong whenever the inviter is not the host. |
| C3 | `friends.join_room` | "Join their room" | "Join room" | Same problem. The row's own hint already says "Join". |
| C4 | Notifications rows (request and invite) | "Player 2 … Accept" | Row label "Player 2 wants to be friends" / "Player 3 invites you to a room" (reuse `friends.toast_request` and C2) | The row and its panel never say what it is. The request's panel shows only "Played you 3 games". |
| C5 | `notifications.none`, `notifications.none_detail` | "Nothing yet." / "What Ember says, such as a link copied or a room left, is kept here for this session." | "No notifications" / "Friend requests, invites and Ember's notices show here." | The list now holds requests and invites, which last longer than a session. |
| C6 | `notifications.waiting` | "To answer: {0}" | "{0} waiting" | Plain wording that fits the badge. |
| C7 | `friends.played_you` | "Played you {0} games, last {1}" | "{0} games together, last {1}" | "Played you" is not idiomatic. |
| C8 | `friends.accept_member` (post-match box) | "Accept request" | "Accept friend" | It fits the box (A9) and says what is accepted next to the rematch buttons. |
| C9 | `public.ranked_needs_discord` | "Connect Discord to play ranked" | "Connect Discord for rated games" | One term: the product says "rated games"; "Ranked" is also a Looking for chip. |
| C10 | `level.no_account` | "Connect Discord in Settings, Online to earn points." | "Connect Discord to earn points." | A Connect Discord button sits right under it. |
| C11 | `friends.no_account` | "Friends need a Discord account. Connect it in Settings, Online." | "Friends need a Discord account." | Same reason. |
| C12 | `find.searching` with `profile.reading`, `card.sets_loading`, `archive.loading` | "Looking." / "Reading {0}'s card." / "Reading sets." / "Loading sets" | "Searching…" / "Loading {0}'s card…" / "Loading sets…" / "Loading sets…" | One verb for loading. "Looking." reads as a fragment. |
| C13 | `profile.unreachable` | "Could not reach the service. The card shows when it answers." | "Could not reach Ember. Try again in a moment." | It names the thing the player knows and points at the Try again button. |
| C14 | `card.you_and` | "You and {0}" | "Head to head" | It wraps and orphans the name (A7). The panel's title already names the player. |
| C15 | `card.region_detail` | "Your region, from the relay Ember picked. You can hide it." | "Your region, from your connection. You can hide it." | "relay" is jargon. |
| C16 | `card.pins_detail` | "Up to four award families on your card, each at the level you reach. Your highest shows the hardest ones." | "Up to four awards on your card, each at the level you've reached. Show my highest picks the hardest ones." | Drops "families" and names the switch it refers to. |
| C17 | `card.pins_auto` | "Your highest" | "Highest" | It is the value of the row "Show my highest". |
| C18 | `discord.account_detail` | 4 sentences | "Connect a Discord account for rated games, points, friends and shared replays. Tournament sites such as BluMint use it too. It can be a different account from the Discord app's." | Half the length. "your Ember ID uses" is jargon. |
| C19 | `settings.interface_size_detail` | "Below 1.00x fits more on a large monitor." | "Above 1.00x is easier to read on a TV. Below 1.00x fits more on a monitor, with smaller text." | It says what each direction costs. |
| C20 | `archive.met` | "Played {0}" | "vs {0}" | {0} is the fighters faced. "Played Cammy" reads as playing Cammy. It also matches `card.set_vs`. |
| C21 | `archive.no_service_detail` | "The archive is read from Ember's service. Open Ember ID to set it up." | "The archive needs an Ember ID. Open Ember ID to set one up." | Says what is missing, without "service". |
| C22 | `archive.f_text_detail` | "Lists the sets with a player's name or an event's title that holds these letters." | "Sets whose player or event name contains these letters." | Shorter. |
| C23 | `find.text_detail` | "Part of a name, two letters at least, or a friend code." | "At least two letters of a name, or a friend code." | Clearer order. |
| C24 | `profile.sets_note` | "Sets found by the name {0}. Someone else who used this name shows here too, and sets played under an earlier name do not." | "Sets are found by the name {0}: anyone else who used it shows here too, and earlier names don't." | Shorter and clearer. |
| C25 | `award.rival.name` / `award.rival.50.title`; `award.friends.15.title`; `award.wins.2500.title` / `award.opponents.200.title` | "Rival" / "Rival"; "Regulars" (beside `award.matches.100` "Regular"); "Legend" / "Street Legend" | Family "Rivalry"; Friends 15 "Crew"; keep one of the two Legends | Each pair puts two near-identical names on the wall or in the pin list. |
| C26 | `profile.main_character` (Edit card row) | "Main character" | "Main fighter" | The card, the Fighters tab and the chips all say "fighter" or "main". |

These also need code rather than catalog changes:

- The Settings > Online panel summary joins bare values: "On · On", "Off · -", "Not connected · On · On". Switches need their names, for example "Activity on · Invitations on". A missing friend code should be left out, not shown as "-".
- An Archive session row says "3 games" twice: in the heading through `archive.session_label` and again on the line under it. Drop one.
- The count format in titles is mixed: "Sent (3)" and "Blocked (1)" (`friends.sent`, `friends.blocked`) next to "Friends · 2" and "Notifications · 2". Use "Sent · {0}" and "Blocked · {0}".

No change needed:

- The `archive.*` filter labels and values, the round names and the sort words.
- The `style.*` chips.
- `card.*` labels and keys, apart from those above.
- `card.device_*`, `card.link_detail`, `card.looking_*`, `card.pins_full`, `card.no_awards`, `card.most_played_none`.
- The `award.*.how` lines.
- `awards.*`.
- `postmatch.*.detail`.
- `discord.account`, `discord.account_none`, `discord.account_linked`, `discord.account_linked_detail`.
- `find.recent*`, `find.none`, `find.more`.
- `profile.blocked`, `profile.no_card`, `profile.pick_title`.
- `friends.now_friends`, `friends.blocked_name`.

Everything is in sentence case, and nothing blames the player.

## 3. Not shot or not judged

- The real game was not used. I could not judge on-TV reading distance, the Steam Deck, or speech output (the speech settings exist, but nothing reads the new screens aloud in the lab).
- The lab has no Discord account in the `--friends full` state. The "Discord account" row's connected value ("Connected · name") never drew, so it is unchecked.
- Some lab data looks inconsistent and may not reflect the product:
  - Edit card says "Awards on card: 2 chosen" while its preview shows four awards.
  - Home's profile panel says "Win / loss record 0 W / 0 L" beside 4747 rated games.
  - A friend is shown with "Friend since 2026-12-26".
  - Home shows a "TOURNAMENT ORGANIZER" title, an award id that this batch removed.
  - Sent shows a request named "-".
- The lab cannot draw the room member popout's card or the post-match prompt at 0.70x and 1.50x through `--screen`, so I did not shoot them there.
- Locales: I checked de, ru and pl on the card only. Japanese, Korean and Chinese were not checked for the shrink or wrap paths.
- `--check` misses words split in the middle (A2) and overlapping chips (A1). Those two were found by eye.

## Shots for each Moderate finding

All are in `D:\sf4-scratch\n1113-shots\`.

| Finding | Shots |
|---|---|
| A1 | `postmatch_private_fhd.png`, `postmatch_blocked_fhd.png`, `crop_pm_private.png`, `crop_pm_blocked.png` |
| A2 | `card_sixty_hd.png`, `sheet_award_tiles_hd.png`, `loc_ru_sixty_1280x720.png`, `loc_de_sixty_1280x720.png` |
| A3 | `card_played_fhd.png`, `fighters_sixty_hd.png`, `crop_tier_focus.png` |
| A4 | `card_pins_fhd.png`, `crop_pins_toggle.png`, `card_looking_fhd.png`, `text_panel_fhd.png`, `friends_empty_fhd.png` |
| A5 | `fighters_sixty_hd.png` |
| A6 | `archive_failed_fhd.png`, `archive_empty_fhd.png`, `find_empty_fhd.png`, `sheet_archive_states.png` |
| A7 | `player_stranger_hd.png`, `player_stranger_s150.png`, `friends_panel_hd.png` |
