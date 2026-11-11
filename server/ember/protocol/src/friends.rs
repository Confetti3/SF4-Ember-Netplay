//! Friends: the routes a player's game calls to sync its presence and to act
//! on its friend list, and the `FriendsState` both answer
//! (`docs/design/FRIENDS.md`). The list belongs to the bridge, by Discord
//! account; no answer here carries a Discord ID or name, an Ember ID or a
//! rating.
//!
//! Requests are strict and answers are tolerant. A request (`FriendsSync` and
//! its parts, `FriendAction`) refuses unknown members, so a client's mistake
//! is caught at the bridge. An answer (`FriendsState` and its parts) ignores
//! members it does not know, so a field a later bridge adds does not break a
//! client already out; the exceptions are `SkillBadge` and `InviteRoom`, which
//! other answers share. A build asks only a bridge that lists
//! [`FRIENDS_FEATURE`], and an older bridge answers 404.
use serde::{Deserialize, Serialize};

use crate::{
    Error, Result,
    encoding::{is_false, is_prefixed_id, is_single_line},
    levels::{FIGHTERS, MAX_TIER, is_card_token},
    play::{check_build, is_hex},
    rooms::{MAX_CAPACITY, MAX_NAME, MIN_CAPACITY, check_room_name, is_country, is_player_name},
    skill::{Listing, SkillBadge},
    wire_enum::wire_enum,
};

/// The capability a bridge with the friends routes offers, and only beside
/// `levels`.
pub const FRIENDS_FEATURE: &str = "friends";
/// `POST` a `FriendsSync`, answered with the `FriendsState`.
pub const SYNC_PATH: &str = "/v1/friends/sync";
/// `POST` a `FriendAction`, answered with the `FriendsState` after it.
pub const ACTIONS_PATH: &str = "/v1/friends/actions";

/// The most friends one account has.
pub const MAX_FRIENDS: usize = 200;
/// The most requests a state shows as received.
pub const MAX_INCOMING: usize = 50;
/// The most accounts one account blocks.
pub const MAX_BLOCKS: usize = 500;
/// The most live invites an account holds.
pub const MAX_INVITES: usize = 20;
/// The highest table number a room has.
pub const MAX_TABLE: u8 = 7;
/// The longest friend code a request may carry as typed, in bytes.
pub const MAX_TYPED_CODE: usize = 32;
/// The length of a room's own invitation, the `emd1:` or `emd2:` token a
/// Discord join secret carries.
pub const SECRET_LEN: usize = 127;

/// Refusal reasons of the friends routes (`details.reason`). A request for a
/// caller without a Discord account is `skill::NO_ACCOUNT`.
pub const UNKNOWN_CODE: &str = "unknown_code";
pub const CARD_EXPIRED: &str = "card_expired";
pub const OWN_CARD: &str = "own_card";
pub const FRIENDS_FULL: &str = "friends_full";
pub const REQUEST_LIMIT: &str = "request_limit";
pub const CODE_TOO_SOON: &str = "code_too_soon";
/// Not a friend, or the friend is offline or appears offline.
pub const INVITE_UNAVAILABLE: &str = "invite_unavailable";

/// Crockford base32: the digits and the letters but I, L, O and U.
const CODE_SYMBOLS: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Whether `text` is a friend code as the bridge writes it: `XXXX-XXXX` in
/// upper-case Crockford base32.
pub fn is_friend_code(text: &str) -> bool {
    let bytes = text.as_bytes();
    bytes.len() == 9
        && bytes[4] == b'-'
        && bytes
            .iter()
            .enumerate()
            .all(|(index, byte)| index == 4 || CODE_SYMBOLS.contains(byte))
}

/// The code a player typed, in the form the bridge writes it: any case,
/// spaces and dashes anywhere, `O` read as 0 and `I` or `L` as 1. `None` for
/// anything that is not eight symbols after that.
pub fn parse_friend_code(typed: &str) -> Option<String> {
    if typed.len() > MAX_TYPED_CODE {
        return None;
    }
    let symbols: Option<Vec<u8>> = typed
        .chars()
        .filter(|symbol| !symbol.is_whitespace() && *symbol != '-')
        .map(|symbol| match symbol.to_ascii_uppercase() {
            'O' => Some(b'0'),
            'I' | 'L' => Some(b'1'),
            upper if upper.is_ascii() && CODE_SYMBOLS.contains(&(upper as u8)) => Some(upper as u8),
            _ => None,
        })
        .collect();
    let symbols = String::from_utf8(symbols.filter(|symbols| symbols.len() == 8)?).ok()?;
    Some(format!("{}-{}", &symbols[..4], &symbols[4..]))
}

/// Whether `text` is shaped like a room's own invitation: `emd1:` or `emd2:`
/// and 122 characters of the unpadded base64url alphabet. The bridge checks
/// only this; the room host reads the rest.
pub fn is_invitation_secret(text: &str) -> bool {
    text.len() == SECRET_LEN
        && (text.starts_with("emd1:") || text.starts_with("emd2:"))
        && text.as_bytes()[5..]
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || *byte == b'-' || *byte == b'_')
}

fn check_id(text: &str, prefix: &str, field: &'static str) -> Result<()> {
    if is_prefixed_id(text, prefix) {
        Ok(())
    } else {
        Err(Error::InvalidField(field))
    }
}

fn check_table(table: Option<u8>) -> Result<()> {
    if table.is_some_and(|table| table > MAX_TABLE) {
        Err(Error::InvalidField("table"))
    } else {
        Ok(())
    }
}

fn check_country(country: Option<&str>) -> Result<()> {
    if country.is_none_or(is_country) {
        Ok(())
    } else {
        Err(Error::InvalidField("country"))
    }
}

