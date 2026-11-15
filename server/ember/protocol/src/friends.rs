//! Friends: the routes a player's game calls to sync its presence and to act
//! on its friend list, and the `FriendsState` both answer
//! (`docs/design/FRIENDS.md`). The list belongs to the bridge, by Discord
//! account; no answer here carries a Discord ID or name, an Ember ID or a
//! rating.
//!
//! Requests are strict and answers are tolerant. A request (`FriendsSync` and
//! its parts, `FriendAction`) refuses unknown members, so a client's mistake
//! is caught at the bridge. An answer (`FriendsState` and its parts) ignores
//! members it does not know, and reads a value it does not know of a closed
//! set (a status, an activity, a presence state) as a neutral one, or drops
//! the invite or the badge, so what a later bridge adds does not break a
//! client already out. Those types are shared with requests, which stay
//! strict, so the tolerance is on the answer's fields (`lenient`). A build
//! asks only a bridge that lists [`FRIENDS_FEATURE`], and an older bridge
//! answers 404.
use std::fmt;

use serde::{Deserialize, Deserializer, Serialize, de::DeserializeOwned};

use crate::{
    Error, Result,
    encoding::{is_false, is_game_id, is_prefixed_id, is_single_line},
    levels::{CardAccount, FIGHTERS, MAX_TIER, is_card_token},
    play::{check_build, is_hex},
    rooms::{MAX_CAPACITY, MAX_NAME, MIN_CAPACITY, check_room_name, is_country, is_player_name},
    skill::{Listing, SkillBadge},
    wire_enum::wire_enum,
};

/// The capability a bridge with the friends routes offers, and only beside
/// `levels`.
pub const FRIENDS_FEATURE: &str = "friends";
/// The capability of a bridge that reads the `game` of a sync, the `offer` of
/// a private room report and the `join` action, and answers `game`,
/// `private_room` and `joining`. Offered wherever [`FRIENDS_FEATURE`] is. A
/// bridge from before it refuses those request members, so a client names
/// none of them to a bridge that does not list this.
pub const PRESENCE_V2_FEATURE: &str = "friends_presence_v2";
/// The game a sync that names none is for: builds from before the `game`
/// member are that game's. The one place the default is written.
pub const DEFAULT_GAME: &str = "usf4";
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

wire_enum! {
    serde
    /// Why a friends route refused (`details.reason` of the error): the one
    /// list the bridge writes, the helper passes on and the game words. A
    /// refusal of a client's own mistake has no reason here.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
    pub enum FriendRefusal {
        /// The caller has no Discord account (`skill::NO_ACCOUNT`).
        NoDiscordAccount = "no_discord_account",
        /// Friends are turned off for the caller. Any friends route answers
        /// it until `FriendAction::TurnOn`; the game shows it as a state, not
        /// as a failure.
        FriendsOff = "friends_off",
        UnknownCode = "unknown_code",
        CardExpired = "card_expired",
        OwnCard = "own_card",
        FriendsFull = "friends_full",
        BlocksFull = "blocks_full",
        RequestLimit = "request_limit",
        CodeTooSoon = "code_too_soon",
        /// Not a friend, or the friend is offline or appears offline.
        InviteUnavailable = "invite_unavailable",
        /// The caller cannot join that friend's private room now, for any
        /// reason: not a friend, offline or appearing offline, another game
        /// or build, no room on offer, or the room is full. One refusal for
        /// all of them, so the caller learns nothing about which.
        JoinUnavailable = "join_unavailable",
    }
}

/// Crockford base32: the digits and the letters but I, L, O and U. The
/// alphabet of a friend code, for the bridge to write one from.
pub const CODE_SYMBOLS: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

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

