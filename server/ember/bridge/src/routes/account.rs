//! The calling player of the routes that serve a Discord account's data:
//! points and the card look-up, the profile's own card and search, and
//! friends. Each admits its caller the same way, in this order: the route's
//! gate (404 where it is closed, as a bridge from before it answers), the
//! player's session, the Discord account connected to their Ember ID (refused
//! where the gate needs one), then the route's rate limit.
//!
//! A limit is kept per Discord account, which follows the player to any Ember
//! ID it is connected to, as their rating and points do (`skill`). Only a
//! caller without a Discord account, who is answered no points and no cards,
//! is limited by Ember ID.
use axum::http::HeaderMap;
use ember_protocol::{EmberId, api::ErrorCode, friends::FriendRefusal, skill::NO_ACCOUNT};

use crate::{
    AppState,
    error::{ApiFailure, Result},
    friends::{self, store},
    routes::{play::playing, rooms},
    skill,
};

/// A rate limit: `uses` per `window` seconds, for one caller.
pub struct Bucket {
    pub name: &'static str,
    pub uses: usize,
    pub window: u64,
}

/// Which routes a caller is admitted to.
#[derive(Clone, Copy)]
pub enum Gate {
    /// Points and the card look-up: where public rooms are on. A caller
    /// without a Discord account is admitted, and answered as one.
    Points,
    /// The caller's own card and search: where public rooms are on, with a
    /// Discord account.
    Profiles,
    /// Friends: where friends are offered (`friends::offered`), with a
    /// Discord account.
    Friends,
}

impl Gate {
    fn open(self, state: &AppState) -> bool {
        match self {
            Self::Points | Self::Profiles => rooms::enabled(state),
            Self::Friends => friends::offered(state),
        }
    }

    fn needs_account(self) -> bool {
        !matches!(self, Self::Points)
    }

    /// The refusal of a caller without a Discord account, worded for the
    /// routes behind the gate.
    fn no_account(self) -> ApiFailure {
        match self {
            Self::Friends => store::refuse(FriendRefusal::NoDiscordAccount),
            Self::Points | Self::Profiles => ApiFailure::new(
                ErrorCode::Forbidden,
                "Connect a Discord account to have a card.",
            )
            .detail("reason", NO_ACCOUNT),
        }
    }
}

/// The calling player: their Ember ID, the Discord account connected to it
/// if there is one, and the time.
pub struct PlayerAccount {
    pub ember_id: EmberId,
    pub account: Option<String>,
    pub now: u64,
}

impl PlayerAccount {
    /// The caller behind `gate`, within `bucket`.
    pub async fn admit(
        state: &AppState,
        headers: &HeaderMap,
        gate: Gate,
        bucket: &Bucket,
    ) -> Result<Self> {
        if !gate.open(state) {
            return Err(ApiFailure::not_found());
        }
        let ember_id = playing(state, headers).await?.ember_id;
        let id = ember_id.clone();
        let account = state.db.read(move |tx| skill::account(tx, &id)).await?;
        if account.is_none() && gate.needs_account() {
            return Err(gate.no_account());
        }
        let caller = Self {
            ember_id,
            account,
            now: state.now(),
        };
        caller.spend(state, bucket)?;
        Ok(caller)
    }

    /// One more use of `bucket`: by Discord account, or by Ember ID for a
    /// caller without one.
    pub fn spend(&self, state: &AppState, bucket: &Bucket) -> Result<()> {
        let owner = match &self.account {
            Some(account) => account.clone(),
            None => format!("ember:{}", self.ember_id.as_str()),
        };
        spend(state, bucket, &owner, self.now)
    }
}

/// A caller admitted through a gate that needs a Discord account: the account
/// and the time.
pub struct Linked {
    pub account: String,
    pub now: u64,
}

impl Linked {
    /// The caller behind `gate`, which needs a Discord account, within
    /// `bucket`.
    pub async fn admit(
        state: &AppState,
        headers: &HeaderMap,
        gate: Gate,
        bucket: &Bucket,
    ) -> Result<Self> {
        let caller = PlayerAccount::admit(state, headers, gate, bucket).await?;
        let account = caller.account.ok_or_else(|| gate.no_account())?;
        Ok(Self {
            account,
            now: caller.now,
        })
    }

    /// One more use of `bucket`, by the Discord account: a route's second
    /// limit for some of its uses.
    pub fn spend(&self, state: &AppState, bucket: &Bucket) -> Result<()> {
        spend(state, bucket, &self.account, self.now)
    }
}

fn spend(state: &AppState, bucket: &Bucket, owner: &str, now: u64) -> Result<()> {
    state
        .rate
        .check(
            &format!("{}:{owner}", bucket.name),
            bucket.uses,
            bucket.window,
            now,
        )
        .map_err(ApiFailure::rate_limited)
}