fn check_name(name: &str) -> Result<()> {
    if is_player_name(name) {
        Ok(())
    } else {
        Err(Error::InvalidField("name"))
    }
}

/// What a sender saw when they asked: a room name or the code they typed.
fn check_label(label: &str) -> Result<()> {
    if is_single_line(label, MAX_NAME) {
        Ok(())
    } else {
        Err(Error::InvalidField("label"))
    }
}

fn check_card_and_label(card: &str, label: &str) -> Result<()> {
    if !is_card_token(card) {
        return Err(Error::InvalidField("card"));
    }
    check_label(label)
}

wire_enum! {
    serde
    /// What a player in a room is doing there.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum RoomStatus {
        Idle = "idle",
        Queued = "queued",
        Ready = "ready",
        Fighting = "fighting",
        Watching = "watching",
        Training = "training",
    }
}

wire_enum! {
    serde
    /// What an online player outside a room is doing.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum Activity {
        Menus = "menus",
        /// Training, Arcade or versus CPU.
        OfflinePlay = "offline_play",
    }
}

/// The room a client says it is in. A public room is named by its id; a
/// private room only by `private`. The bridge keeps a `room_id` that is not
/// an open, listed room as "in a room" without one.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RoomReport {
    /// 32 hex digits; with `private` absent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub room_id: Option<String>,
    /// A private room, named by nothing. Written only when true.
    #[serde(default, skip_serializing_if = "is_false")]
    pub private: bool,
    pub status: RoomStatus,
    /// 0 to `MAX_TABLE`, when seated or watching.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub table: Option<u8>,
}

impl RoomReport {
    pub fn check(&self) -> Result<()> {
        if self.room_id.is_some() == self.private {
            return Err(Error::InvalidField("room"));
        }
        if self.room_id.as_deref().is_some_and(|id| !is_hex(id, 32)) {
            return Err(Error::InvalidField("room_id"));
        }
        check_table(self.table)
    }
}

/// The presence a client sends: what its player is doing now.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "activity", rename_all = "snake_case", deny_unknown_fields)]
pub enum PresenceReport {
    Menus {},
    OfflinePlay {},
    Room {
        room: RoomReport,
    },
    /// Leaving now: the bridge forgets the presence at once.
    Offline {},
}

impl PresenceReport {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Room { room } => room.check(),
            Self::Menus {} | Self::OfflinePlay {} | Self::Offline {} => Ok(()),
        }
    }
}

/// `POST /v1/friends/sync`: the client's presence in, the state out.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FriendsSync {
    /// As in a ticket request: decides whether a friend's public room is
    /// joinable for this caller.
    pub build_id: String,
    /// What friends see as the name and flag.
    pub listing: Listing,
    /// The player's main fighter, 0 to `FIGHTERS` - 1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<u8>,
    pub presence: PresenceReport,
}

impl FriendsSync {
    pub fn check(&self) -> Result<()> {
        check_build(&self.build_id)?;
        self.listing.check()?;
        if self.main.is_some_and(|main| main >= FIGHTERS) {
            return Err(Error::InvalidField("main"));
        }
        self.presence.check()
    }
}

/// A room an invite or a friend's presence offers: a public room by its id,
/// or a private room by its own invitation.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum InviteRoom {
    /// 32 hex digits.
    RoomId(String),
    /// A room's `emd1:` or `emd2:` invitation (`is_invitation_secret`). The
    /// bridge keeps it in memory only.
    Secret(String),
}

impl InviteRoom {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::RoomId(id) if is_hex(id, 32) => Ok(()),
            Self::Secret(secret) if is_invitation_secret(secret) => Ok(()),
            Self::RoomId(_) => Err(Error::InvalidField("room_id")),
            Self::Secret(_) => Err(Error::InvalidField("secret")),
        }
    }
}

/// `POST /v1/friends/actions`: one action in, the state after it out.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "action", rename_all = "snake_case", deny_unknown_fields)]
pub enum FriendAction {
    /// Asks the account behind a card token (`card` and `label`) or a friend
    /// code (`code`, as typed: `parse_friend_code`), and exactly one of the
    /// two. When that account already asked the caller, it is accepted
    /// instead.
    Request {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        card: Option<String>,
        /// What the sender saw: a room name.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        label: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        code: Option<String>,
    },
    /// Makes the friendship (`rq_`).
    Accept { request: String },
    /// Hides a request from its receiver; with `block`, blocks the sender.
    Decline {
        request: String,
        #[serde(default, skip_serializing_if = "is_false")]
        block: bool,
    },
    /// The sender takes a request back.
    Cancel { request: String },
    /// Ends a friendship (`fr_`), silently; with `block`, blocks them.
    Remove {
        friend: String,
        #[serde(default, skip_serializing_if = "is_false")]
        block: bool,
    },
    /// Blocks a room member by their card token.
    Block { card: String, label: String },
    /// Lifts a block (`bl_`).
    Unblock { block: String },
    /// Offers a friend the caller's room for 10 minutes.
    Invite { friend: String, room: InviteRoom },
    /// Drops an invite the caller received (`iv_`).
    Dismiss { invite: String },
    /// Friends see the caller as offline.
    AppearOffline { on: bool },
    /// A new friend code, at most once a day; the old one stops working.
    NewCode {},
    /// Turns friends off: the caller's settings, code, friendships, requests
    /// both ways and blocks are deleted.
    Forget {},
}