fn check_game(game: Option<&str>) -> Result<()> {
    if game.is_none_or(is_game_id) {
        Ok(())
    } else {
        Err(Error::InvalidField("game"))
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

/// Reads a `T`, or `None` for a value `T` does not know. What a later bridge
/// adds to a closed set must not fail the whole answer a client reads, so the
/// fields of an answer that hold one read through this and choose what an
/// unknown value is (`Relation` does the same for its type). The answer is
/// JSON, as every wire type here is.
fn lenient<'de, D, T>(deserializer: D) -> std::result::Result<Option<T>, D::Error>
where
    D: Deserializer<'de>,
    T: DeserializeOwned,
{
    Ok(T::deserialize(serde_json::Value::deserialize(deserializer)?).ok())
}

/// A room status this build does not know reads as `Idle`: in the room, and
/// nothing a client would show differently from sitting there (the others
/// say queued, ready, fighting, watching or training).
fn status_or_idle<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<RoomStatus, D::Error> {
    Ok(lenient(deserializer)?.unwrap_or(RoomStatus::Idle))
}

/// An activity this build does not know reads as `Menus`, the plain "online".
fn activity_or_menus<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<Activity, D::Error> {
    Ok(lenient(deserializer)?.unwrap_or(Activity::Menus))
}

/// A game id of an answer, or none when it is not one (`is_game_id`): a
/// value this build cannot use drops only itself.
fn game_or_none<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<Option<String>, D::Error> {
    Ok(lenient::<_, String>(deserializer)?.filter(|game| is_game_id(game)))
}

/// A private room of an answer, or none when it cannot be read or is not
/// well formed: it drops only itself, and the friend stays in the room.
fn private_room_or_none<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<Option<PrivateRoom>, D::Error> {
    Ok(lenient::<_, PrivateRoom>(deserializer)?.filter(|room| room.check().is_ok()))
}

/// The invitation a join answered, or none when it cannot be read.
fn joining_or_none<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<Option<Joining>, D::Error> {
    Ok(lenient::<_, Joining>(deserializer)?.filter(|joining| joining.check().is_ok()))
}

/// A presence this build cannot read (a state it does not know) reads as
/// online and doing the plain thing, with no room: the friend is shown as
/// there, which is true of anyone with a presence at all.
fn presence_or_online<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<FriendPresence, D::Error> {
    let mut presence = lenient(deserializer)?.unwrap_or(FriendPresence::Online {
        activity: Activity::Menus,
        game: None,
    });
    // A private room's facts beside a room that is not private say nothing
    // this build can show: they drop, and the friend stays where they are.
    if let FriendPresence::Room {
        private: false,
        private_room,
        ..
    } = &mut presence
    {
        *private_room = None;
    }
    Ok(presence)
}

/// A list of which the entries this build cannot read are left out, so one
/// invite of a kind it does not know does not take the others with it.
fn readable_only<'de, D, T>(deserializer: D) -> std::result::Result<Vec<T>, D::Error>
where
    D: Deserializer<'de>,
    T: DeserializeOwned,
{
    let entries = Vec::<serde_json::Value>::deserialize(deserializer)?;
    Ok(entries
        .into_iter()
        .filter_map(|entry| T::deserialize(entry).ok())
        .collect())
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
    /// The private room's invitation and display facts, for friends to join
    /// by; only with `private`, and only to a bridge that lists
    /// [`PRESENCE_V2_FEATURE`]. Absent keeps the room to invites.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub offer: Option<PrivateOffer>,
}

impl RoomReport {
    pub fn check(&self) -> Result<()> {
        if self.room_id.is_some() == self.private {
            return Err(Error::InvalidField("room"));
        }
        if self.room_id.as_deref().is_some_and(|id| !is_hex(id, 32)) {
            return Err(Error::InvalidField("room_id"));
        }
        if let Some(offer) = &self.offer {
            if !self.private {
                return Err(Error::InvalidField("offer"));
            }
            offer.check()?;
        }
        check_table(self.table)
    }
}

/// What a private room's member tells the bridge so friends of anyone in the
/// room can join it: the room's own invitation, which the bridge hands to a
/// friend only when they press join, and the facts a friend's list shows.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PrivateOffer {
    /// The room's `emd1:` or `emd2:` invitation (`is_invitation_secret`).
    pub secret: String,
    pub name: String,
    pub members: u8,
    pub capacity: u8,
}

impl PrivateOffer {
    pub fn check(&self) -> Result<()> {
        if !is_invitation_secret(&self.secret) {
            return Err(Error::InvalidField("secret"));
        }
        check_room_name(&self.name)?;
        check_facts(self.members, self.capacity)
    }
}

/// The invitation is a key to the room, so it stays out of any log line that
/// prints a report.
impl fmt::Debug for PrivateOffer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PrivateOffer")
            .field("secret", &"<redacted>")
            .field("name", &self.name)
            .field("members", &self.members)
            .field("capacity", &self.capacity)
            .finish()
    }
}

