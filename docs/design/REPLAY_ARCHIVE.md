# Public replay archive

Every game played on Ember can be downloaded from the archive at
replays.ember.fraccie.be, with tournament sets listed by event and round.

## What goes in

- **Tournament sets.** A set's games always go in. An organizer turns them on
  by accepting tournament desk access; nobody opts in per game. The game sends
  each set game to the set's bridge (`replay_send`, `PUT
  /v1/matches/{id}/replays/{crc}`). The bridge keeps the game for the set and
  also puts it in the archive.
- **Other games.** When a player has Settings › Replays › **Share my replays**
  on (it is on by default), the game sends each game it played and did not
  just watch through `replay_archive`, which is `PUT /v1/replays/{crc}`. The
  helper tries each of the player's bridges and stops at the first one that
  has an archive. Only `.emberreplay` files are sent.
- The game sends one replay at a time. It sends all of a set's games before
  any other game, and the bridge stores tournament games first
  (`priority` 0).

Each game is kept once, under its CRC. When a copy comes in for a game the
archive already holds, the only thing it can add is the sender's country, on
the sender's side. A copy that comes with set information upgrades a game
that had none.

## Where it is kept

The bridge holds each game in `archive_games` until the `Archive` delivery
queue (`src/delivery/archive.rs`) has stored it. Games are stored with
signed S3 PUTs to the rustfs bucket `replays`, with these keys:

- `games/{crc}.emberreplay`: one game
- `sets/{match_id}.emberpack`: a set's pack (EMBRPAK1), rebuilt after each
  new game of the set

Once a game is stored, the bridge drops its bytes and keeps only the row that
lists it. When the store refuses a game, the bridge tries again an hour later
and keeps the game until then.

Anyone can read the bucket at https://s3.replays.ember.fraccie.be/replays/,
but nobody can list its contents. Its proxy only lets GET, HEAD and OPTIONS
requests through.

## Listing

`GET /v1/archive` takes these filters: `event` (a title, or `online`),
`fighter` (native ID), `country`, `round` (`top8`, `pools`, `winners`,
`losers`, `grand`), `q` (a name or event) and `before` (a time). It answers
with up to 400 games, newest first, each with its download URL. It also
answers with each listed set's score and pack, and with the events and
countries the filters can choose from.

The site in `server/ember/archive-site` is plain HTML. It shows the bridge's
listing at `/v1/archive`. Tournament games are grouped by event and set.
Online games are grouped by day; games between the same two players within
30 minutes count as one set.

## Bridge configuration

```json
"archive": {
  "endpoint": "http://10.40.0.247:9400",
  "bucket": "replays",
  "access_key": "…",
  "public_base": "https://s3.replays.ember.fraccie.be/replays"
}
```

The secret key is `archive_secret_key` in the integration secrets file.

## Limits

- One sender can send at most 3000 games a day.
- The bridge refuses new games while 2 GiB of games are still waiting to be
  stored.
- Filtered listings scan the whole table. That needs an index once the
  archive holds more than about 100k games.