impl FriendAction {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Request { card, label, code } => {
                match (card.as_deref(), label.as_deref(), code.as_deref()) {
                    (Some(card), Some(label), None) => check_card_and_label(card, label),
                    (None, None, Some(code)) if parse_friend_code(code).is_some() => Ok(()),
                    (None, None, Some(_)) => Err(Error::InvalidField("code")),
                    _ => Err(Error::InvalidField("request")),
                }
            }
            Self::Accept { request } | Self::Decline { request, .. } | Self::Cancel { request } => {
                check_id(request, "rq", "request")
            }
            Self::Remove { friend, .. } => check_id(friend, "fr", "friend"),
            Self::Block { card, label } => check_card_and_label(card, label),
            Self::Unblock { block } => check_id(block, "bl", "block"),
            Self::Invite { friend, room } => {
                check_id(friend, "fr", "friend")?;
                room.check()
            }
            Self::Dismiss { invite } => check_id(invite, "iv", "invite"),
            Self::AppearOffline { .. } | Self::NewCode {} | Self::Forget {} => Ok(()),
        }
    }
}

/// A friend's main fighter.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MainFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// At least 1; absent while the friend does not show points. Present
    /// exactly when `tier` is.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub points: Option<u64>,
    /// 0 to `MAX_TIER`: the frame colour of `points`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tier: Option<u8>,
}

impl MainFighter {
    pub fn check(&self) -> Result<()> {
        if self.fighter >= FIGHTERS {
            return Err(Error::InvalidField("fighter"));
        }
        if self.points == Some(0) {
            return Err(Error::InvalidField("points"));
        }
        if self.points.is_some() != self.tier.is_some()
            || self.tier.is_some_and(|tier| tier > MAX_TIER)
        {
            return Err(Error::InvalidField("tier"));
        }
        Ok(())
    }
}

/// A public room a friend is in.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FriendRoom {
    /// 32 hex digits.
    pub room_id: String,
    pub name: String,
    pub members: u8,
    pub capacity: u8,
    /// Open, not locked, not full, and of the caller's build.
    pub joinable: bool,
}

impl FriendRoom {
    pub fn check(&self) -> Result<()> {
        if !is_hex(&self.room_id, 32) {
            return Err(Error::InvalidField("room_id"));
        }
        check_room_name(&self.name)?;
        if !(MIN_CAPACITY..=MAX_CAPACITY).contains(&self.capacity) {
            return Err(Error::InvalidField("capacity"));
        }
        if self.members > self.capacity {
            return Err(Error::InvalidField("members"));
        }
        Ok(())
    }
}

/// Where a friend is, as the caller may see it. A friend who appears offline
/// is `Offline`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum FriendPresence {
    Offline {
        /// When they were last seen (unix seconds), when known.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        seen_at: Option<u64>,
    },
    Online {
        activity: Activity,
    },
    /// In a room: a public one by `room`, a private one by `private`, or, for
    /// a public room that is no longer open and listed, neither.
    Room {
        status: RoomStatus,
        /// 0 to `MAX_TABLE`, when seated or watching.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        table: Option<u8>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        room: Option<FriendRoom>,
        /// Written only when true.
        #[serde(default, skip_serializing_if = "is_false")]
        private: bool,
    },
}

impl FriendPresence {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Offline { .. } | Self::Online { .. } => Ok(()),
            Self::Room {
                table,
                room,
                private,
                ..
            } => {
                if room.is_some() && *private {
                    return Err(Error::InvalidField("room"));
                }
                check_table(*table)?;
                room.as_ref().map_or(Ok(()), FriendRoom::check)
            }
        }
    }
}

/// The wins between the caller and another account, from the caller's side,
/// and their last game. Absent before their first counted game.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PairRecord {
    pub won: u64,
    pub lost: u64,
    /// Unix seconds.
    pub last_at: u64,
}

impl PairRecord {
    pub fn check(&self) -> Result<()> {
        if self.won.saturating_add(self.lost) == 0 {
            return Err(Error::InvalidField("record"));
        }
        Ok(())
    }
}

/// How often the caller and a request's sender have played. Absent before
/// their first counted game.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Played {
    pub games: u64,
    /// Unix seconds.
    pub last_at: u64,
}

impl Played {
    pub fn check(&self) -> Result<()> {
        if self.games == 0 {
            return Err(Error::InvalidField("played"));
        }
        Ok(())
    }
}

/// One friend.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Friend {
    /// `fr_`: the name both sides use for the friendship.
    pub id: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    /// When they became friends (unix seconds).
    pub since: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<MainFighter>,
    /// Their class as last shown, or the developer mark; absent when they do
    /// not take part in ranking, or classes are hidden.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub badge: Option<SkillBadge>,
    pub presence: FriendPresence,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub record: Option<PairRecord>,
}

impl Friend {
    pub fn check(&self) -> Result<()> {
        check_id(&self.id, "fr", "id")?;
        check_name(&self.name)?;
        check_country(self.country.as_deref())?;
        self.main.as_ref().map_or(Ok(()), MainFighter::check)?;
        self.badge.as_ref().map_or(Ok(()), SkillBadge::check)?;
        self.presence.check()?;
        self.record.as_ref().map_or(Ok(()), PairRecord::check)
    }
}

/// A request the caller received.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct IncomingRequest {
    /// `rq_`.
    pub id: String,
    /// The sender's own name.
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    pub sent_at: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub played: Option<Played>,
}

impl IncomingRequest {
    pub fn check(&self) -> Result<()> {
        check_id(&self.id, "rq", "id")?;
        check_name(&self.name)?;
        check_country(self.country.as_deref())?;
        self.played.as_ref().map_or(Ok(()), Played::check)
    }
}

/// A request the caller sent, including one that was declined or could never
/// be seen, which stays "sent".
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct OutgoingRequest {
    /// `rq_`.
    pub id: String,
    /// What the sender saw: a room name or the code they typed.
    pub label: String,
    pub sent_at: u64,
}