/// A room's capacity is within the limits and it holds at least one player
/// and no more than its capacity.
fn check_facts(members: u8, capacity: u8) -> Result<()> {
    if !(MIN_CAPACITY..=MAX_CAPACITY).contains(&capacity) {
        return Err(Error::InvalidField("capacity"));
    }
    if !(1..=capacity).contains(&members) {
        return Err(Error::InvalidField("members"));
    }
    Ok(())
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
    /// The game the client is for (`is_game_id`); absent reads as
    /// [`DEFAULT_GAME`]. Only to a bridge that lists [`PRESENCE_V2_FEATURE`].
    /// Decides whose rooms the caller may see and join.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game: Option<String>,
    /// What friends see as the name and flag.
    pub listing: Listing,
    /// The player's main fighter, 0 to `FIGHTERS` - 1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<u8>,
    pub presence: PresenceReport,
}

impl FriendsSync {
    /// The game this sync is for.
    pub fn game_id(&self) -> &str {
        self.game.as_deref().unwrap_or(DEFAULT_GAME)
    }

    pub fn check(&self) -> Result<()> {
        check_build(&self.build_id)?;
        if self.game.as_deref().is_some_and(|game| !is_game_id(game)) {
            return Err(Error::InvalidField("game"));
        }
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
    /// Asks for the invitation to a friend's private room (`fr_`). Answered
    /// with `FriendsState::joining`, or `FriendRefusal::JoinUnavailable`.
    Join { friend: String },
    /// Drops an invite the caller received (`iv_`).
    Dismiss { invite: String },
    /// Friends see the caller as offline.
    AppearOffline { on: bool },
    /// A new friend code, at most once a day; the old one stops working.
    NewCode {},
    /// Turns friends off: the caller's friendships and requests both ways
    /// and presence are deleted, their code is replaced (the old one stops
    /// working) and the account is stored as off. Their own blocks stay.
    /// Every friends route then refuses with `FriendRefusal::FriendsOff`,
    /// this one included, until `TurnOn`; other players meet the account as
    /// one without friends.
    Forget {},
    /// Turns friends on again after `Forget`: the account is as it was left,
    /// with its new code, no friends and its blocks. The only action an off
    /// account is allowed.
    TurnOn {},
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
            Self::Join { friend } => check_id(friend, "fr", "friend"),
            Self::Dismiss { invite } => check_id(invite, "iv", "invite"),
            Self::AppearOffline { .. } | Self::NewCode {} | Self::Forget {} | Self::TurnOn {} => {
                Ok(())
            }
        }
    }
}

/// A friend's main fighter.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MainFighter {
    /// 0 to `FIGHTERS` - 1.
    pub fighter: u8,
    /// At least 1; absent until the friend has played this fighter. Present
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

/// A private room a friend is in and offers to join: the facts a list shows
/// and whether the caller can join it now. Never the invitation, which a
/// `Join` action hands out.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrivateRoom {
    pub name: String,
    pub members: u8,
    pub capacity: u8,
    /// Same game and build as the caller, and not full.
    pub joinable: bool,
}

impl PrivateRoom {
    pub fn check(&self) -> Result<()> {
        check_room_name(&self.name)?;
        check_facts(self.members, self.capacity)
    }
}

/// The invitation to a friend's private room, answered to `Join` and to
/// nothing else.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Joining {
    /// The friend (`fr_`) the caller asked about.
    pub friend: String,
    /// The room's `emd1:` or `emd2:` invitation (`is_invitation_secret`).
    pub secret: String,
}

impl Joining {
    pub fn check(&self) -> Result<()> {
        check_id(&self.friend, "fr", "friend")?;
        if is_invitation_secret(&self.secret) {
            Ok(())
        } else {
            Err(Error::InvalidField("secret"))
        }
    }
}

