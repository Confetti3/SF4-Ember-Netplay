# Public replay archive

A bridge with an archive keeps the games played on Ember for anyone to
download from its archive page, with tournament sets listed by event and
round.

## What goes in

- **Tournament sets.** A desk event's set games always go in. An organizer turns
  them on by accepting tournament desk access; nobody opts in per game. The
  bridge archives a set's game only when the set is a match on its desk's
  connection; a partner's or a provider's set (BluMint, say) keeps its replays
  for the set alone: its games never reach the archive through the set route
  or the backfill, whatever its players share. Once such a set is closed (see
  Where it is kept) its games can still arrive as their senders' own online games, if
  their games share them, with only the sender's own name and no event. The game sends
  each set game to the set's bridge (`replay_send`, `PUT
  /v1/matches/{id}/replays/{crc}`). The bridge keeps the game for the set and
  also puts the set's copy in the archive.
- **Sets from before the archive.** When a bridge's archive is turned on, the
  sets' games it already holds go in too: each maintenance pass takes the next
  16 of them the archive has not taken in (`match_replays.archived` is 0),
  oldest first. A pass stops where the archive has no room (see Limits) and
  the next one goes on from there.
- **Other games.** When a player has Settings › Training and replays › **Share my replays**
  on (it is on by default), the game sends each game it played and did not
  just watch through `replay_archive`, which is `PUT /v1/replays/{crc}`. The
  helper tries each of the player's bridges and stops at the first one that
  has an archive. Only `.emberreplay` files are sent, and only games saved
  since sharing came on, so the replays already on the PC stay theirs. A game
  not sent before closing is sent the next time.
- **Played, not watched.** A game's provenance is unknown, played or watched,
  as the note bound to its body says. Only a played game is ever shared; an
  unknown one (no note yet, an unreadable note, or a game put back into the
  game and saved again) is never sent.
- **The cutoff.** The Share my replays cutoff changes only where preferences
  are loaded or accepted: turning sharing off clears it, turning it on from
  off sets it to now, and loading with sharing on and no cutoff sets it to now
  and saves it. The sender only reads it.
- A bridge without an archive answers `replays_unsupported`. The game then
  stops sending other games for the rest of the run and marks none of them
  sent.
- The game sends one replay at a time. It sends a set's games before any
  other game, except that one its bridge has not confirmed yet
  (`replay_unconfirmed`) waits 10 minutes while the others go. The bridge
  stores tournament games first (`priority` 0). The helper answers each request within 45 seconds across
  all of the player's bridges; the game gives up on one only after 3 minutes,
  ignores any later answer to it, and never sends the same replay again while
  the first request may still be live.

The bridge takes only a whole replay, checked as the game checks an export
before it imports one (`replays::Replay::parse`, after `Describes` in
`src/common/ReplaySlots.hxx`): the record is used, names the replay's size
and CRC, and the replay has a whole `#BRP` header whose two fighters are the
record's.

## Which game it is

A game is known by the SHA-256 of its replay, the bytes after the record and
the slot's two bytes. Those two are the saving PC's own (its slot number), so
both players' copies of one game are one game, and two games whose CRCs
collide are two. The CRC is only the native name the game is sent under; the
bridge checks it against the file.

A copy of a game the archive already holds changes nothing (200), whoever
sends it and whatever it says, with exceptions below: a set's own players, and
an online game's side that has no name yet, named by an account other than the
game's first sender.

## Who says what

- **Online games.** The first copy to arrive says its sender's side and that
  side's name and country. A name is only ever taken from its own player's
  copy: the game sends its own name alone (the other side empty), and the
  bridge ignores a name for the side that is not the sender's, so an older game
  that sends both names names no opponent. A copy that does not say its side
  names no one. A side with no name yet takes the name a later copy
  gives it, only from an account other than the first sender, never for the
  side the first sender said was theirs, and one account names one side;
  nothing else a later copy says is taken. The archive cannot check that the
  account played the side it names, so any account with a play session can
  name an empty side of a game it downloaded; this is inherent to a public
  archive. `sent_by` records whose word the first copy is, `sent_side` the
  side it claimed, and `named_by_a` / `named_by_b` who named a side later. A side nobody's copy named is
  shown unnamed, so a player who shares nothing is never named by the other.
- **Set games.** The sides are the set's roster: slot 0 is P1. The names are
  the players' accounts on the event's site, as the desk shows them, never
  what a player's game sent. A player adds their own country, on their own
  roster slot, when they send the game, and the other player's copy adds
  theirs.
- A game goes into a set only through the set's replay route, from one of its
  two players, and only as the replay of one of the set's confirmed games:
  one saved within that game's own time, from its permit to the last report
  on it, or to the organizer's decision for a game nobody reported, give or
  take 2 minutes. Each confirmed game takes one replay, the first that fits
  it (the nearest game when two fit); a replay that fits no game without one
  is refused (400). A replay that fits a game not decided yet (awaiting its
  second report, or held for review) is told to come back: 503
  `service_unavailable` with `details.reason` `replay_unconfirmed`, which the
  game puts aside for 10 minutes and then sends again. A voided game's replay
  holds no game, so the game played in its place takes its own. A replay kept before games
  were told apart holds one of the set's confirmed games without a replay,
  which one unknown, so such a set takes a new replay only while it has more
  of those games than such replays. The set's copy of a game is the one the
  archive gets, whoever sends it.
- The `revision` a game is sent with must be one the set's room binding had
  (1 to its current revision). The bridge does not record which binding each
  game was played under, so that is all it checks; the pack's room count is
  the senders' word.
