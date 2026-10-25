# Replay channel

Players' room matches go to one shared channel that anyone can browse in game
(Replays, Replay channel) and on the website (embernetplay.link/replays). Each
entry shows both players' names, fighters and classes. A player can stop
sharing at any time. A later step renders channel replays to video; this
document covers only the channel.

## What is shared

- A game an Ember room played with the player fighting in it: public or
  private room, never Training, offline Versus or a watched match.
- Only replays the game saved (Versus "record replay" on) and Ember archived
  with names bound to them (`RecordingAttribution`). A replay the archive
  could not tie to the match is never uploaded, since wrong names are worse
  than a missing replay.
- The file is the `.emberreplay` export the Replays screen already plays
  (`EMBRPLY1`, at most 51,335 bytes). Typical replays are 6 to 15 KB.

## Who can be listed

A replay is published when all of these hold:

1. Both players have a Discord account linked to their Ember ID. Without one
   there is nowhere to keep a choice to stop sharing, so the replay is held.
2. Neither player has turned sharing off.
3. The bridge knows both sides' accounts:
   - Public room: the room host's games report gives the winner and loser
     (`skill::record_games`). One player's upload is enough.
   - Private room: the bridge has no host report, so the replay is held until
     both players have uploaded the same game (room, table, generation).

Sharing is on by default. Turning it off hides every published replay of that
player at once (they are filtered on every read, not deleted) and stops new
ones. Turning it back on lists them again. The choice may change at any time.
It is kept per Discord account, like the ranking choice, so it follows the
player to any PC.

## What an entry shows

- Each side's name: the name that side's own game sent with its upload. A side
  that never uploaded (public rooms only) shows its board listing name, or
  "Player 1" / "Player 2" when it has none. A player's name is never taken
  from the opponent's upload.
- Each side's fighter, read by the bridge from the replay header (offsets
  0x20 and 0x170), never from what the client says.
- Each side's class at the time of the game (the band the ranking would show
  then, or the developer mark). A player who does not take part in ranking
  shows no class.
- The winner when known (public rooms: host report), the date, and the room
  kind (public or private).

## Flow

```
game: match ends ──▶ archive binds names ──▶ upload queue file
       (%LOCALAPPDATA%\sf4e\replay-channel\<sha256>.upload)
game ──small JSON op──▶ helper (reads the queue file itself)
helper ──HTTPS + player session──▶ bridge POST /v1/replays
bridge: verify, store file + row ──▶ list / file routes
site: /replays.json (nginx cache) and /replays/<id>.emberreplay
game: browse via helper ──▶ helper downloads into the archive ──▶ Watch
```

The game never sends replay bytes over its 8 KiB helper channel. It writes a
queue file and names it; the helper accepts only names inside its own queue
folder (no paths), reads the file, uploads and removes it on success or on a
refusal that will never change. Downloads are written by the helper into
`%APPDATA%\sf4e\replays\channel\` and answered with the file name, which the
Replays screen's Watch then plays through the existing import.

## Bridge

Capability `replay_channel` (only when `rooms.enabled`). Routes:

| Route | Auth | Purpose |
| --- | --- | --- |
| `POST /v1/replays` | player session | upload one replay (`ReplayUpload`, JSON, replay base64; body limit 96 KiB) |
| `GET /v1/replays/me` | player session | `ReplaySharing` (on, or no account) |
| `PUT /v1/replays/me` | player session | `{"sharing": bool}`, answered with `ReplaySharing` |
| `GET /v1/replays` | player session | `ReplayPage` for the game, filters below |
| `GET /v1/replays/public` | none | the same page for the website, cached 15 s |
| `GET /v1/replays/{id}/file` | none | the `.emberreplay` bytes (`application/octet-stream`) |

List filters (query): `fighter` (FighterCatalog id, either side), `band`
(1-10, either side), `name` (case-insensitive substring of either name),
`before` (cursor: an entry id from the last page). 50 per page, newest first.

Storage: migration 031 adds `replay_sharing` (per Discord account),
`channel_games` (room, table, generation, winner and loser accounts, played
at; written by `record_games` for every reported game and kept 14 days, since
`rated_games` goes with the room) and `channel_replays` (one row per game: id,
room kind, both accounts, names, fighters, bands, winner, played at, file
sha256, size, which sides uploaded). Files live in the bridge's state folder
under `replays/<sha256>.emberreplay`, created only where none exists.

Limits: 300 uploads per account a day; the oldest replays are pruned past
200,000 entries or 10 GB, whichever comes first; held private-room uploads
expire after 2 days without a partner.

Ids are 16 random bytes in hex, so the public file links cannot be walked.

## Website

`/replays` lists the public page with fighter, class and name filters, each
entry with both fighters' faces (the existing faces sprite), names, flags of
class pills, the date, and a Download button. Watching is in game: the page
says so and links the Ember download. `/replays.json` proxies
`/v1/replays/public` (15 s cache), `/replays/<id>.emberreplay` proxies the
file route (1 day cache, immutable). noindex like `/rankings`.

## Game

- Replays screen: a new "Replay channel" row opens the channel list, newest
  first, with a fighter filter. Choosing an entry downloads it (helper) and
  starts Watch; it is then in the player's own archive too.
- Online play: "Replay channel" opens the player's sharing choice
  (Share my matches: On / Off) with a line saying what is shared.
- The upload worker sends one queued upload at a time while the selected
  bridge offers `replay_channel`, retrying with backoff; queue files older
  than 7 days are dropped.

## Later

- Rendering channel replays to video on server1 for the YouTube channel.
- A nightly backup pull of the replay folder to server1 (the
  `ember-build-pull` pattern: server1 pulls, the VPS never connects in).
- An `ember://replay/channel?id=` link so the website can open an entry in
  the game.
