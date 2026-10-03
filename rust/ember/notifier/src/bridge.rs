//! A small typed client for the bridge routes the room bot calls, with the
//! connection's provider credential: the player lookup, opening a room and
//! reading one back.
use ember_protocol::{
    api::{ApiError, BridgeProfile, ErrorCode, WELL_KNOWN_PATH},
    json,
    partner::{FoundPlayer, LOOKUP_PATH, Lookup, LookupAnswer},
    rooms::{ConnectionCreateRoom, ConnectionRoom},
};
use serde::de::DeserializeOwned;
use tokio::sync::OnceCell;
use zeroize::Zeroizing;

use crate::sent::{bounded_body, http_client};

const MAX_ANSWER: usize = 256 * 1024;

/// The page that connects a Discord account to an Ember ID on a bridge.
const START_PAGE: &str = "https://embernetplay.link/start#";

/// Why a bridge call did not give the answer asked for.
#[derive(Debug)]
pub(crate) enum BridgeError {
    /// The bridge answered with its error shape.
    Refused(Refusal),
    /// It could not be reached, or its answer was not one the bridge sends.
    Unavailable,
}

#[derive(Debug)]
pub(crate) struct Refusal {
    pub code: ErrorCode,
    /// `details.reason` of a room refusal, such as `room_limit`.
    pub reason: Option<String>,
    /// `details.room_id`: the room a `room_limit` refusal is about.
    pub room_id: Option<String>,
}

pub(crate) struct BridgeClient {
    http: reqwest::Client,
    api: String,
    credential: Zeroizing<String>,
    bridge_id: OnceCell<String>,
}

impl BridgeClient {
    pub(crate) fn new(api: &str, credential: Zeroizing<String>) -> Result<Self, String> {
        Ok(Self {
            http: http_client()?,
            api: api.trim_end_matches('/').to_owned(),
            credential,
            bridge_id: OnceCell::new(),
        })
    }

    /// The player connected to a Discord account and linked on the
    /// connection, if there is one.
    pub(crate) async fn lookup(
        &self,
        discord_id: &str,
    ) -> Result<Option<FoundPlayer>, BridgeError> {
        let body = Lookup {
            discord: vec![discord_id.to_owned()],
        };
        let request = self
            .http
            .post(format!("{}{LOOKUP_PATH}", self.api))
            .bearer_auth(self.credential.as_str())
            .json(&body);
        let answer: LookupAnswer = self.answer(request).await?;
        Ok(answer
            .players
            .into_iter()
            .find(|player| player.discord_user_id == discord_id))
    }

    pub(crate) async fn create_room(
        &self,
        command: &ConnectionCreateRoom,
    ) -> Result<ConnectionRoom, BridgeError> {
        let request = self
            .http
            .post(format!("{}/v1/rooms", self.api))
            .bearer_auth(self.credential.as_str())
            .json(command);
        self.answer(request).await
    }

    pub(crate) async fn room(&self, room_id: &str) -> Result<ConnectionRoom, BridgeError> {
        let request = self
            .http
            .get(format!("{}/v1/rooms/{room_id}", self.api))
            .bearer_auth(self.credential.as_str());
        self.answer(request).await
    }

    /// The page where a player connects Discord to their Ember ID on this
    /// bridge. The bridge's ID comes from its discovery document, once.
    pub(crate) async fn start_url(&self) -> Result<String, BridgeError> {
        let id = self
            .bridge_id
            .get_or_try_init(|| async {
                let request = self.http.get(format!("{}{WELL_KNOWN_PATH}", self.api));
                let profile: BridgeProfile = self.answer(request).await?;
                Ok(profile.bridge_id)
            })
            .await?;
        Ok(format!("{START_PAGE}{id}"))
    }

    async fn answer<T: DeserializeOwned>(
        &self,
        request: reqwest::RequestBuilder,
    ) -> Result<T, BridgeError> {
        let response = request.send().await.map_err(|_| BridgeError::Unavailable)?;
        let success = response.status().is_success();
        let body = bounded_body(response, MAX_ANSWER)
            .await
            .ok_or(BridgeError::Unavailable)?;
        if success {
            return json::parse_as(&body, MAX_ANSWER).map_err(|_| BridgeError::Unavailable);
        }
        let error: ApiError =
            json::parse_as(&body, MAX_ANSWER).map_err(|_| BridgeError::Unavailable)?;
        let detail = |key: &str| {
            error
                .error
                .details
                .get(key)
                .and_then(|value| value.as_str())
                .map(str::to_owned)
        };
        Err(BridgeError::Refused(Refusal {
            code: error.error.code,
            reason: detail("reason"),
            room_id: detail("room_id"),
        }))
    }
}
