# Ember controller-first menus

Ember's menus cover Home, rooms, fighter selection, settings and offline training.

## Navigate

Press F10 or Start at the native main menu to open Ember. On an assigned XInput controller, physical A selects and B goes back, independently of your fighting bindings. DirectInput retains SF4-mapped Light Punch/Light Kick with explicit LP/LK prompts because its physical button labels are not identifiable, and has no shortcut buttons. On an XInput controller in a room, X, Y and View are shortcuts (see Play online). D-pad or keyboard arrows move the highlight; Left/Right adjusts value rows. Lists and grids stop at their edges. Direction holds repeat, but actions and confirmations do not.

Back cancels the current text draft, confirmation or open choice first, otherwise returns one level. On your own table card in a room, Back leaves your seat or queue place instead (see Play online); elsewhere on the room board it goes to Home. Back from Home hides Ember and returns to the game without leaving your room, and Home's Online play row returns to it. Use the explicit Leave room action to disconnect; its confirmation starts on Cancel.

Select a text row before typing or pasting with a physical keyboard. Enter accepts the draft; Escape or controller Back discards it. Chat is the exception: its message box is already open when the screen opens, so you just type and press Enter to send. Moving the mouse pointer moves the highlight over menu rows, dialog options and the seat chooser, while a resting pointer never undoes what the pad just chose; clicking selects. Click either end of a value row to adjust it with a mouse.

## Play online

Choose Online Play, then Create Room or Join Room. Create retains populated defaults. Join prioritizes Paste Invitation and Join. While connected, Online Play returns to your room.

The room overview follows SF6's Custom Room composition: four stacked battle slots on the left, members above chat on the right, and the room's actions (Members, Chat, Table options, Room settings, Copy invitation, Leave room) sharing the right column beneath them, with the focused entry's explanation on a line below. Right moves from a slot to the member pane; Left returns to the last focused slot. Up/Down visits entries in order. Narrow layouts stack the same entries in one scrolling list.

Select a slot with A. With no seat or queue place, it opens the seat chooser: Play as P1 or Play as P2 for an open seat (an empty table offers both), otherwise Join queue; then Watch; and Table options. The D-pad or mouse pointer picks, A confirms and B cancels. On your own seat, A readies (Ready up, Ready for rematch) or takes Ready back (Unready / unlock fighter). On your queue place, or on another table when you already have a place, A opens that table's Table options, which is looking, not joining. B on your own card leaves your seat or queue place at once. It asks first only when leaving would end a set score or hand the seat to a queued member, with the question starting on keeping your seat. It explains why instead when a game or unresolved result holds the seat. While a start is held for a locked-in spectator, B (Cancel the start) takes Ready back, the same as A. On an Xbox pad, X opens Change fighter, Y opens Table options (the focused table's, otherwise your own) and View opens Chat; on the keyboard F, T and C do the same. On the keyboard Escape always goes back, even on your own card, and Delete leaves your seat or queue place. Pressing the same button again returns to the board, and all three work from every room screen, including fighter selection.

Player cards use native SF4 portraits. If the game's portrait cannot be loaded, the packaged original-outfit picture stands in where one exists. The Random stage card reads the game's character-select random tile at runtime and shows the usual preview-unavailable placeholder when that tile cannot be read. The members pane and Home profile card show each player's saved main, independently of battle selection. The main is validated and shared during room admission, including for spectators. Battle-slot portraits still show the current match fighter after the pre-battle exchange. Older snapshots without optional profile metadata show an unknown portrait instead of guessing from the match fighter. Portrait metadata does not change selection or readiness.

Members, room rules and host administration retain separate action menus. Chat (View, or C) shows the whole conversation, oldest at the top, with your own messages marked You and a line for each member joining or leaving, a new host and a finished game or set. Typing needs a keyboard: Enter sends, and the draft stays in the box, when you leave the screen too, until the room shows your message. Up and Down (or Page Up and Page Down) scroll; Escape or B goes back. While you type, F, T and C are letters, so the keyboard shortcuts wait until you leave the box. Messages from other members that arrive while you are elsewhere show as a count on the Chat row and above Recent chat until you open Chat; your own and muted members' messages never count. Table options (Y) opens Battle setup for a table: both players' Ready status stays visible. Change fighter (X) opens the roster at your fighter; picking one goes on to its Ultra, and picking the Ultra returns to the room. Then press A on your card for Ready up. Ready requires both seats, a connected assigned input device, valid choices and a healthy room. Missing requirements appear in the detail panel. Ready locks fighter changes; Unready / unlock fighter cancels Ready before the match begins. Rematch uses the saved selection. Host-only actions remain unavailable to other members, with an explanation. Battle setup has, in order: Ready, Change fighter, Ultra Combo (Left and Right step it), Input delay (Left and Right choose 1 to 10 frames, Select applies the recommendation; the detail names the recommended and match delays), Check connection, then the table's rules. The host edits Rounds, Round time and Edition Select there and presses Apply rules; others see the rules on one line. Both fighters play at the higher of their two delays. See [custom rooms](CUSTOM_ROOMS.md).

Discord invitations use the same pending, cancellation and confirmed room-switch flow. Cancel does not leave your current room. Live table/member updates preserve focus by identity, and stale destructive confirmations are discarded.

## Choose a fighter

The arcade roster previews the highlighted fighter. Select commits it; merely moving the highlight does not. Saved choices are marked. Appearance opens separate Costume gallery and Color gallery pages with individual artwork cards. Move to preview, Select to save, then Back to return. Choices use native availability rather than asset counts. Ultra, Stage and additional options remain separate; P1 retains stage authority.