impl OutgoingRequest {
    pub fn check(&self) -> Result<()> {
        check_id(&self.id, "rq", "id")?;
        check_label(&self.label)
    }
}

/// An invite the caller received and that is still live.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReceivedInvite {
    /// `iv_`.
    pub id: String,
    /// The inviting friend (`fr_`).
    pub friend: String,
    /// The room as it was sent.
    pub room: InviteRoom,
    pub expires_at: u64,
}

impl ReceivedInvite {
    pub fn check(&self) -> Result<()> {
        check_id(&self.id, "iv", "id")?;
        check_id(&self.friend, "fr", "friend")?;
        self.room.check()
    }
}

/// An account the caller blocked, with the name to list it under.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BlockedPlayer {
    /// `bl_`.
    pub id: String,
    pub name: String,
    pub at: u64,
}

impl BlockedPlayer {
    pub fn check(&self) -> Result<()> {
        check_id(&self.id, "bl", "id")?;
        check_name(&self.name)
    }
}

/// Everything a client shows of its friends. Both routes answer the whole
/// state, and the client replaces its own with it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct FriendsState {
    /// The caller's friend code (`is_friend_code`).
    pub code: String,
    /// Friends see the caller as offline. Written only when on.
    #[serde(default, skip_serializing_if = "is_false")]
    pub appear_offline: bool,
    /// At most `MAX_FRIENDS`, in no order.
    pub friends: Vec<Friend>,
    /// At most `MAX_INCOMING` visible.
    pub incoming: Vec<IncomingRequest>,
    pub outgoing: Vec<OutgoingRequest>,
    /// Received and live: at most `MAX_INVITES`.
    pub invites: Vec<ReceivedInvite>,
    /// At most `MAX_BLOCKS`.
    pub blocked: Vec<BlockedPlayer>,
}

impl FriendsState {
    pub fn check(&self) -> Result<()> {
        if !is_friend_code(&self.code) {
            return Err(Error::InvalidField("code"));
        }
        let bounded = [
            (self.friends.len() <= MAX_FRIENDS, "friends"),
            (self.incoming.len() <= MAX_INCOMING, "incoming"),
            (self.invites.len() <= MAX_INVITES, "invites"),
            (self.blocked.len() <= MAX_BLOCKS, "blocked"),
        ];
        if let Some((_, field)) = bounded.iter().find(|(within, _)| !within) {
            return Err(Error::InvalidField(field));
        }
        self.friends.iter().try_for_each(Friend::check)?;
        self.incoming.iter().try_for_each(IncomingRequest::check)?;
        self.outgoing.iter().try_for_each(OutgoingRequest::check)?;
        self.invites.iter().try_for_each(ReceivedInvite::check)?;
        self.blocked.iter().try_for_each(BlockedPlayer::check)
    }
}

#[cfg(test)]
mod tests {
    use serde::de::DeserializeOwned;
    use serde_json::{Value, json};

    use super::*;

    const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";
    const REQUEST: &str = "rq_22222222-2222-4222-8222-222222222222";
    const INVITE: &str = "iv_33333333-3333-4333-8333-333333333333";
    const BLOCK: &str = "bl_44444444-4444-4444-8444-444444444444";
    const ROOM: &str = "0f0123456789abcdef0123456789abcd";
    const TOKEN: &str = "c1.abcdEFGH-_09";

    fn secret() -> String {
        format!("emd1:{}", "A".repeat(122))
    }

    fn read<T: DeserializeOwned>(value: &Value) -> T {
        serde_json::from_value(value.clone()).unwrap()
    }

    /// Reads `value` as a `T`, checks it and writes it back: what the
    /// document shows must survive both unchanged.
    fn round_trips<T>(value: &Value, check: fn(&T) -> Result<()>) -> T
    where
        T: DeserializeOwned + Serialize,
    {
        let parsed: T = read(value);
        assert_eq!(check(&parsed), Ok(()), "{value}");
        assert_eq!(&serde_json::to_value(&parsed).unwrap(), value);
        parsed
    }

    /// Adds a member the type does not have at `pointer` and expects `T` to
    /// read the value as it reads the one without it.
    fn ignores_extra<T: DeserializeOwned + PartialEq + std::fmt::Debug>(
        value: &Value,
        pointer: &str,
    ) {
        let mut extended = value.clone();
        extended
            .pointer_mut(pointer)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .insert("extra".into(), json!(1));
        assert_eq!(read::<T>(&extended), read::<T>(value), "{pointer:?}");
    }

    /// Adds a member the type does not have at `pointer` and expects a refusal.
    fn refuses_extra<T: DeserializeOwned>(value: &Value, pointer: &str) {
        let mut value = value.clone();
        value
            .pointer_mut(pointer)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .insert("extra".into(), json!(1));
        assert!(
            serde_json::from_value::<T>(value).is_err(),
            "extra member at {pointer:?}"
        );
    }

    fn state_wire() -> Value {
        json!({
            "code": "4F2A-91CX",
            "friends": [{
                "id": FRIEND, "name": "Player 3", "country": "FR", "since": 1730073600,
                "main": {"fighter": 5, "points": 1986, "tier": 3},
                "badge": {"band": 7, "settled": true},
                "presence": {"state": "room", "status": "fighting", "table": 0,
                    "room": {"room_id": ROOM, "name": "Player 1's room",
                             "members": 7, "capacity": 16, "joinable": true}},
                "record": {"won": 14, "lost": 9, "last_at": 1731000000}}],
            "incoming": [{"id": REQUEST, "name": "Player 9", "country": "DE",
                "sent_at": 1731100000, "played": {"games": 6, "last_at": 1731090000}}],
            "outgoing": [{"id": REQUEST, "label": "Player 4", "sent_at": 1731100000}],
            "invites": [{"id": INVITE, "friend": FRIEND, "room": {"room_id": ROOM},
                "expires_at": 1731100600}],
            "blocked": [{"id": BLOCK, "name": "Player 7", "at": 1731000000}]
        })
    }

