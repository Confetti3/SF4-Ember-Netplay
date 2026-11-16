-- Friends (docs/design/FRIENDS.md): a Discord account's friends, requests and
-- blocks. Nothing here is ever served by Discord user ID; every row is named
-- to players by a random prefixed id.

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
-- A hidden request its sender cancelled has `withdrawn_at`: the sender no
-- longer sees it, and it still holds the pair until it lapses.
CREATE TABLE friend_requests (
    id TEXT PRIMARY KEY,
    sender TEXT NOT NULL,
    receiver TEXT NOT NULL,
    label TEXT NOT NULL,
    sent_at INTEGER NOT NULL,
    hidden_at INTEGER,
    withdrawn_at INTEGER,
    UNIQUE (sender, receiver)
) WITHOUT ROWID;
CREATE INDEX friend_requests_receiver ON friend_requests(receiver, sent_at);
CREATE INDEX friend_requests_sent ON friend_requests(sent_at);

-- Accounts a player blocked, with the name to list them under. Read by the
-- key alone: by blocker, and by (blocker, blocked) on every request and card.
CREATE TABLE friend_blocks (
    blocker TEXT NOT NULL,
    blocked TEXT NOT NULL,
    id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    at INTEGER NOT NULL,
    PRIMARY KEY (blocker, blocked)
) WITHOUT ROWID;