## Change settings

Home also includes Profile. Edit your name and choose a main from the portrait roster. Press A on Xbox (or click a portrait), then wait for Saving to finish. After the settings update is accepted, the menu returns to Profile and names the saved portrait. A failure stays visible with Retry. Merely highlighting a portrait does not save it. Your saved main supplies the player-card photo and name; it does not overwrite your next match's fighter. The card shows local confirmed online wins, losses and win percentage. Tracking started with v0.8.0, is local to this settings profile, and excludes offline/training, spectating, draws, cancellations, disconnects and unresolved results. It is not a Steam rank or historical record import. A native result must agree with the room's confirmed result, and repeated notifications are deduplicated. The profile persists through the existing asynchronous settings writer. Your name, main and personal settings cannot be changed while you are in a room; leave the room first.

XInput prompts display A for Select and B for Back. These menu defaults do not rewrite your SF4 attack mappings. DirectInput uses a generic button sprite with LP/LK labels instead of guessing another controller's labels. Controller, direction and keyboard sprites are unmodified assets from Kenney Input Prompts 1.5A (CC0), not custom-drawn shapes or font-dependent Unicode symbols.

Player & Controller, Gameplay Defaults, Interface and Discord use named value rows. Valid personal changes are coalesced and saved asynchronously. Saving/Saved/Error feedback is quiet; Retry saving resubmits after failure. Room rules still require Apply and host authority.

Ember automatically adopts SF4's connected controller, or the sole connected controller when the native menu still points to keyboard. Multiple unassigned controllers trigger release-all / press / release capture; simultaneous presses cannot choose an arbitrary device. With no controller, keyboard is provisional until explicitly chosen or used to join a room. You can explicitly choose a device under Player & Controller. Disconnecting never silently replaces an established device.

Before starting a match, Ember rechecks the device and verifies native P1/P2 binding. Each local GGPO input sample is read only while that binding and device identity still match. A disconnect, replacement, or changed native slot produces neutral input and a visible warning, never another controller's buttons. Reconnect the original device; if its slot changed, return to the room to reassign it. These checks use SF4's native type, index and device name; they cannot prove physical hardware identity when identical devices are re-enumerated under the same slot/name.

## Practice

In offline training, F6 opens/closes Ember's controls. Use keyboard arrows, Enter and Escape, or the mouse. Escape returns one level, closing the flyout at its root. Training has no controller binding; Start affects only native pause while the flyout is closed. An open flyout captures gameplay input everywhere, including controllers. Held closing buttons must return to neutral before gameplay resumes.

Dummy Recording has eight slots, playback, loop and clear; Input History exposes each player's runs. The save-position section and separate frame panel are removed; use the passive bar for frame data. Occupied recording overwrite and clearing require confirmation. Record and Play close Ember only after the game thread accepts the command. Failure stays on screen. Recording/playback is suspended while controls are open and while closing inputs drain. Close native pause separately before practice resumes.

F5 toggles the passive training meter, F6 controls, F7 record/stop and F8 play/stop. F9 restore is removed. F7 opens the recording menu for confirmation when a slot is occupied.

## Runtime acceptance checklist

These checks require your physical controllers and a running game; synthetic tests are not substitutes:

- XInput: A selects and B returns even with remapped fighting controls; X, Y and View never activate a menu row, and only open Change fighter, Table options and Chat inside a room. DirectInput retains mapped LP/LK. Check release/open, directional repeat, focus restoration, disconnect/reconnect and explicit reassignment.
- Native main menu: no cursor movement, selection or Back action underneath the visible overlay; no held close-button leak on return.
- Training: Start opens only native pause. F6 opens/closes Ember; keyboard arrows/Enter/Escape navigate. Controller presses cannot open or operate the training flyout.
- Training isolation: navigation never moves fighters or enters recordings; recording/playback resumes cleanly after accepted commands and release.
- Rooms: create/join, seat chooser, queue/watch and Lock in to watch, Ready/Unready/rematch, B leaving a seat or queue place, host rules, stale/rejected commands, safe leave, Discord cancellation/switching and two-PC play.
- Settings: rapid changes coalesce, failed persistence displays Retry, and a restart reloads accepted settings.
- Match input: test the same controller as both P1 and P2, after rematches, and after disconnect/reconnect. Press a second controller to confirm it cannot control the local fighter. Check the warning on a changed native device slot.

## Local evidence and rebuild

`ControllerNavigationTest` exercises the renderer's shared semantic model and shell/training journeys. `UiRenderTest` renders all redesigned screens across ten viewport/DPI configurations, including 1920x1080 at 150%, and can decode the installed game's artwork without launching it. Its row-text probe caught 35-pixel Home text being drawn into a 24-pixel padded interior. Rows now reserve space from their actual font metrics, and the extra heading scale was removed. The test checks this on every standard menu row and verifies that all four battle slots fit at standard landscape sizes. The native input analysis is recorded in `2026-09-08_reverse-USF4-menu-input-report.md`; the visual references are in `SF6_ROOM_REFERENCE.md`.

Build only with `scripts/build-current.ps1`. Package only from `build/current` and `build/current/stage`, using `scripts/package-team.ps1`. The parent workspace's `verify-current-package.ps1` checks the source fingerprint, receipt, staged/package binaries and ZIP contents. Other historical checkouts are not release targets.