    #[test]
    fn the_state_in_the_design_round_trips() {
        let wire = state_wire();
        let state = round_trips::<FriendsState>(&wire, FriendsState::check);
        assert_eq!(state.friends.len(), 1);
        assert!(!state.appear_offline);
        assert_eq!(
            state.friends[0].presence,
            FriendPresence::Room {
                status: RoomStatus::Fighting,
                table: Some(0),
                room: Some(FriendRoom {
                    room_id: ROOM.into(),
                    name: "Player 1's room".into(),
                    members: 7,
                    capacity: 16,
                    joinable: true,
                }),
                private: false,
            }
        );
        assert_eq!(state.invites[0].room, InviteRoom::RoomId(ROOM.into()));

        // Members that are only written when set stay out of an empty state,
        // in the document's order.
        let empty = FriendsState {
            code: "4F2A-91CX".into(),
            appear_offline: true,
            friends: vec![],
            incoming: vec![],
            outgoing: vec![],
            invites: vec![],
            blocked: vec![],
        };
        assert!(empty.check().is_ok());
        assert_eq!(
            serde_json::to_string(&empty).unwrap(),
            r#"{"code":"4F2A-91CX","appear_offline":true,"friends":[],"incoming":[],"outgoing":[],"invites":[],"blocked":[]}"#
        );
    }

    #[test]
    fn every_state_member_ignores_what_it_does_not_know() {
        let wire = state_wire();
        for pointer in [
            "",
            "/friends/0",
            "/friends/0/main",
            "/friends/0/presence",
            "/friends/0/presence/room",
            "/friends/0/record",
            "/incoming/0",
            "/incoming/0/played",
            "/outgoing/0",
            "/invites/0",
            "/blocked/0",
        ] {
            ignores_extra::<FriendsState>(&wire, pointer);
        }
        // The badge and the invited room are types other answers share, and
        // they keep refusing what they do not know.
        for pointer in ["/friends/0/badge", "/invites/0/room"] {
            refuses_extra::<FriendsState>(&wire, pointer);
        }
        // A member of the state may not be left out, though.
        for member in [
            "code", "friends", "incoming", "outgoing", "invites", "blocked",
        ] {
            let mut short = wire.clone();
            short.as_object_mut().unwrap().remove(member);
            assert!(
                serde_json::from_value::<FriendsState>(short).is_err(),
                "{member}"
            );
        }
    }

    #[test]
    fn a_friends_presence_is_one_of_three_states() {
        let offline = round_trips::<FriendPresence>(
            &json!({"state": "offline", "seen_at": 1731000000}),
            FriendPresence::check,
        );
        assert_eq!(
            offline,
            FriendPresence::Offline {
                seen_at: Some(1731000000)
            }
        );
        round_trips::<FriendPresence>(&json!({"state": "offline"}), FriendPresence::check);
        round_trips::<FriendPresence>(
            &json!({"state": "online", "activity": "offline_play"}),
            FriendPresence::check,
        );
        round_trips::<FriendPresence>(
            &json!({"state": "room", "status": "training", "private": true}),
            FriendPresence::check,
        );
        // A public room that is no longer listed is "in a room" and no more.
        round_trips::<FriendPresence>(
            &json!({"state": "room", "status": "idle"}),
            FriendPresence::check,
        );
        for bad in [
            json!({"state": "away"}),
            json!({"state": "online"}),
            json!({"state": "online", "activity": "room"}),
            json!({"state": "online", "activity": "offline"}),
            json!({"state": "room"}),
            json!({"state": "room", "status": "asleep"}),
            json!({"status": "idle"}),
        ] {
            assert!(
                serde_json::from_value::<FriendPresence>(bad.clone()).is_err(),
                "{bad}"
            );
        }
        // An answer reads past a member a later bridge adds: offline with a
        // status is offline.
        assert!(matches!(
            serde_json::from_value::<FriendPresence>(json!({"state": "offline", "status": "idle"})),
            Ok(FriendPresence::Offline {})
        ));
        let private_with_room = FriendPresence::Room {
            status: RoomStatus::Idle,
            table: None,
            room: Some(FriendRoom {
                room_id: ROOM.into(),
                name: "Room".into(),
                members: 1,
                capacity: 2,
                joinable: false,
            }),
            private: true,
        };
        assert_eq!(private_with_room.check(), Err(Error::InvalidField("room")));
        let at_table = |table| FriendPresence::Room {
            status: RoomStatus::Fighting,
            table: Some(table),
            room: None,
            private: false,
        };
        assert!(at_table(MAX_TABLE).check().is_ok());
        assert_eq!(
            at_table(MAX_TABLE + 1).check(),
            Err(Error::InvalidField("table"))
        );
    }

