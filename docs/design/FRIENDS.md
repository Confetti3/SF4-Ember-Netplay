# Friends

Players who added each other see where the other is (in a room, online,
offline), join or watch them, and invite them into their own room. Friends
belong to the Discord account connected to the Ember ID, like the rating
(`SKILL_BADGE.md`): a new Ember ID keeps its friends, and a throwaway one has
none. A player without a Discord account has no friends list; the tab says
how to connect one (Settings, Online, Discord). This is the base the owner
wants more social features on.

## The list lives on the bridge

The friend list, requests, blocks and presence are kept by the bridge, by
Discord account, not taken from the Discord Social SDK's relationships:

- It works on Linux and Steam Deck without the Discord client. The companion
  (`src/discord/main.cxx`) only sets rich presence with a join secret, and
  needs the Discord client on the same machine to do even that.
- Discord friends are not Ember players, and the SDK's relationships need a
  second Discord authorisation in the game, separate from the Ember ID's
  sign-in.
- Ember decides what a friend sees, and never hands out a Discord ID or name.

Later, the SDK could suggest Discord friends who play Ember; that needs
nothing here to change.

## Data model

### Migration 061 (`061_friends.sql`)

After 060 (`FIGHTER_LEVELS.md`).

```sql
-- A Discord account's friends settings and how friends see it: the name,
-- flag and main fighter its game last sent (as the ranking listing is sent),
-- its friend code, whether it appears offline, and when it was last seen.
-- Made at its first sync. When the player turns friends off, the friendships,
-- requests and presence are deleted, the code is replaced and `off` is set;
-- the row stays, with the player's blocks, so that a sync cannot turn
-- friends on again and the player's blocks keep protecting them. Only
-- turning friends on clears `off`.
CREATE TABLE social_accounts (
    discord_user_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    country TEXT,
    main_fighter INTEGER CHECK (main_fighter BETWEEN 0 AND 43),
    code TEXT NOT NULL UNIQUE,
    code_at INTEGER NOT NULL,
    appear_offline INTEGER NOT NULL DEFAULT 0,
    seen_at INTEGER NOT NULL,
    off INTEGER NOT NULL DEFAULT 0 CHECK (off IN (0, 1))
) WITHOUT ROWID;

-- Two accounts who are friends; `first` sorts before `second`. `id` is the
-- name both sides use for the friendship.
CREATE TABLE friendships (
    first TEXT NOT NULL,
    second TEXT NOT NULL,
    id TEXT NOT NULL UNIQUE,
    since INTEGER NOT NULL,
    PRIMARY KEY (first, second),
    CHECK (first < second)
) WITHOUT ROWID;
CREATE INDEX friendships_second ON friendships(second);

-- Requests. `label` is what the sender saw (a room name or the code they
-- typed); the receiver is shown the sender's own name. A request the
-- receiver declined, or could never see (they blocked the sender, or their
-- inbox was full), has `hidden_at`: the sender still sees it as sent, and
-- cannot ask that account again until it lapses, 30 days after `sent_at`.
CREATE TABLE friend_requests (
    id TEXT PRIMARY KEY,
    sender TEXT NOT NULL,
    receiver TEXT NOT NULL,
    label TEXT NOT NULL,
    sent_at INTEGER NOT NULL,
    hidden_at INTEGER,
    UNIQUE (sender, receiver)
) WITHOUT ROWID;
CREATE INDEX friend_requests_receiver ON friend_requests(receiver, sent_at);
CREATE INDEX friend_requests_sent ON friend_requests(sent_at);

-- Accounts a player blocked, with the name to list them under.
CREATE TABLE friend_blocks (
    blocker TEXT NOT NULL,
    blocked TEXT NOT NULL,
    id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    at INTEGER NOT NULL,
    PRIMARY KEY (blocker, blocked)
) WITHOUT ROWID;
CREATE INDEX friend_blocks_blocked ON friend_blocks(blocked, blocker);
```

| Table | Read by | Lifetime |
|---|---|---|
| `social_accounts` | key; `code` (unique) for a request by code | for good; `off` from turning friends off (`forget`) until they turn them on (`turn_on`) |
| `friendships` | `first` (key prefix) and `second` (index) | until removed, blocked or forgotten (turned off) |
| `friend_requests` | sender (unique prefix, also the daily count), receiver, `sent_at` (sweep) | until answered or cancelled; any row 30 days after `sent_at` (the sweep in `routes/expiry.rs`) |
| `friend_blocks` | blocker (key), blocked (index, on every request) | until unblocked or forgotten |

Ids are `prefixed_id` (`fr_`, `rq_`, `bl_`, `iv_`), random, and say nothing.
The friendship's record (wins each way, last game) is `pair_records`
(`FIGHTER_LEVELS.md`, migration 060); it counts from that migration on.

### Kept in memory

- **Presence**: Discord account to `{presence, build_id, game, beat_at}`.
  Online is a sync within the last 75 seconds; an entry older than 10 minutes,
  or an `offline` sync, is dropped. A bridge restart forgets it; players are
  back within a sync. A private room's offer (its name, size and invitation)
  is part of the presence and exists nowhere else: it is never written to the
  database or a log, and the types that hold it print `<redacted>` for the
  invitation.
