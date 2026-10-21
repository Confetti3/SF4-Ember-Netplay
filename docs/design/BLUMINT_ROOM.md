# BluMint room

Status: client side done in the UI lab, not yet run in a real BluMint match.
Branch `fraccie/blumint-room` (on `fraccie/v2-ui`). Not reviewed.

The room always wears this look, whatever the player's Look setting
(Classic or V2): it is BluMint's room, not a preference.

A BluMint match plays in Ember's ordinary tournament match room. In the V2
look that room wears BluMint's colours and wordmark, so players and watchers
see at once that it is BluMint's. Watchers get nothing to press but watching.
When a game or the set ends, the fighters get a notice that says what happened
and offers what comes next.

## What BluMint runs for USF4 (app.blumint.com, 2026-10-21)

| Format | How it plays | Set | Seen on BluMint |
|---|---|---|---|
| King of the hill | Viewers `!join` from the stream; the next one plays the crown holder; the winner stays; most matches won (top 3) is paid | Bo1 (host's choice) | JWong's USF4 King of the Hill, 100 matches |
| Streamer vs viewer | Viewers queue to play the streamer; a viewer who wins takes the per-match prize, one who loses can queue again | Bo3 | "Body me in USF4", "Beat me win $$", 50 matches |
| Double elimination | Bracket with check-in; grand final can reset | Bo3, losers final and grand final Bo5 | Good Fighters USFIV Ember Tournament (64 players) |

Every format has a 10 minute match setup wait: a player not ready in time is
disqualified by BluMint. Payouts, the queue, the bracket and the prize pool
are BluMint's.

Ember does not learn the format: a match reaches it as two fighters, a set
length (`gamesToWin` 1 to 10) and optional starting wins (BluMint's partner API
2.0.0). So the room says the same things for every format, and shows the
round's label when BluMint gives one.

## What the room shows

- **Header:** BluMint's wordmark × Ember's mark, `BLUMINT · <round>` over the
  room name, and chips: LIVE while a game is played, First to N, the score.
- **Ground:** BluMint's near-black navy with its blue, instead of Ember's art;
  cards, focus, tabs and buttons in BluMint's blue.
- **Watchers:** the table, the members and Chat, and Leave. No table options,
  room settings, invitations, Training or controllers: those are the
  fighters' and the host's.

## The notices

| Moment | When Ember sees it | Fighter's buttons | Watcher |
|---|---|---|---|
| Game done | The table's score rises and nobody reached first to N | Ready for the next game · Change fighter first · Not yet | A line on the status bar |
| Set won / lost | The score reaches first to N, or the match's assignment is `completed` | Stay in the room · Leave the room; with the next BluMint match already assigned: Play <next> · Stay | "<name> wins the set" on the status bar |
| Game did not count | The match ended on a lost connection; BluMint asks for the set again from the score | OK | Status bar |
| Host took the match | The assignment is `cancelled` (BluMint's close: host took over, score entered, removed) | Stay · Leave | Status bar |
| Not played in time | The assignment is `expired` | Stay · Leave | Status bar |

"Rematch" in a first to N is the next game: the notice's Ready readies the
fighter at once (never unreadies). A rematch after the set is BluMint's to
make (king of the hill's next challenger, a grand final reset): it arrives as
a new match, which the notice offers to play when it is already assigned.

## What the client reads from the backend

All of it already exists except the first item.

| Client needs | From | Used for |
|---|---|---|
| **The match's provider (`blumint`) and round label in the room's tournament binding** (`session::TournamentBinding`) | Bridge → room protocol. **Missing.** | Watchers: their PC has no assignment, so today only the two fighters see BluMint's room. With the binding carrying it, `InBluMintRoom` reads the room instead of the fighter's assignment list. |
| Assignment `provider == "blumint"`, `roundLabel`, `gamesToWin` | Bridge assignment list | Brand, header eyebrow, First to N |
| Table 0's score and phase | Room | Stage, pips, LIVE, game done / set done |
| Assignment `state`: `completed`, `cancelled`, `expired` | Bridge | Set done, host took the match, not played in time |
| `ConnectionLostReason` on the match's end | Bridge | Game did not count (BluMint retries from `startingWins`) |
| A next `blumint` assignment that is playable | Bridge assignment list | "Play <next>" on the set-won notice |

## Not yet

- A watcher's PC does not hold the match's assignment, so it cannot tell a
  BluMint match from another tournament's. The room's binding should say whose
  match it is (bridge and room protocol, Katie's), then watchers get the look.
- The setup wait's countdown (BluMint's 10 minutes) is not shown: Ember is not
  told it.
- The event room that queues an event's matches (BLUMINT_EVENT_ROOM.md).