    #[test]
    fn the_state_checks_its_members_and_bounds() {
        let good: FriendsState = read(&state_wire());
        assert!(good.check().is_ok());
        let friend = |change: &dyn Fn(&mut Friend)| {
            let mut state = good.clone();
            change(&mut state.friends[0]);
            state.check()
        };
        assert_eq!(
            friend(&|f| f.id = REQUEST.into()),
            Err(Error::InvalidField("id"))
        );
        assert_eq!(
            friend(&|f| f.name = "".into()),
            Err(Error::InvalidField("name"))
        );
        assert_eq!(
            friend(&|f| f.country = Some("fr".into())),
            Err(Error::InvalidField("country"))
        );
        assert_eq!(
            friend(&|f| f.main = Some(MainFighter {
                fighter: 44,
                points: None,
                tier: None
            })),
            Err(Error::InvalidField("fighter"))
        );
        assert_eq!(
            friend(&|f| f.main = Some(MainFighter {
                fighter: 4,
                points: Some(5),
                tier: Some(5)
            })),
            Err(Error::InvalidField("tier"))
        );
        // Points and tier come together.
        assert_eq!(
            friend(&|f| f.main = Some(MainFighter {
                fighter: 4,
                points: Some(5),
                tier: None
            })),
            Err(Error::InvalidField("tier"))
        );
        assert_eq!(
            friend(&|f| f.main = Some(MainFighter {
                fighter: 4,
                points: Some(0),
                tier: Some(0)
            })),
            Err(Error::InvalidField("points"))
        );
        assert!(
            friend(&|f| f.main = Some(MainFighter {
                fighter: 4,
                points: None,
                tier: None
            }))
            .is_ok()
        );
        assert_eq!(
            friend(&|f| f.badge = Some(SkillBadge::class(11, true))),
            Err(Error::InvalidField("badge"))
        );
        assert!(friend(&|f| f.badge = Some(SkillBadge::DEVELOPER)).is_ok());
        assert_eq!(
            friend(&|f| f.record = Some(PairRecord {
                won: 0,
                lost: 0,
                last_at: 1
            })),
            Err(Error::InvalidField("record"))
        );
        let full_room = |members, capacity| {
            friend(&|f| {
                f.presence = FriendPresence::Room {
                    status: RoomStatus::Idle,
                    table: None,
                    room: Some(FriendRoom {
                        room_id: ROOM.into(),
                        name: "Room".into(),
                        members,
                        capacity,
                        joinable: false,
                    }),
                    private: false,
                }
            })
        };
        assert!(full_room(16, 16).is_ok());
        assert_eq!(full_room(17, 16), Err(Error::InvalidField("members")));
        assert_eq!(full_room(1, 1), Err(Error::InvalidField("capacity")));

        let mut bad = good.clone();
        bad.incoming[0].played = Some(Played {
            games: 0,
            last_at: 1,
        });
        assert_eq!(bad.check(), Err(Error::InvalidField("played")));
        let mut bad = good.clone();
        bad.outgoing[0].label = "two\nlines".into();
        assert_eq!(bad.check(), Err(Error::InvalidField("label")));
        let mut bad = good.clone();
        bad.invites[0].room = InviteRoom::Secret("emd1:short".into());
        assert_eq!(bad.check(), Err(Error::InvalidField("secret")));
        let mut bad = good.clone();
        bad.blocked[0].id = FRIEND.into();
        assert_eq!(bad.check(), Err(Error::InvalidField("id")));
        for code in [
            "",
            "4F2A91CX",
            "4f2a-91cx",
            "4F2A-91CU",
            "4F2A-91C",
            "4F2A-91CXX",
        ] {
            let mut bad = good.clone();
            bad.code = code.into();
            assert_eq!(bad.check(), Err(Error::InvalidField("code")), "{code}");
        }

        let with = |count: usize, fill: &dyn Fn(&mut FriendsState, usize)| {
            let mut state = good.clone();
            fill(&mut state, count);
            state.check()
        };
        let friends = |state: &mut FriendsState, count| {
            state.friends = vec![state.friends[0].clone(); count];
        };
        let incoming = |state: &mut FriendsState, count| {
            state.incoming = vec![state.incoming[0].clone(); count];
        };
        let invites = |state: &mut FriendsState, count| {
            state.invites = vec![state.invites[0].clone(); count];
        };
        let blocked = |state: &mut FriendsState, count| {
            state.blocked = vec![state.blocked[0].clone(); count];
        };
        assert!(with(MAX_FRIENDS, &friends).is_ok());
        assert_eq!(
            with(MAX_FRIENDS + 1, &friends),
            Err(Error::InvalidField("friends"))
        );
        assert!(with(MAX_INCOMING, &incoming).is_ok());
        assert_eq!(
            with(MAX_INCOMING + 1, &incoming),
            Err(Error::InvalidField("incoming"))
        );
        assert!(with(MAX_INVITES, &invites).is_ok());
        assert_eq!(
            with(MAX_INVITES + 1, &invites),
            Err(Error::InvalidField("invites"))
        );
        assert!(with(MAX_BLOCKS, &blocked).is_ok());
        assert_eq!(
            with(MAX_BLOCKS + 1, &blocked),
            Err(Error::InvalidField("blocked"))
        );
    }