- **Invites**: one per (inviter, invitee), at most 20 per invitee, each 10
  minutes. Lost on restart.
- `seen_at` is written when it is over 5 minutes old, and on an `offline`
  sync, so an online player costs at most one write per 5 minutes. It does
  not move while the account appears offline.

## Wire contracts

Capability `friends` (`ember_protocol::friends::FRIENDS_FEATURE`), offered
where public rooms are on and Discord sign-in is offered
(`sign_in::offered(&state, &Site::Discord)`), and only beside `points`. Every
request type denies unknown fields and every answer type ignores them
(`friends.rs` header). A build asks only a bridge that lists `friends`;
an older bridge answers 404, worded `friends_unavailable`.

Capability `friends_presence_v2` (`ember_protocol::friends::PRESENCE_V2_FEATURE`)
is offered wherever `friends` is. It stands for three request members that a
bridge from before it refuses as unknown (a sync's `game`, a private room
report's `offer` and the `join` action) and the answer members that go with
them (`game`, `private_room` and `joining`). A client names none of the request
members to a bridge that does not list it (Older builds and bridges).

Friends are on by default: the bridge setting `rooms.friends`
(`config::Rooms::friends`, read only by `friends::offered`) defaults to true,
and an operator can still turn it off. While it is off, discovery does not
list `friends`, both routes answer as an unknown route (404), the card
look-up's `friend` selector answers null and a card's `relation` is `none`.

Both routes take the player session (`routes::play::playing`) and answer the
whole `FriendsState`, so the client holds one state and replaces it whole,
as the desk's read and act do (`desk_read`, `desk_act`).

### `POST /v1/friends/sync`

The client's presence in, the state out. Bucket `friends-sync:{account}`, 12
a minute.

```json
{"build_id":"…","listing":{"name":"Player 1","country":"GB"},"main":3,
 "presence":{"activity":"room","room":{"room_id":"0f…","status":"fighting","table":0}}}
```

| Field | Rule |
|---|---|
| `build_id` | as in a ticket request; decides whether a friend's room is joinable for this caller |
| `game` | optional, 1 to 16 lower-case letters and digits (`is_game_id`): the game the client is for. Absent is read as `usf4` (`DEFAULT_GAME`, the one place the default is written), because every build from before the field is USF4's and sends none. Decides whose rooms the caller is shown and can join. Only to a bridge that lists `friends_presence_v2` |
| `listing` | `ember_protocol::skill::Listing`: what friends see as the name and flag |
| `main` | 0 to 43, optional |
| `presence.activity` | `menus`, `offline_play` (Training, Arcade, versus CPU), `room`, or `offline` (leaving now) |
| `presence.room` | with `room` only: `room_id` (32 hex, a public room only) or `private: true`; `status` `idle`, `queued`, `ready`, `fighting`, `watching` or `training`; `table` 0 to 7 when seated or watching; `offer` for a private room that friends may join |
| `presence.room.offer` | only with `private: true` (a public room with one is refused): `secret`, the room's own invitation (the `emd1:`/`emd2:` token, checked by shape); `name`, a room name; `capacity`, 2 to 16; `members`, 1 to `capacity`. Only to a bridge that lists `friends_presence_v2` |

A private room that offers itself, in a sync that names the game:

```json
{"build_id":"…","game":"usf4","listing":{"name":"Player 1","country":"GB"},
 "presence":{"activity":"room","room":{"private":true,"status":"idle",
   "offer":{"secret":"emd1:…","name":"Player 1's room","members":2,"capacity":8}}}}
```

The bridge takes a `room_id` only for a room that is open and listed (its
`rooms` row); any other is kept as "in a room" without one. It keeps an
`offer` as sent, in memory, for as long as that presence stands, and hands the
`secret` to no one but through the `join` action. The request stays strict at
the offer: an unknown member, a malformed `secret` or a size that does not hold
refuses the whole sync as `invalid_request`.

### `POST /v1/friends/actions`

One action in, the state after it out. Bucket `friends-act:{account}`, 30 per
10 minutes, with the limits under Abuse.

| `action` | Fields | Does |
|---|---|---|
| `request` | `card` and `label`, or `code` | asks the account behind a card token (`FIGHTER_LEVELS.md`) or a friend code; when that account already asked the caller, it is accepted instead |
| `accept` | `request` | makes the friendship |
| `decline` | `request`, `block` | hides it from the receiver; with `block`, blocks the sender |
| `cancel` | `request` | the sender takes it back |
| `remove` | `friend`, `block` | ends the friendship, silently; with `block`, blocks |
| `block` | `card` and `label` | blocks a room member |
| `unblock` | `block` | lifts a block |
| `invite` | `friend`, `room`: `{room_id}` or `{secret}` | offers a friend the caller's room for 10 minutes |
| `join` | `friend` | asks for the invitation to the private room a friend offers; answered with `joining` (below), or refused `join_unavailable`. Only to a bridge that lists `friends_presence_v2` |
| `dismiss` | `invite` | drops an invite the caller received |
| `appear_offline` | `on` | friends see the caller as offline |
| `new_code` | none | a new friend code, at most once a day; the old one stops working |
| `forget` | none | turns friends off: the caller's friendships, requests both ways and presence are deleted, the code is replaced and the account is stored as off; their own blocks stay. Answered with the `friends_off` refusal |
| `turn_on` | none | turns friends on again: the account as it was left, with its new code, no friends and its blocks; the only action an off account is allowed |

`secret` is the room's own invitation, the 127-character `emd1:`/`emd2:`
token the Discord join secret carries (`src/discord/Ticket.hxx`,
`IrohRoom::DiscordInvitation`); the bridge keeps it in memory only and
checks only its shape.

`join` changes nothing on the bridge. It reads the caller's friend, that
friend's last sync and the offer in it, and answers the whole state with one
extra member, the room's invitation for that one friend:

```json
{"action":"join","friend":"fr_…"}
```

```json
{"code":"4F2A-91CX","friends":[…],"incoming":[],"outgoing":[],"invites":[],"blocked":[],
 "joining":{"friend":"fr_…","secret":"emd1:…"}}
```

The bridge hands the invitation out only when all of these hold: the caller
has not turned friends off; the account is the caller's friend; that friend
does not appear offline and synced within 75 seconds; the friend's last sync
and the caller's name the same game and the same `build_id`; the friend's
presence is a private room with an offer; and the offer's `members` is below
its `capacity`. The caller must have synced since the bridge started
(`resync_required` otherwise, as for every action).

Refusals (`details.reason`, the one `FriendRefusal` list in the protocol
crate, which the bridge writes and the helper passes on): `no_discord_account`,
`friends_off`, `unknown_code`, `card_expired`, `own_card`, `friends_full`,
`blocks_full`, `request_limit`, `code_too_soon`, `invite_unavailable` (not a
friend, or the friend is offline or appears offline), `join_unavailable`
(error code `stale_revision`, "That friend's room cannot be joined now.", as
`invite_unavailable`). A `join` has one refusal for every reason: not a friend,
a friend who is offline, appears offline or has gone quiet, another game or
build, no room on offer, a room with no seat free. The caller learns nothing
about which, so a refusal never says whether a friend is hiding. Rate limits
answer 429.

`friends_off` (error code `stale_revision`, "Friends are off.") answers the
sync and every action but `turn_on` while the caller's account is off. A game
shows it as the state "friends are off", not as a failure.

### `FriendsState`

```json
{"code":"4F2A-91CX",
 "friends":[{"id":"fr_…","name":"Player 3","country":"FR","since":1730073600,
   "main":{"fighter":5,"points":1986,"tier":3},"badge":{"band":7,"settled":true},
   "presence":{"state":"room","status":"fighting","table":0,
     "room":{"room_id":"0f…","name":"Player 1's room","members":7,"capacity":16,"joinable":true},
     "game":"usf4"},
   "record":{"won":14,"lost":9,"last_at":1731000000}}],
 "incoming":[{"id":"rq_…","name":"Player 9","country":"DE","sent_at":1731100000,
   "played":{"games":6,"last_at":1731090000}}],
 "outgoing":[{"id":"rq_…","label":"Player 4","sent_at":1731100000}],
 "invites":[{"id":"iv_…","friend":"fr_…","room":{"room_id":"0f…"},"expires_at":1731100600}],
 "blocked":[{"id":"bl_…","name":"Player 7","at":1731000000}]}
```

| Field | Rule |
|---|---|
| `code` | the caller's friend code, `XXXX-XXXX` in Crockford base32 |
| `appear_offline` | written only when on |
| `friends` | at most 200, in no order (the client groups and sorts) |
| `main` | the friend's main and its points and tier; `points` and `tier` absent together until the friend has played that fighter |
| `badge` | `SkillBadge`: their class as last shown (`skill_ratings.band`, settled by the aged deviation), read without remembering anything; the developer mark for a developer; absent when they do not take part, or classes are hidden |
| `presence.state` | `offline` (with `seen_at` when known), `online` (with `activity`), `room` (with `status`, `table`, and `room` for a public room or `private: true`) |
| `presence.game` | on `online` and `room`: the game the friend is in, written whenever the bridge knows it. A friend in another game than the caller's has `game` and what they are doing, and neither `room` nor `private_room` |
| `presence.private_room` | on a `room` with `private: true`, for a friend in the caller's game who offers the room: `name`, `members`, `capacity` and `joinable`. Never the invitation |
| `room.joinable` | a public room: open, not locked, not full, and of the caller's build |
| `private_room.joinable` | the friend is of the caller's build and the room has a seat free. The bridge does not know whether the room is locked: the game stops offering a locked room at its next sync (Presence) |
| `joining` | `friend` and `secret`: only in the answer to `join`, never in a sync's answer or any other action's |
| `record` | the pair's wins from the caller's side and their last game (`pair_records`); absent before their first counted game |
| `incoming` | at most 50 visible; `played` is the same pair record, so a receiver sees whether they played the sender |
| `outgoing` | including hidden ones, which stay "sent" |
| `invites` | received and live; `room` as sent |
| `blocked` | at most 500 |

A friend in a private room that offers itself, and one in another game:

```json
{"id":"fr_…","name":"Player 3",…,
 "presence":{"state":"room","status":"idle","private":true,"game":"usf4",
   "private_room":{"name":"Player 3's room","members":2,"capacity":8,"joinable":true}}}
{"id":"fr_…","name":"Player 4",…,
 "presence":{"state":"room","status":"fighting","private":true,"game":"sfv"}}
```

A game reads `game`, `private_room` and `joining` as it reads the rest of the
answer: a value it cannot use (a `game` that is not a game id, a
`private_room` with no name or a size that does not hold, a `joining` with a
malformed `secret`) drops only itself, and the friend stays in the room.

### The card look-up

`POST /v1/cards/lookup` (`FIGHTER_LEVELS.md`) also takes `{"friend":"fr_…"}`
for a friend's whole card, and answers `relation` for each card once
`friends` is offered: what the room's member page needs to offer Add friend.
A card of a friend also names the friendship (`friend`, the id the two call it
by), and a card of anyone carries the caller's record against them (`versus`),
as `PROFILE.md` says. A `request` selector opens the card of whoever sent the
caller a request they can see, and a `code` selector the card of the account a
friend code names; neither names anyone while friends are off.

### Older builds and bridges

- Builds before 1.6.0 never call these routes; nothing they see changes.
- A 1.6.0 build on a bridge without `friends`: the Friends tab says "This
  service does not offer friends yet", and a room member's page offers no
  Add friend.
- No route here changes an existing answer, ticket or table.
- **A game with `friends_presence_v2` on a bridge that lacks it.** The helper
  (`rust/sf4-net/src/tournament/friends.rs`) strips the sync's `game` and the
  room's `offer` before it asks, and refuses `join` itself as
  `join_unavailable`, the refusal the bridge would word. The bridge then sees
  the sync it always read, so no sync fails with `invalid_request`. The game
  shows a private room as "In a private room" with no Join, as before.
- **A game without it on a bridge with it.** It sends no `game`, which reads as
  `usf4`, no `offer` and no `join`. Answers only gain optional keys (`game`,
  `private_room`, `joining`), which an older game ignores, so its list is what
  it was.
- **A bridge upgraded under a running helper.** The helper keeps what it read
  of the capabilities of a bridge that lists `friends`, so it picks up
  `friends_presence_v2` after a restart of the game. A request that carries one
  of the members and is refused as malformed (`invalid_request`, or a 400 that
  names nothing) makes it read the capabilities again, which covers a bridge
  put back to an older version.
- No migration, no new table or column. Public room hosts
  need no new build.

## Presence

The client builds its presence from the same description the Discord
companion is sent (`discord::Describe`, `src/discord/Presence.hxx`):
"In menus" is `menus`, "Playing offline" `offline_play`, and every room state
`room`, with `Member::training` as `training`. The public room's id is the one
the client joined through the bridge.

A private room is "In a private room" to the friends of anyone in it. It is
also offered to them (`presence.room.offer`) while all of these hold:

- the room is private and not a bound match;
- its Privacy setting is Friends (`room::Snapshot::friendsHidden` is off);
- it passes the rule `discord::Describe` offers a room to Discord friends by
  (`offerable`): not a bound match, healthy, not locked, not full; and it has
  an invitation that has not run out.

`netplay::friends::ReportOf` builds the offer and the sync carries it to the
bridge. The Discord presence and invite settings do not govern it; they are
the companion's. A report's comparison includes the offer, so a renewed
invitation, a new size or name, a leader change, Lock or Hidden syncs at once
(under the cadence below).

Friends are shown only the room's name, its size and whether it is joinable
(the same game, the same build, a seat free). The invitation leaves the bridge
only in the answer to that friend's `join`. A room that is not offered
(Hidden, locked, full, a match, or an invitation not ready) stays "In a
private room" and nothing more.

**Another game**: a friend whose sync names another game than the caller's is
shown with that game ("Playing SFV", "In a room · SFV") and nothing joinable:
no room, no offer, no Join, Watch or Invite. A caller the bridge has not heard
from since it started is read as the default game with no build, so no private
room is joinable for them until their first sync.

The client syncs every 30 seconds while the helper runs, every 10 seconds
while the Friends tab shows, at once when its own presence changes (at most
every 5 seconds), and with `offline` as it closes. A friend's change reaches
a viewer within that viewer's next sync.

**Appear offline**: friends see `offline` with the `seen_at` of when it was
turned on; the player still sees their friends, requests and invites, and
can still invite (an invite reveals them to that one friend, which the
invite box says). While it is on, the game sends no offer
(`Report::WithoutOffer`), so no invitation leaves the PC, and the bridge
refuses a `join` for that player's room.

## Privacy

- **Friends** see: name and flag (as the friend's game sends them), class as
  last shown, main and its points and tier (once played), presence, and the
  record between the two of them. In a private room set to Friends they also
  see the room's name, size and whether it is joinable, and the game.
- **A private room's invitation** is the room's key. The bridge hands it out
  only in the answer to a `join`, to the friend who pressed Join, and only
  while the friendship stands, the friend is online and not appearing offline,
  both are of the same game and build, and the room is offered with a seat
  free. The friends of any member can ask, since every member's game sends the
  offer; a non-friend, a blocked account and an account with friends off never
  can. The bridge keeps the invitation in memory only, never stores or logs
  it, and no state carries it except that one answer.
- **What stops a new hand-out**, each within one sync: the host sets Privacy to
  Hidden; the host sets Admission to Locked (the game stops offering the
  room); the room fills; the friendship ends (unfriend, or either side blocks);
  the player turns on Appear offline (no offer is sent, and `join` is
  refused).
- **What does not recall an invitation already handed out.** Hidden withdraws
  the offer, not a key a friend holds. The room's capability stays the same
  through renewals of the invitation and through leader changes, so an
  invitation works until its own expiry (stamped up to a week ahead, by the
  member who renewed it last) while the room is open and has a seat. Admission:
  Locked stops anyone joining, holders included. Kick removes a member and
  keeps that connection out until their helper restarts
  (`PUBLIC_ROOMS.md`). The Room settings text says this beside the Privacy row.
- **A non-friend** learns nothing of presence. Holding a card token (being in
  the same public room), a received request, a friend code or a search result
  they see what a card shows anyone (`PROFILE.md`, Privacy): the points and
  tier, the name, flag, class and main, the month the player came to Ember, the
  award families they wear and show and the counts behind them, how they play,
  and their own relation and record with that player. Sending a request tells
  them nothing about the answer: a declined, blocked or full-inbox request
  stays "sent" until it lapses. A friend code says only whether it exists, and
  who holds it is the card it opens.
- **Request receivers** see the sender's name and flag and whether they have
  played each other (games and when); nothing else until they accept.
- **Never served**: a Discord user ID or name, an Ember ID, a rating, or
  anything of an account to the account it blocked. A card token opens only
  on the bridge.
- **Removing** a friend is silent: they stop seeing presence at their next
  sync; the record stays in `pair_records` and shows again only if they become
  friends again.
- **Turning friends off** (`forget`) is a stored state, not a reset. The
  friendships, requests both ways and presence are deleted, the code is
  replaced (the old one stops working) and the account is marked `off`. The
  player's own blocks stay, since they protect the player; blocks others
  placed on them are those players' own and stay as well. While it is off the
  sync and every action but `turn_on` are refused as `friends_off`, so the
  game's own 30-second sync cannot turn friends on again. A request by code
  is refused as an unknown code, one by card looks sent and is never shown
  (also after they turn friends on), and an invite finds no friend. The
  player is not found by name either (`PROFILE.md`, Find a player); their
  card stays and is still served from a room's token. Only `turn_on` ends
  it.
- The Discord account moving to another Ember ID keeps its friends; a
  different Discord account on the same Ember ID starts with none.

## Abuse

| Limit | Value | Kept |
|---|---|---|
| New requests a day | 20 per sender | counted from `friend_requests` |
| Pending outgoing | 30 | counted |
| Visible incoming | 50; further ones are stored hidden | counted |
| Same pair | one request at a time; none for 30 days after a hidden one | `UNIQUE (sender, receiver)` and `hidden_at` |
| Requests by code | 10 an hour | in-memory bucket `friends-code:{account}` |
| Friends | 200 each; accepting past it refuses `friends_full` | counted |
| Blocks | 500 | counted |
| Invites | 10 per 10 minutes; one live per pair; friends only | in-memory |
| Joins | 10 per 10 minutes, counted when asked, whether refused or not; each one hands out a room's invitation | in-memory bucket `friends-join:{account}` |
| New code | once a day | `code_at` |

A block hides every request from the blocked account at once, ends the
friendship and any invite both ways, and makes their card's `relation`
`none` to them. Blocking is from a request (Decline and block), a friend
(Remove and block) or a room member's page (Block). Room moderation (mute,
kick) is unchanged and separate.

Friend codes are 8 symbols of Crockford base32 (about 10^12), typed with any
case, spaces or dash, `O` read as 0 and `I`, `L` as 1.

## Client

### View models

- `netplay::friends::State` (`src/netplay/Friends.hxx`), the answer decoded
  with its known keys checked and unknown ones ignored (`src/session/SocialAnswers.cxx`,
  as `TournamentAnswers.cxx` `Object` does), plus `unavailable` and `no_account`.
- `netplay::friends::Requests` (`FriendRequests.hxx`): the cadence above, one
  sync or action in flight, the last refusal, kept per bridge and Ember ID.
- `ShellView::friends`, mapped in `sf4e__Overlay.cxx`.
- `src/netplay/GameIdentity.hxx` is the one
  game-specific file of the feature: `GameId` (`usf4`), the sync's `game`;
  `GameTitle` ("USF4", "SFV", "UMVC3"; empty for an id this build does not
  know); `SameGame`, which reads a presence that names no game as this
  game's.
- The runtime (`sf4e__NetplayRuntime__Social.cxx`) builds the presence, and
  carries out Join, Watch and an accepted invite:
  - **Join a public room**: the public room join by room id that the Public
    rooms list uses (`room_ticket`), asking first when in a room or a game,
    as a Discord join does.
  - **Watch**: Join, then, once in, the room's own Watch on the friend's
    table.
  - **A private room's invite**: the secret goes through the Discord join's
    own path (`discord::PendingInvite::Offer`, which asks first when busy,
    `sf4e__NetplayRuntime__Status.cxx`), with its own sequence.
  - **Invite to my room**: a public room's id, or the private room's
    `DiscordInvitation()` under the rule `discord::Describe` offers it by
    (not a bound match, healthy, not locked, not full).
  - **Join a friend's private room**: Join on a friend whose presence carries
    a joinable `private_room` (`ToFriend` answers `Destination::Kind::
    PrivateOffer`). The runtime sends the `join` action (`FriendAskJoin`)
    with the friend and the room generation the press was made in. The answer
    is taken (`TakeJoining`) only when its `joining` names that friend and the
    player is still in the room the press was made in; a refusal or an answer
    for another friend says "That room cannot be joined now.", and a changed
    room drops the answer with a log line. Then the invitation goes through the
    path of a private room's invite above, which asks first when the player is
    busy (Switch to invited room, or Cancel invitation). The invitation lives
    for that one press: it is in no state, log line or interface text. A friend
    in another game is refused as `OtherGame` with the same words. Watch stays
    for public rooms; a private room is refused for it, offered or not.
  - **No offer while appearing offline**: the sync is sent with
    `Report::WithoutOffer`.

### Friends tab (Profile)

Board, the `Friends` board's layout:

- **Left**: requests first, each a line with name, flag, "Played you 6
  games, last yesterday" and two buttons, Accept (primary) and Decline;
  then the friends under section headings (`v2::SectionHeading`) "In a room ·
  3", "Online · 2", "Offline · 2", each row: a presence mark, flag, name,
  class emblem, a small tile of their main with `LevelBand`, the status line
  ("Playing at table 1, Player 1's room", "In Training", "Last online 3
  hours ago") and its action word as the value (Join, Invite); then "Sent
  (n)" and "Blocked (n)" rows, each opening a page of its own.
- **Right**: the selected friend's panel: the small card (`PLAYER_CARD.md`),
  presence, "You and Player 3: 14 – 9", Last played, Friend since, and the
  focused row's action as the primary button (Join their room when joinable,
  Invite to my room when the player's room can be offered, else Player card)
  beside More (Y), which lists Player card, Join their room, Watch, Invite to
  my room, Remove friend and Remove and block, each disabled with its reason.

A friend in a room, as the row and More show them:

| The friend is | Status line | Row action | Join their room (More) |
|---|---|---|---|
| in a public room the bridge lists | "Playing at table 1, Player 1's room" and its kin | Join when joinable | enabled when joinable |
| in a private room set to Friends, joinable | the same lines, with the room's name | Join | enabled; sends `join` |
| in a private room set to Friends, of another build | the same lines, with the room's name | Invite when the player's room can be offered, else Player card | disabled: "You can't join that room right now." |
| in a private room that is hidden, locked, full or a match, or on a bridge without `friends_presence_v2` | "In a private room" | Invite or Player card | disabled: "A private room is joined by invite only." |
| in another game | "Playing SFV" (menus), "In a room · SFV"; "Playing another game" for a game this build has no title for | Player card | disabled: "That room cannot be joined now."; Invite to my room is disabled too |

A full room is "In a private room" like a hidden one, because the game stops
offering a room that is full; the bridge's own check of a seat is a second
guard against a stale sync.

Home's Friends row counts the friends who are not offline, whatever game they
are in.

Presence marks differ in shape, not colour alone, as toasts' tones do: a
filled disc in a room, a ring online, a hollow dashed ring offline.

Navigation: request buttons and friend rows are placed rows; the D-pad moves
by geometry. The panel shows the friend whose row was last focused (as
`selectedTable_` follows a table), so Right from a friend row reaches that
friend's buttons and Left comes back. Select on a friend row is its action
word; a row without one moves to the panel. X opens Find a player (the page
that adds a friend by code), Y opens More on a request or friend row, and
Delete on a friend row asks Remove friend (`FriendsPanel::Shortcut`); the hint
bar shows `Y More` and `X Find a player`. Appear offline is the row at the foot
of the list, and its state a header chip, "Appearing offline". LB and RB step
the tabs; Back returns to where the tab was opened from.

Pages: **Add a friend** (the player's own code with New code, a text row
"Their friend code" through the on-screen keyboard, Send request), **Sent**
(each request, Select cancels after asking), **Blocked** (Select unblocks
after asking). Decline and Remove ask with two choices each: Decline or
Decline and block, Remove or Remove and block (`ConfirmRow`).

Where the board is harder than drawn: the tab's request count is a pill on
the tab; `v2::Tabs` draws labels only, so the label reads "Friends · 2"
until the kit's tabs take a count.

Without a Discord account the tab is one card, "Friends need a Discord
account", and a row opening Settings, Online, Discord.

**Where the tab opens from.** Each of these opens the Profile strip on its
Friends tab (screen `friends`), so Back returns to where it was opened:

| Entry | What it shows |
|---|---|
| Home's Friends row, after Online play | "1 request" or "{n} requests" (highlighted) while requests wait, else "{n} online" (friends who are not offline, from the last list read), else nothing |
| Online play's Friends row, after Public rooms | no value |
| a room's More menu, Friends, after Room settings (in a room only) | Back returns to the room |
| Notifications' Friends row, after the waiting requests and invites | shown whenever the service offers friends, not only when something waits |
| a request toast's View | the tab with that request focused |
| LB / RB from the other Profile tabs | |

Home's Profile row opens Card, always.

### In a room

The member popout and the member's page (`PLAYER_CARD.md`) offer Add friend
(or the state: Request sent, Friends, disabled) and Block, for a member with
a card token in a public room. Add friend sends `request` with the token and
the member's room name as `label`.

### Room privacy

A private room has a Privacy setting, Friends or Hidden
(`room::Snapshot::friendsHidden`). Friends is the default: the friends of
anyone in the room see it and can join while it is offered (Presence). Hidden
shows them only "In a private room", and no invitation is handed out for it.

- **Where it is chosen.** Create shows a Privacy row after Visibility when the
  room is private (row `privacy`). The room defaults screen has the same row
  and keeps the choice for the private rooms created later. The host changes
  it in Room settings (row `room-privacy`, `ActionKind::SetFriendsHidden`):
  host only, and only in a room the host opened itself. A room its service
  owns, or one bound to a tournament match, has no row and refuses the action.
  It changes what the friends of members are shown and nothing about play.
- **What is saved.** The room snapshot writes `friends_hidden` only when it is
  true, so the default writes nothing. The saved defaults and a template keep
  `friendsHidden` under `roomDefaults` in the player's preferences, again only
  when true, and a reopened room keeps its setting. A private room, a saved
  default or a template from before the setting reads as Friends.
- **Older hosts.** The action kind is appended, so an authority from before it
  rejects it as unknown.

### Toasts

With buttons, outside a fight (`ToastAction`): a request ("Player 9 wants to
be friends": Accept, View, Later), an invite ("Player 3 invites you to their
room": Join, Dismiss). Select is the first button and Back the last, so Select
accepts the request and Back is Later, which only puts the toast away. View
opens the Friends tab with that request focused. No toast when a friend comes
online.

### Settings, Online

A Friends panel beside Discord and Rankings (`ApplicationShell__Settings.cxx`,
`tab-online`): Appear offline, Friend code (Select: New code, after asking),
Turn off friends (after asking: says what is deleted; blocks are kept). While they are off the panel says so and offers Turn on.

## Strings

Added to all fourteen catalogs, as for `PLAYER_CARD.md`.

| Key | English |
|---|---|
| `profile.tab.friends` | Friends |
| `friends.tab_count` | Friends · {0} |
| `friends.in_room` | In a room · {0} |
| `friends.online` | Online · {0} |
| `friends.offline` | Offline · {0} |
| `friends.playing_table` | Playing at table {0}, {1} |
| `friends.watching_table` | Watching table {0}, {1} |
| `friends.waiting_room` | Waiting in {0} |
| `friends.training_room` | In Training in {0} |
| `friends.private_room` | In a private room |
| `friends.menus` | At the main menu |
| `friends.offline_play` | Playing offline |
| `friends.last_online` | Last online {0} |
| `friends.other_version` | On another version |
| `friends.join` | Join |
| `friends.join_room` | Join their room |
| `friends.watch` | Watch |
| `friends.invite` | Invite |
| `friends.invite_room` | Invite to my room |
| `friends.remove` | Remove friend |
| `friends.remove_block` | Remove and block |
| `friends.accept` | Accept |
| `friends.decline` | Decline |
| `friends.decline_block` | Decline and block |
| `friends.block` | Block |
| `friends.unblock` | Unblock |
| `friends.add` | Add a friend |
| `friends.add_member` | Add friend |
| `friends.request_sent` | Request sent |
| `friends.are_friends` | Friends |
| `friends.played_you` | Played you {0} games, last {1} |
| `friends.record` | You and {0} |
| `friends.last_played` | Last played |
| `friends.since` | Friend since {0} |
| `friends.code` | Friend code |
| `friends.their_code` | Their friend code |
| `friends.new_code` | New code |
| `friends.send` | Send request |
| `friends.sent` | Sent ({0}) |
| `friends.blocked` | Blocked ({0}) |
| `friends.appear_offline` | Appear offline |
| `friends.appearing_offline` | Appearing offline |
| `friends.invite_reveals` | Your invite shows this friend where you are. |
| `friends.turn_off` | Turn off friends |
| `friends.turn_off_detail` | Removes your friends and requests and retires your friend code. Your blocks stay. |
| `friends.no_account` | Friends need a Discord account. Connect it in Settings, Online. |
| `friends.unavailable` | This service does not offer friends yet. |
| `friends.toast_request` | {0} wants to be friends |
| `friends.toast_invite` | {0} invites you to their room |
| `friends.unknown_code` | No player has that friend code. |
| `friends.full` | That friend list is full. |
| `friends.request_limit` | You have sent enough requests for today. |
| `home.friends` | Friends |
| `home.friends_detail` | Join friends, answer requests and add new friends. |
| `online.friends_detail` | Friends who are online, and the rooms they are in. |
| `room.v2.friends_detail` | Invite friends to this room, or join theirs. |
| `notifications.friends_detail` | Your friends, requests and blocked players. |
| `friends.home_online` | {0} online |
| `friends.home_request_one` | 1 request |
| `friends.home_requests` | {0} requests |
| `friends.view` | View |
| `friends.playing_game` | Playing {0} |
| `friends.room_game` | In a room · {0} |
| `friends.another_game` | Playing another game |
| `friends.join_unavailable` | That room cannot be joined now. |
| `room.privacy` | Privacy |
| `room.privacy.friends` | Friends |
| `room.privacy.hidden` | Hidden |
| `room.privacy.detail` | Friends: friends of anyone in the room see it and can join while it is open. Hidden: they see only that you are in a private room, and join by invite. |
| `room.privacy.settings_detail` | Players who already joined can come back with their invitation. Set Admission to Locked to stop anyone joining. |

## Another game's copy

A second game's client (SFV Ember keeps its own copy) takes the friends code
as it is and changes one file.

- **Shared and game-neutral**: `server/ember/protocol/src/friends.rs`
  (`DEFAULT_GAME` is the one place a game is named), the bridge's
  `src/friends/*` and `src/routes/friends.rs`, and the helper's
  `rust/sf4-net/src/tournament/friends.rs`. None of them, nor the feature name
  or a string id, names a game besides that default.
- **Taken unchanged**: `src/netplay/{Friends,FriendsView,FriendPresence,
  FriendVisits,FriendWire,FriendCommands,FriendRequests}.hxx`,
  `src/session/SocialAnswers.{hxx,cxx}`, `src/discord/{Presence,Ticket}.hxx`,
  `src/sf4e/sf4e__NetplayRuntime__Social.cxx`, `src/ui/FriendsPanel.{hxx,cxx}`
  and `src/ui/FriendsBoard.cxx`, and the room setting's parts
  (`friendsHidden` in `src/session/{RoomTypes.hxx,RoomModel.cxx,
  RoomModelActions.cxx,RoomModelJson.cxx,RoomAuthority.hxx,RoomQueries.hxx}`,
  `src/netplay/{PlayerPreferences,RoomPreferences}.hxx`, the Privacy rows).
- **Changed**: `src/netplay/GameIdentity.hxx`: `GameId`, and a line in
  `GameTitle` for a game whose id it does not list (it lists `usf4`, `sfv` and
  `umvc3`).
- **Not done by copying**: friends that span games (Open questions).

## Work packages

Numbering is shared with `FIGHTER_LEVELS.md` (P0 to P5) and
`PLAYER_CARD.md` (P6, P7). P0 defines the friends types; P4 their helper ops
(`friends_sync`, `friends_act`) and runtime.

| Package | Owns | Depends | Tests | Only in the game |
|---|---|---|---|---|
| P3 Bridge friends | `migrations/061_friends.sql`, `src/db/migrations.rs` (one line), `src/friends/{mod.rs,store.rs,presence.rs}`, `src/routes/friends.rs`, `src/routes/{mod.rs,discovery.rs,levels.rs}` (routes, feature, `relation` and the `friend` selector), `src/lib.rs` (presence and invites in `AppState`), `src/routes/expiry.rs` (the request sweep), `tests/friends.rs` | P2 | request by card and code; mutual request accepts; hidden requests look sent; daily, pending and inbox limits; block removes everything both ways and hides from cards; forget; joinable by build; appear offline freezes `seen_at`; no Discord ID in any answer (a test that scans every answer for the seeded IDs) | none |
| P8 Friends interface | `src/ui/{FriendsPanel.hxx,FriendsPanel.cxx}`, `src/ui/ProfilePanel.cxx` (the tab), `src/ui/ShellView.hxx` (`friends`), `src/sf4e/sf4e__Overlay.cxx` (mapping), `src/ui/ApplicationShell__Settings.cxx` (Friends panel), `src/ui/ApplicationShell__Toasts.cxx` (request and invite toasts), `src/ui/{RoomPanel.cxx,RoomActions.cxx}` (Add friend, Block), `src/ui/lab/*`, `locales/*.po` | P3, P4, P6, P7 | lab: no account, unavailable, empty, full groups, requests, appear offline, narrow; navigation from a row into the panel and back | two PCs: request from a room, accept from a toast, join and watch a public room, invite into a private room, appear offline, remove |

Friends ship last: the levels slice and the room's cards do not need them.

## Open questions

| Question | Recommendation | Cost to change later |
|---|---|---|
| Friend codes, beside adding from a room? | Yes: without them, two players can befriend only after sharing a public room. | Dropping them: a column and a page. |
| Join a friend's private room from presence, without an invite? | Yes, decided by the owner on 2026-11-14: by the room's Privacy setting (Friends, the default, or Hidden). The invitation is handed out only when the friend presses Join; it never sits in presence. | The offer, the `join` action and the setting; dropping them returns private rooms to invites only. |
| Friends across games? | Undecided. This bridge serves one game's clients today, and the wire names the game from now on (`game`, absent is `usf4`), so a friend in another game is already shown with it and cannot be joined. A second game shows up in the list once its client syncs to the same friends service: it calls this bridge's friends routes, or the bridges share one friends service. | `main` (the fighter) is stored per account with one game's roster indices (0 to 43) and becomes per game then. |
| Tell friends when someone comes online? | No: noise. | A toast and a setting. |
| Friend cap 200? | Yes, to bound the state's size (about 50 KB). | A constant; past about 500 the state needs paging. |
| Show friends' levels when they do not take part in ranking? | Yes, decided: a card is how players find each other to add friends, so it is public, here and in rooms (`FIGHTER_LEVELS.md`, Who sees what). | The same read filter as rooms. |
| A recent-players list to add from after leaving a room? | Later: card tokens already last 7 days, so the client can keep them. | Client only. |
| Friends' presence on the Discord bot or website? | Not in 1.6.0. | A read route; the data is there. |