- What this does not prove: the save time is in the record, outside the
  replay's CRC and digest, so a set's own player can still send another
  replay with its record's time moved into a free game's time. The set route
  takes only a participant's word for which game a replay is, and `sent_by`
  records whose word it was.
- An online game the archive already holds joins the set only when its own
  sender sends it for the set; nobody else's copy relabels it.

## Where it is kept

The bridge holds each game in `archive_games` until the `Archive` delivery
queue (`src/delivery/archive.rs`) has stored it. Games are stored with
signed S3 PUTs to the rustfs bucket `replays`, with these keys:

- `games/{sha256}.emberreplay`: one game, as its first copy came
- `sets/{match_id}.emberpack`: a set's pack (EMBRPAK1)

Once a game is stored, the bridge drops its bytes and keeps only the row that
lists it. When the store refuses a game, the bridge tries again an hour later
and keeps the game until then.

A set's pack has its own queue (`src/delivery/packs.rs`) and its own row in
`archive_sets`. Every game the set gains in the archive, new or an online
game joining it, raises the row's `version` in the same transaction. The
queue builds the pack from the set's replays as they stand, and records the
version it built in `uploaded_version` once the bucket has it; a set that
gained a game meanwhile stays due and is stored again. A set is not leased
again while a lease of it runs, so an older pack never replaces a newer one.
A set with no replays left is never stored as an empty pack. Replay
retention keeps a set's replays while the bucket's pack is older than the
set's archived games, whether the bridge has an archive now or not, and,
while it has one, until each is `archived`. When retention removes a set's replays, the same transaction marks
its `archive_sets` row `pruned_at`, and the set is closed: its replay route
refuses every later game (400, `details.reason` `set_closed`, which the game
takes as final), so no pack is ever rebuilt from part of a set. Such a game
can still go in the archive as an online game, if its sender's game shares
it.

The endpoint is read once, at configuration load, as an origin (http or
https, a host and an optional port; no credentials, path, query or
fragment). Each object's URL is built from it once, and the signature and
the request are both made from that URL, so a default port or an IPv6 host
is signed as it is sent.

Anyone can read the bucket at its public address (`public_base`), but
nobody can list it: the archive site's nginx passes only exact game and set
file names (`games/<64 hex>.emberreplay`, `sets/emt_<uuid>.emberpack`), with
GET, HEAD and OPTIONS, and the bucket policy allows anonymous `s3:GetObject`
on those two prefixes and nothing else. Staging serves the page, its listing
and the bucket under `/archive/` on the staging bridge's own host
(`https://bridge-staging.embernetplay.link/archive/`), where the bridge's own
`/v1/archive` is 404 so the listing has one way in; production gets a site of
its own, `https://archive.embernetplay.link/`. Deployment:
`server/ember/archive-site/deploy/README.md`.

## Listing

`GET /v1/archive` takes these filters: `event` (a title, or `online`),
`fighter` (native ID), `country`, `round` (`top8`, `pools`, `winners`,
`losers`, `grand`), `q` (a name or event) and `cursor`. It answers with up to
400 games, newest first by save time, then by digest, each with its digest,
CRC and download URL. It also answers with each listed set's score, and its
pack's URL once the bucket holds one (`null` before), and with the events and
countries the filters can choose from.

When there is more to look through, the answer carries `next`, an opaque
string, and `more: true`; the request with `cursor=<next>` goes on from
there, so games saved in the same second are never skipped at a page's end.
One listing looks through at most 5000 stored games, so a selective filter
holds the database only briefly; a filtered page can hold fewer than 400
games, even none, and still carry `next`.

The events and countries are kept in `archive_events` and
`archive_countries` as games are stored or join a set, so a listing does not
read the whole archive for them.

The site in `server/ember/archive-site` is plain HTML. It shows the bridge's
listing at `/v1/archive`. Tournament games are grouped by event and set.
Online games are grouped by day; games between the same two players within
30 minutes count as one set.

## Bridge configuration

```json
"archive": {
  "endpoint": "http://127.0.0.1:9400",
  "bucket": "replays",
  "access_key": "emberarchive",
  "public_base": "https://bridge-staging.embernetplay.link/archive/replays"
}
```

It goes in the instance's tenants file (`tenants.staging.json` for staging),
which `apply-tenants` lays over bridge.json. The secret key is
`archive_secret_key` in the integration secrets file
(`set-integration-secret.sh archive`); the bridge does not start with an
archive and no key, so the secret goes in first.

## Limits

Both routes go through one admission, in the transaction that keeps the game:

- One sender can send at most 3000 games a day (429). A set's games do not
  count against it: the games the set played bound them.
- The bridge takes no new game, from either route, while 2 GiB of games are
  still waiting to be stored (503). A set's game refused this way is not kept
  for the set either; the game sends it again later.
- One sender's day counts only the games it sent as online games
  (`archive_games.charged`); a game that joins a set later keeps its charge.
- `GET /v1/archive` needs no credential. The page's nginx site limits it to
  2 requests a second per address (burst 10) and 6 at once, and caches each
  answer for 15 seconds. It is the only way in: the bridge's own public host
  answers 404 for `/v1/archive` (`deploy/nginx/bridge.conf.in`), and the
  archive site reaches the bridge on loopback.
- A set keeps one replay per CRC (`match_replays`), so a CRC collision within
  one set is still one game there.

Migration 050 rebuilt `archive_games` around the digest. It kept none of the
games 049 held: a stored one no longer has its replay, so its digest cannot
be known, and no bridge had run an archive. The backfill takes the sets'
games in again.