    #[test]
    fn a_sync_in_the_design_round_trips_and_is_checked() {
        let wire = json!({"build_id": "build", "listing": {"name": "Player 1", "country": "GB"},
            "main": 3,
            "presence": {"activity": "room",
                "room": {"room_id": ROOM, "status": "fighting", "table": 0}}});
        let sync = round_trips::<FriendsSync>(&wire, FriendsSync::check);
        assert_eq!(sync.main, Some(3));
        for pointer in ["", "/listing", "/presence", "/presence/room"] {
            refuses_extra::<FriendsSync>(&wire, pointer);
        }

        for presence in [
            json!({"activity": "menus"}),
            json!({"activity": "offline_play"}),
            json!({"activity": "offline"}),
            json!({"activity": "room", "room": {"private": true, "status": "watching", "table": 7}}),
        ] {
            let wire = json!({"build_id": "build", "listing": {"name": "Player 1"},
                "presence": presence});
            let sync = round_trips::<FriendsSync>(&wire, FriendsSync::check);
            assert_eq!(sync.main, None, "main is left out when there is none");
        }

        let with = |change: &dyn Fn(&mut FriendsSync)| {
            let mut sync = sync.clone();
            change(&mut sync);
            sync.check()
        };
        assert_eq!(
            with(&|s| s.build_id = "".into()),
            Err(Error::InvalidField("build_id"))
        );
        assert_eq!(
            with(&|s| s.listing.name = "".into()),
            Err(Error::InvalidField("name"))
        );
        assert_eq!(
            with(&|s| s.main = Some(44)),
            Err(Error::InvalidField("main"))
        );
        assert!(with(&|s| s.main = Some(43)).is_ok());
        let in_room =
            |room: RoomReport| with(&|s| s.presence = PresenceReport::Room { room: room.clone() });
        let report = RoomReport {
            room_id: Some(ROOM.into()),
            private: false,
            status: RoomStatus::Ready,
            table: Some(1),
        };
        assert!(in_room(report.clone()).is_ok());
        for (bad, field) in [
            // A room is a public id or private, never both and never neither.
            (
                RoomReport {
                    private: true,
                    ..report.clone()
                },
                "room",
            ),
            (
                RoomReport {
                    room_id: None,
                    ..report.clone()
                },
                "room",
            ),
            (
                RoomReport {
                    room_id: Some("abc".into()),
                    ..report.clone()
                },
                "room_id",
            ),
            (
                RoomReport {
                    room_id: Some(ROOM.to_uppercase()),
                    ..report.clone()
                },
                "room_id",
            ),
            (
                RoomReport {
                    table: Some(8),
                    ..report.clone()
                },
                "table",
            ),
        ] {
            assert_eq!(in_room(bad), Err(Error::InvalidField(field)), "{field}");
        }

        // A presence names its activity, and a room only with `room`.
        for bad in [
            json!({"activity": "room"}),
            json!({"activity": "menus", "room": {"private": true, "status": "idle"}}),
            json!({"activity": "away"}),
            json!({"room": {"private": true, "status": "idle"}}),
            json!({"activity": "room", "room": {"private": true, "status": "sleeping"}}),
        ] {
            assert!(
                serde_json::from_value::<PresenceReport>(bad.clone()).is_err(),
                "{bad}"
            );
        }
        // The sync wants all of its required members.
        for member in ["build_id", "listing", "presence"] {
            let mut short = wire.clone();
            short.as_object_mut().unwrap().remove(member);
            assert!(
                serde_json::from_value::<FriendsSync>(short).is_err(),
                "{member}"
            );
        }
    }

    #[test]
    fn each_action_has_the_documents_shape() {
        let secret = secret();
        let cases = [
            json!({"action": "request", "card": TOKEN, "label": "Player 4's room"}),
            json!({"action": "request", "code": "4F2A-91CX"}),
            json!({"action": "accept", "request": REQUEST}),
            json!({"action": "decline", "request": REQUEST}),
            json!({"action": "decline", "request": REQUEST, "block": true}),
            json!({"action": "cancel", "request": REQUEST}),
            json!({"action": "remove", "friend": FRIEND}),
            json!({"action": "remove", "friend": FRIEND, "block": true}),
            json!({"action": "block", "card": TOKEN, "label": "Player 4"}),
            json!({"action": "unblock", "block": BLOCK}),
            json!({"action": "invite", "friend": FRIEND, "room": {"room_id": ROOM}}),
            json!({"action": "invite", "friend": FRIEND, "room": {"secret": secret}}),
            json!({"action": "dismiss", "invite": INVITE}),
            json!({"action": "appear_offline", "on": true}),
            json!({"action": "appear_offline", "on": false}),
            json!({"action": "new_code"}),
            json!({"action": "forget"}),
        ];
        for wire in &cases {
            round_trips::<FriendAction>(wire, FriendAction::check);
            refuses_extra::<FriendAction>(wire, "");
        }
        refuses_extra::<FriendAction>(&cases[10], "/room");
        refuses_extra::<FriendAction>(&cases[11], "/room");
        assert_eq!(
            read::<FriendAction>(&cases[4]),
            FriendAction::Decline {
                request: REQUEST.into(),
                block: true
            }
        );
        assert_eq!(
            read::<FriendAction>(&cases[3]),
            FriendAction::Decline {
                request: REQUEST.into(),
                block: false
            }
        );

        for bad in [
            json!({"action": "befriend"}),
            json!({"request": REQUEST}),
            json!({"action": "accept"}),
            json!({"action": "unblock", "block": true}),
            json!({"action": "appear_offline"}),
            json!({"action": "decline", "request": REQUEST, "block": "yes"}),
            json!({"action": "invite", "friend": FRIEND}),
            json!({"action": "invite", "friend": FRIEND, "room": {}}),
            json!({"action": "invite", "friend": FRIEND,
                "room": {"room_id": ROOM, "secret": secret}}),
            json!({"action": "invite", "friend": FRIEND, "room": {"room": ROOM}}),
        ] {
            assert!(
                serde_json::from_value::<FriendAction>(bad.clone()).is_err(),
                "{bad}"
            );
        }
    }