/// The invitation is a key to the room, so it stays out of any log line that
/// prints a state.
impl fmt::Debug for Joining {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Joining")
            .field("friend", &self.friend)
            .field("secret", &"<redacted>")
            .finish()
    }
}

/// Where a friend is, as the caller may see it. A friend who appears offline
/// is `Offline`. `game` is the game they are playing, written whenever the
/// bridge knows it; a friend in another game than the caller's shows no room.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum FriendPresence {
    Offline {
        /// When they were last seen (unix seconds), when known.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        seen_at: Option<u64>,
    },
    Online {
        #[serde(deserialize_with = "activity_or_menus")]
        activity: Activity,
        #[serde(
            default,
            deserialize_with = "game_or_none",
            skip_serializing_if = "Option::is_none"
        )]
        game: Option<String>,
    },
    /// In a room: a public one by `room`, a private one by `private`, or, for
    /// a public room that is no longer open and listed, neither.
    Room {
        #[serde(deserialize_with = "status_or_idle")]
        status: RoomStatus,
        /// 0 to `MAX_TABLE`, when seated or watching.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        table: Option<u8>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        room: Option<FriendRoom>,
        /// Written only when true.
        #[serde(default, skip_serializing_if = "is_false")]
        private: bool,
        #[serde(
            default,
            deserialize_with = "game_or_none",
            skip_serializing_if = "Option::is_none"
        )]
        game: Option<String>,
        /// A private room the caller may see the facts of: only with
        /// `private`, and only for a friend in the caller's game.
        #[serde(
            default,
            deserialize_with = "private_room_or_none",
            skip_serializing_if = "Option::is_none"
        )]
        private_room: Option<PrivateRoom>,
    },
}

