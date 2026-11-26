# Fight chat: room chat over a fight, and quick lines

A room's chat already reaches the room during a fight (`SessionController` accepts room
actions mid-game), but a fighter never saw it and could not send. Fight chat shows the
room's chat briefly over the fight and lets a fighter send a few preset lines without
leaving it. Typing stays where it was: the room board's chat between games, and the
see-through menu a watcher opens over the game they watch.

## What the player sees

- **The feed** (`ui/FightChatFeed`): over a fight this PC plays or watches, the newest
  three messages said since the fight began, each for six seconds and then a second's
  fade. A member's repeated line folds into one ("GG x2"); two members who share a name
  never fold together. A line is stamped once, when the feed first sees it, and never
  again after it fades. It sits under the life bar on the left, or on the right when the
  match strip's panel, wherever the strip placed it (`MatchStripGeometry`), is in the way.
  It is never drawn over Ember's own windows.
- **Settings › Interface › Room layout** (with the chat filter, which a room leaves open):
  - Chat in matches: My table (default: the fighters, queue and watchers of the table
    this player has a place at), Everyone, or Off.
  - Quick lines: on by default.
  - Four quick slots, Back + Up/Right/Down/Left or F1 to F4, each one of eight lines
    (GG, One more?, Lag?, Sorry!, Nice!, Thanks!, Good luck!, Close one!). Defaults: GG,
    One more?, Sorry!, Lag?.
- A muted member's lines never show, and typed lines follow the chat filter as in the
  Chat screen (`ChatLineText`). Quick lines are Ember's words and skip the filter.

## Quick lines

| Step | Where |
| --- | --- |
| Catalog | `common/QuickChat.hxx`: the eight lines, each with a saved code, a catalog key and its English text. |
| Pad | `common/QuickChatPad.hxx`: while Back is held in a fight this PC fights in, the directions belong to the chord, and one that goes down sends its slot once. The game sees neither Back nor the directions meanwhile: the fighter stands still while choosing. The pad update counts those updates as withheld by a gesture (`WithheldReason::Gesture`). |
| Keys | F1 to F4 down in such a fight (not repeats), kept from Ember's menu and the window's other keys. USF4 reads its own keyboard through Raw Input, so a game key the player bound to F1 to F4 still reaches the game. |
| Hand-off | Both post the slot into one bit mask (`sf4e::overlay::PostQuickLine`). Whether quick lines are live (a fight this PC fights in, the player's quick lines on) is decided once, by the pad update on the game thread (`SetQuickLinesLive`); the window procedure reads that. The overlay's frame takes the mask every frame and hands it to the shell (`ApplicationShell::SendQuickLines`) while Ember is hidden; while Ember is shown the lines are dropped. The shell applies only the room's rules. |
| Send | The slot's line goes as a room chat action with `quick` set and its English text, now or never: one the room cannot take at this moment, or a second inside the room's one-a-second rule, is dropped rather than sent after the moment it was for. |
| Room | `RoomAuthority::ApplyChat` keeps every chat rule and takes a quick line only with its own English text. The message keeps `quick`. |
| Readers | Each shows the line in their own language; anything that does not know the id reads the English text. |

Every peer in a room runs the same build (public rooms are listed per build, private
rooms check the sidecar hash), so a quick line never meets a reader without the field.

Chat in matches is one table (`netplay::FightChatScopes`: saved word, catalog key, next
scope) that the saved file, the Settings row and the validity check all read.

## Not done here

- A Chat row on the post-match prompt: that prompt is a ten-second rematch decision, and
  the room board's chat is there between games.
- Quick lines from watchers of a tournament match, whose rooms take no chat from them
  (`MATCH_WATCH.md`).