    #[test]
    fn an_action_is_checked_for_what_it_names() {
        let check = |wire: Value| read::<FriendAction>(&wire).check();
        let field = |name| Err(Error::InvalidField(name));

        // A request names a card token and the label the sender saw, or a code.
        assert_eq!(
            check(json!({"action": "request", "card": TOKEN})),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "request", "label": "Room"})),
            field("request")
        );
        assert_eq!(check(json!({"action": "request"})), field("request"));
        assert_eq!(
            check(
                json!({"action": "request", "card": TOKEN, "label": "Room", "code": "4F2A-91CX"})
            ),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "request", "code": "4F2A-91CX", "label": "Room"})),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "request", "card": "c2.x", "label": "Room"})),
            field("card")
        );
        assert_eq!(
            check(json!({"action": "request", "card": TOKEN, "label": ""})),
            field("label")
        );
        assert_eq!(
            check(json!({"action": "request", "card": TOKEN, "label": "a".repeat(65)})),
            field("label")
        );
        assert!(
            check(json!({"action": "request", "card": TOKEN, "label": "a".repeat(64)})).is_ok()
        );
        // A code goes as typed.
        for typed in ["4f2a 91cx", "4F2A91CX", "o1o1-i1l1", " 4F2A-91CX "] {
            assert!(
                check(json!({"action": "request", "code": typed})).is_ok(),
                "{typed}"
            );
        }
        let long_code = "1".repeat(MAX_TYPED_CODE + 1);
        for typed in [
            "",
            "4F2A-91C",
            "4F2A-91CXX",
            "4F2A-91CU",
            "4F2A-91C!",
            long_code.as_str(),
        ] {
            assert_eq!(
                check(json!({"action": "request", "code": typed})),
                field("code"),
                "{typed}"
            );
        }

        assert_eq!(
            check(json!({"action": "block", "card": "nope", "label": "Room"})),
            field("card")
        );
        assert_eq!(
            check(json!({"action": "block", "card": TOKEN, "label": "\t"})),
            field("label")
        );
        // Each id has its own prefix.
        assert_eq!(
            check(json!({"action": "accept", "request": FRIEND})),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "decline", "request": "rq_x"})),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "cancel", "request": INVITE})),
            field("request")
        );
        assert_eq!(
            check(json!({"action": "remove", "friend": REQUEST})),
            field("friend")
        );
        assert_eq!(
            check(json!({"action": "unblock", "block": FRIEND})),
            field("block")
        );
        assert_eq!(
            check(json!({"action": "dismiss", "invite": BLOCK})),
            field("invite")
        );
        assert_eq!(
            check(json!({"action": "invite", "friend": REQUEST, "room": {"room_id": ROOM}})),
            field("friend")
        );
        assert_eq!(
            check(json!({"action": "invite", "friend": FRIEND, "room": {"room_id": "0F"}})),
            field("room_id")
        );
        let long = format!("emd2:{}", "A".repeat(123));
        for secret in ["emd1:", "emd3:AAAA", long.as_str()] {
            assert_eq!(
                check(json!({"action": "invite", "friend": FRIEND, "room": {"secret": secret}})),
                field("secret"),
                "{secret}"
            );
        }
        assert!(
            check(json!({"action": "invite", "friend": FRIEND,
            "room": {"secret": format!("emd2:{}", "a-_9".repeat(30) + "ab")}}))
            .is_ok()
        );
    }

    #[test]
    fn friend_codes_are_eight_crockford_symbols() {
        assert!(is_friend_code("4F2A-91CX"));
        assert!(is_friend_code("0000-ZZZZ"));
        for bad in [
            "",
            "4F2A91CX",
            "4F2A-91C",
            "4F2A-91CXX",
            "4f2a-91cx",
            "4F2A-91CI",
            "4F2A-91CL",
            "4F2A-91CO",
            "4F2A-91CU",
            "4F2A 91CX",
            "4F2AA91CX",
            "4F2A-91C\u{e9}",
        ] {
            assert!(!is_friend_code(bad), "{bad:?}");
        }
        assert_eq!(parse_friend_code("4f2a 91cx").as_deref(), Some("4F2A-91CX"));
        assert_eq!(parse_friend_code("4F2A-91CX").as_deref(), Some("4F2A-91CX"));
        // O reads as 0, and I and L as 1, in any case.
        assert_eq!(parse_friend_code("oOiI-lL00").as_deref(), Some("0011-1100"));
        assert_eq!(
            parse_friend_code("-4-F-2-A-9-1-C-X-").as_deref(),
            Some("4F2A-91CX")
        );
        for bad in [
            "",
            "4F2A-91C",
            "4F2A-91CXX",
            "4F2A-91CU",
            "4F2A-91C#",
            "4F2A-91C\u{e9}",
        ] {
            assert_eq!(parse_friend_code(bad), None, "{bad:?}");
        }
        assert_eq!(parse_friend_code(&"4".repeat(MAX_TYPED_CODE + 1)), None);
        // Whatever it parses to is a code.
        assert!(is_friend_code(&parse_friend_code("0123 4567").unwrap()));
    }

    #[test]
    fn rooms_and_activities_use_the_documents_names() {
        for (status, name) in [
            (RoomStatus::Idle, "idle"),
            (RoomStatus::Queued, "queued"),
            (RoomStatus::Ready, "ready"),
            (RoomStatus::Fighting, "fighting"),
            (RoomStatus::Watching, "watching"),
            (RoomStatus::Training, "training"),
        ] {
            assert_eq!(status.as_str(), name);
            assert_eq!(RoomStatus::parse(name), Some(status));
            assert_eq!(serde_json::to_value(status).unwrap(), name);
        }
        assert_eq!(RoomStatus::ALL.len(), 6);
        for (activity, name) in [
            (Activity::Menus, "menus"),
            (Activity::OfflinePlay, "offline_play"),
        ] {
            assert_eq!(activity.as_str(), name);
            assert_eq!(serde_json::to_value(activity).unwrap(), name);
        }
        assert_eq!(Activity::ALL.len(), 2);
    }
}