impl FriendPresence {
    pub fn check(&self) -> Result<()> {
        match self {
            Self::Offline { .. } => Ok(()),
            Self::Online { game, .. } => check_game(game.as_deref()),
            Self::Room {
                table,
                room,
                private,
                game,
                private_room,
                ..
            } => {
                if room.is_some() && *private {
                    return Err(Error::InvalidField("room"));
                }
                if private_room.is_some() && !*private {
                    return Err(Error::InvalidField("private_room"));
                }
                check_game(game.as_deref())?;
                check_table(*table)?;
                private_room.as_ref().map_or(Ok(()), PrivateRoom::check)?;
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
    /// Their account's points and frame, as their card shows them to anyone;
    /// absent when the answer is from a bridge that does not send it, or
    /// they have no card (no Discord account connected).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub account: Option<CardAccount>,
    /// Their class as last shown, or the developer mark; absent when they do
    /// not take part in ranking, or classes are hidden; also absent when it
    /// is one this build cannot read.
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "Option::is_none"
    )]
    pub badge: Option<SkillBadge>,
    #[serde(deserialize_with = "presence_or_online")]
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
        self.account.as_ref().map_or(Ok(()), CardAccount::check)?;
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
    #[serde(deserialize_with = "readable_only")]
    pub invites: Vec<ReceivedInvite>,
    /// At most `MAX_BLOCKS`.
    pub blocked: Vec<BlockedPlayer>,
    /// The invitation to the friend's private room the caller asked to join.
    /// Written only in the answer to `FriendAction::Join`, never in a sync's.
    #[serde(
        default,
        deserialize_with = "joining_or_none",
        skip_serializing_if = "Option::is_none"
    )]
    pub joining: Option<Joining>,
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
        self.blocked.iter().try_for_each(BlockedPlayer::check)?;
        self.joining.as_ref().map_or(Ok(()), Joining::check)
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
                "account": {"points": 2331, "tier": 3},
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
                game: None,
                private_room: None,
            }
        );
        assert_eq!(state.joining, None);
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
            joining: None,
        };
        assert!(empty.check().is_ok());
        assert_eq!(
            serde_json::to_string(&empty).unwrap(),
            r#"{"code":"4F2A-91CX","appear_offline":true,"friends":[],"incoming":[],"outgoing":[],"invites":[],"blocked":[]}"#
        );
    }

    /// A bridge that sends no `account` is read as no account known, and a state
    /// without one is written back without the member.
    #[test]
    fn a_friend_without_an_account_reads_as_unknown() {
        let mut wire = state_wire();
        let with = round_trips::<FriendsState>(&wire, FriendsState::check);
        assert_eq!(
            with.friends[0].account,
            Some(CardAccount {
                points: 2331,
                tier: 3
            })
        );
        wire["friends"][0]
            .as_object_mut()
            .unwrap()
            .remove("account");
        let without = round_trips::<FriendsState>(&wire, FriendsState::check);
        assert_eq!(without.friends[0].account, None);
    }

    #[test]
    fn every_state_member_ignores_what_it_does_not_know() {
        let wire = state_wire();
        for pointer in [
            "",
            "/friends/0",
            "/friends/0/main",
            "/friends/0/account",
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
        // Types other answers and the requests share, and the closed sets of
        // the answer, are read past in `ignores_unknown_value`.
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
    /// Replaces the value at `pointer` of the sample state with `unknown`
    /// and hands the state it reads to `check`.
    fn reads_unknown_value_as(pointer: &str, unknown: Value, check: impl Fn(&FriendsState)) {
        let mut wire = state_wire();
        *wire.pointer_mut(pointer).unwrap() = unknown;
        check(&read::<FriendsState>(&wire));
    }

    #[test]
    fn ignores_unknown_value() {
        // A room status a later bridge adds is "in the room" and no more.
        reads_unknown_value_as(
            "/friends/0/presence/status",
            json!("arcade_queue"),
            |state| {
                assert!(matches!(
                    state.friends[0].presence,
                    FriendPresence::Room {
                        status: RoomStatus::Idle,
                        table: Some(0),
                        room: Some(_),
                        ..
                    }
                ));
                assert!(state.check().is_ok());
            },
        );
        // An activity reads as the plain "online".
        let mut wire = state_wire();
        wire["friends"][0]["presence"] = json!({"state": "online", "activity": "streaming"});
        assert_eq!(
            read::<FriendsState>(&wire).friends[0].presence,
            FriendPresence::Online {
                activity: Activity::Menus,
                game: None
            }
        );
        // A presence state it does not know reads as online, whatever else it
        // carries, and the rest of the friend is kept.
        wire["friends"][0]["presence"] = json!({"state": "spectating", "room": {"id": 1}});
        let state = read::<FriendsState>(&wire);
        assert_eq!(
            state.friends[0].presence,
            FriendPresence::Online {
                activity: Activity::Menus,
                game: None
            }
        );
        assert_eq!(state.friends[0].name, "Player 3");
        // An invite of a kind it does not know is dropped; the others stay.
        let mut wire = state_wire();
        let known = wire["invites"][0].clone();
        wire["invites"] = json!([
            {"id": INVITE, "friend": FRIEND, "room": {"lobby": "x"}, "expires_at": 1731100600},
            known,
            {"id": INVITE, "friend": FRIEND, "room": {"room_id": ROOM}},
        ]);
        let state = read::<FriendsState>(&wire);
        assert_eq!(state.invites.len(), 1);
        assert_eq!(state.invites[0].room, InviteRoom::RoomId(ROOM.into()));
        // A badge it cannot read shows none, as one a bridge never sent does.
        for badge in [
            json!({"band": 7, "settled": true, "extra": 1}),
            json!(5),
            json!(null),
        ] {
            let mut wire = state_wire();
            wire["friends"][0]["badge"] = badge;
            assert_eq!(read::<FriendsState>(&wire).friends[0].badge, None);
        }
        // What it does read is written back as it was.
        let wire = state_wire();
        assert_eq!(
            serde_json::to_value(read::<FriendsState>(&wire)).unwrap(),
            wire
        );
        // The requests share these types and stay strict.
        assert!(
            serde_json::from_value::<RoomReport>(
                json!({"status": "arcade_queue", "private": true})
            )
            .is_err()
        );
        assert!(
            serde_json::from_value::<SkillBadge>(json!({"band": 7, "settled": true, "extra": 1}))
                .is_err()
        );
        assert!(serde_json::from_value::<InviteRoom>(json!({"lobby": "x"})).is_err());
    }

    /// The reasons are written as the game and the helper spell them; a
    /// rename changes a wire name and must fail here.
    #[test]
    fn refusal_reasons_keep_their_spelling() {
        let names: Vec<&str> = FriendRefusal::ALL.iter().map(|r| r.as_str()).collect();
        assert_eq!(
            names,
            [
                "no_discord_account",
                "friends_off",
                "unknown_code",
                "card_expired",
                "own_card",
                "friends_full",
                "blocks_full",
                "request_limit",
                "code_too_soon",
                "invite_unavailable",
                "join_unavailable",
            ]
        );
        assert_eq!(FriendRefusal::NAMES.as_slice(), names.as_slice());
        assert_eq!(
            FriendRefusal::NoDiscordAccount.as_str(),
            crate::skill::NO_ACCOUNT
        );
        for reason in FriendRefusal::ALL {
            assert_eq!(serde_json::to_value(reason).unwrap(), reason.as_str());
            assert_eq!(FriendRefusal::parse(reason.as_str()), Some(reason));
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
            json!({"state": "room"}),
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
            Ok(FriendPresence::Offline { .. })
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
            game: None,
            private_room: None,
        };
        assert_eq!(private_with_room.check(), Err(Error::InvalidField("room")));
        let at_table = |table| FriendPresence::Room {
            status: RoomStatus::Fighting,
            table: Some(table),
            room: None,
            private: false,
            game: None,
            private_room: None,
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
            friend(&|f| f.account = Some(CardAccount { points: 5, tier: 5 })),
            Err(Error::InvalidField("tier"))
        );
        assert!(friend(&|f| f.account = Some(CardAccount { points: 0, tier: 0 })).is_ok());
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
                    game: None,
                    private_room: None,
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
            offer: None,
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
            json!({"action": "turn_on"}),
            json!({"action": "join", "friend": FRIEND}),
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
            json!({"action": "join"}),
            json!({"action": "join", "friend": FRIEND, "room": {"room_id": ROOM}}),
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
        assert_eq!(
            check(json!({"action": "join", "friend": REQUEST})),
            field("friend")
        );
        assert!(check(json!({"action": "join", "friend": FRIEND})).is_ok());
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

    fn offer() -> PrivateOffer {
        PrivateOffer {
            secret: secret(),
            name: "Player 1's room".into(),
            members: 2,
            capacity: 8,
        }
    }

    #[test]
    fn a_sync_names_its_game_and_offers_a_private_room() {
        let wire = json!({"build_id": "build", "game": "usf4", "listing": {"name": "Player 1"},
            "presence": {"activity": "room", "room": {"private": true, "status": "idle",
                "offer": {"secret": secret(), "name": "Player 1's room",
                          "members": 2, "capacity": 8}}}});
        let sync = round_trips::<FriendsSync>(&wire, FriendsSync::check);
        assert_eq!(sync.game_id(), "usf4");
        match &sync.presence {
            PresenceReport::Room { room } => assert_eq!(room.offer, Some(offer())),
            other => panic!("{other:?}"),
        }
        // A request stays strict at the offer too.
        refuses_extra::<FriendsSync>(&wire, "/presence/room/offer");

        // A sync that names no game is the default game's and writes none.
        let plain = json!({"build_id": "build", "listing": {"name": "Player 1"},
            "presence": {"activity": "menus"}});
        let sync = round_trips::<FriendsSync>(&plain, FriendsSync::check);
        assert_eq!(sync.game, None);
        assert_eq!(sync.game_id(), DEFAULT_GAME);

        let with = |game: &str| {
            let mut sync = sync.clone();
            sync.game = Some(game.into());
            sync.check()
        };
        assert!(with("sfv").is_ok());
        assert!(with("usf4").is_ok());
        assert!(with(&"a".repeat(16)).is_ok());
        let long = "a".repeat(17);
        for bad in ["", "SFV", "sf-v", "sf v", "s\u{e9}", long.as_str()] {
            assert_eq!(with(bad), Err(Error::InvalidField("game")), "{bad:?}");
        }
    }

    #[test]
    fn an_offer_goes_only_with_a_private_room_and_must_be_well_formed() {
        let with = |change: &dyn Fn(&mut RoomReport)| {
            let mut report = RoomReport {
                room_id: None,
                private: true,
                status: RoomStatus::Idle,
                table: None,
                offer: Some(offer()),
            };
            change(&mut report);
            report.check()
        };
        let field = |name| Err(Error::InvalidField(name));
        assert_eq!(with(&|_| {}), Ok(()));
        assert_eq!(with(&|r| r.offer = None), Ok(()));
        // A public room has no offer.
        assert_eq!(
            with(&|r| {
                r.room_id = Some(ROOM.into());
                r.private = false;
            }),
            field("offer")
        );
        let offered =
            |change: &dyn Fn(&mut PrivateOffer)| with(&|r| change(r.offer.as_mut().unwrap()));
        for bad in ["", "emd1:short", "emd3:AAAA", "c1.abcdEFGH-_09"] {
            assert_eq!(
                offered(&|o| o.secret = bad.into()),
                field("secret"),
                "{bad}"
            );
        }
        assert_eq!(
            offered(&|o| o.secret = format!("emd2:{}", "A".repeat(123))),
            field("secret")
        );
        assert_eq!(offered(&|o| o.name = "".into()), field("name"));
        assert_eq!(offered(&|o| o.name = "two\nlines".into()), field("name"));
        assert_eq!(offered(&|o| o.capacity = 1), field("capacity"));
        assert_eq!(offered(&|o| o.capacity = 17), field("capacity"));
        assert_eq!(offered(&|o| o.members = 0), field("members"));
        assert_eq!(offered(&|o| o.members = 9), field("members"));
        assert_eq!(offered(&|o| o.members = 8), Ok(()));
        assert_eq!(offered(&|o| o.members = 1), Ok(()));
    }

    #[test]
    fn a_private_room_of_an_answer_has_its_facts_and_no_invitation() {
        let wire = json!({"state": "room", "status": "idle", "private": true, "game": "usf4",
            "private_room": {"name": "Player 1's room", "members": 2, "capacity": 8,
                             "joinable": true}});
        let presence = round_trips::<FriendPresence>(&wire, FriendPresence::check);
        let expected = |private_room, game: Option<&str>| FriendPresence::Room {
            status: RoomStatus::Idle,
            table: None,
            room: None,
            private: true,
            game: game.map(str::to_owned),
            private_room,
        };
        let facts = PrivateRoom {
            name: "Player 1's room".into(),
            members: 2,
            capacity: 8,
            joinable: true,
        };
        assert_eq!(presence, expected(Some(facts.clone()), Some("usf4")));
        ignores_extra::<FriendPresence>(&wire, "/private_room");

        // Another game's friend: the game and no room.
        round_trips::<FriendPresence>(
            &json!({"state": "online", "activity": "menus", "game": "sfv"}),
            FriendPresence::check,
        );
        round_trips::<FriendPresence>(
            &json!({"state": "room", "status": "fighting", "private": true, "game": "sfv"}),
            FriendPresence::check,
        );

        // A value this build cannot use drops only itself.
        for bad in [
            json!({"name": "", "members": 2, "capacity": 8, "joinable": true}),
            json!({"name": "Room", "members": 9, "capacity": 8, "joinable": true}),
            json!({"name": "Room", "members": 0, "capacity": 8, "joinable": true}),
            json!({"name": "Room", "members": 2, "capacity": 99, "joinable": true}),
            json!({"name": "Room", "members": 2, "capacity": 8}),
            json!({"name": "Room"}),
            json!("room"),
            json!(5),
            json!(null),
        ] {
            let mut wire = wire.clone();
            wire["private_room"] = bad.clone();
            assert_eq!(
                read::<FriendPresence>(&wire),
                expected(None, Some("usf4")),
                "{bad}"
            );
        }
        for bad in [
            json!(7),
            json!("SFV"),
            json!(""),
            json!("a".repeat(17)),
            json!(null),
        ] {
            let mut wire = wire.clone();
            wire["game"] = bad.clone();
            assert_eq!(
                read::<FriendPresence>(&wire),
                expected(Some(facts.clone()), None),
                "{bad}"
            );
            let mut online = json!({"state": "online", "activity": "menus"});
            online["game"] = bad.clone();
            assert_eq!(
                read::<FriendPresence>(&online),
                FriendPresence::Online {
                    activity: Activity::Menus,
                    game: None
                },
                "{bad}"
            );
        }
        // Inside a state the friend stays whole.
        let mut wire = state_wire();
        wire["friends"][0]["presence"]["game"] = json!(7);
        wire["friends"][0]["presence"]["private_room"] = json!({"name": ""});
        let state = read::<FriendsState>(&wire);
        assert!(matches!(
            state.friends[0].presence,
            FriendPresence::Room {
                room: Some(_),
                game: None,
                private_room: None,
                ..
            }
        ));
        assert_eq!(state.friends[0].name, "Player 3");
        // Well-formed facts beside a room that is not private drop too, and
        // the answer still checks.
        let mut wire = state_wire();
        wire["friends"][0]["presence"]["private_room"] =
            json!({"name": "Lounge", "members": 2, "capacity": 8, "joinable": true});
        let state = read::<FriendsState>(&wire);
        assert!(matches!(
            state.friends[0].presence,
            FriendPresence::Room {
                room: Some(_),
                private: false,
                private_room: None,
                ..
            }
        ));
        assert_eq!(state.check(), Ok(()));

        // What the bridge writes is checked: a private room only with
        // `private`, and a game that is one.
        let public_with_facts = FriendPresence::Room {
            status: RoomStatus::Idle,
            table: None,
            room: None,
            private: false,
            game: None,
            private_room: Some(facts.clone()),
        };
        assert_eq!(
            public_with_facts.check(),
            Err(Error::InvalidField("private_room"))
        );
        assert_eq!(
            expected(None, Some("SFV")).check(),
            Err(Error::InvalidField("game"))
        );
        assert_eq!(
            FriendPresence::Online {
                activity: Activity::Menus,
                game: Some("".into())
            }
            .check(),
            Err(Error::InvalidField("game"))
        );
        let bad_facts = PrivateRoom {
            members: 9,
            ..facts
        };
        assert_eq!(
            expected(Some(bad_facts), None).check(),
            Err(Error::InvalidField("members"))
        );
    }

    #[test]
    fn a_join_is_answered_with_the_invitation() {
        let mut wire = state_wire();
        wire["joining"] = json!({"friend": FRIEND, "secret": secret()});
        let state = round_trips::<FriendsState>(&wire, FriendsState::check);
        assert_eq!(
            state.joining,
            Some(Joining {
                friend: FRIEND.into(),
                secret: secret()
            })
        );
        ignores_extra::<FriendsState>(&wire, "/joining");
        // An ordinary state has none, and writes none.
        let plain = round_trips::<FriendsState>(&state_wire(), FriendsState::check);
        assert_eq!(plain.joining, None);

        // One this build cannot use drops only itself.
        for bad in [
            json!({"friend": FRIEND, "secret": "emd1:short"}),
            json!({"friend": "x", "secret": secret()}),
            json!({"friend": FRIEND}),
            json!(3),
            json!(null),
        ] {
            let mut wire = state_wire();
            wire["joining"] = bad.clone();
            let state = read::<FriendsState>(&wire);
            assert_eq!(state.joining, None, "{bad}");
            assert_eq!(state.friends.len(), 1);
        }
        let mut bad = state.clone();
        bad.joining = Some(Joining {
            friend: REQUEST.into(),
            secret: secret(),
        });
        assert_eq!(bad.check(), Err(Error::InvalidField("friend")));
    }

    #[test]
    fn debug_output_never_carries_an_invitation() {
        let report = PresenceReport::Room {
            room: RoomReport {
                room_id: None,
                private: true,
                status: RoomStatus::Idle,
                table: None,
                offer: Some(offer()),
            },
        };
        let mut state = read::<FriendsState>(&state_wire());
        state.joining = Some(Joining {
            friend: FRIEND.into(),
            secret: secret(),
        });
        let shown = format!("{:?} {:?} {report:?} {state:?}", offer(), state.joining);
        assert!(!shown.contains(&secret()), "{shown}");
        assert!(!shown.contains("emd1:"), "{shown}");
        assert!(shown.contains("redacted"));
        assert!(shown.contains("Player 1's room"));
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
