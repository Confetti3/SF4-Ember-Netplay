//! The bridge routes the connector calls with its connection's provider
//! credential: finding players by Challonge username, creating a set's
//! match, and cancelling one.
use std::fmt;

use ember_protocol::{
    api::{ApiError, ErrorCode},
    json,
    matches::{CreateMatch, MatchState},
    partner::{FoundChallongePlayer, LOOKUP_PATH, Lookup, LookupAnswer, MAX_LOOKUP},
};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use zeroize::Zeroizing;

use crate::http::{bounded_body, http_client};

const MAX_ANSWER: usize = 256 * 1024;

#[derive(Debug)]
pub(crate) enum BridgeError {
    /// The bridge answered with its error shape.
    Refused { code: ErrorCode, message: String },
    /// It could not be reached, or its answer was not one the bridge sends.
    Unavailable,
}

impl fmt::Display for BridgeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Refused { code, message } => {
                let code = serde_json::to_value(code).unwrap_or_default();
                write!(f, "{} ({message})", code.as_str().unwrap_or("refused"))
            }
            Self::Unavailable => f.write_str("the bridge could not be reached"),
        }
    }
}

/// The part of a match the connector reads.
#[derive(Deserialize)]
struct MatchView {
    match_id: String,
    state: MatchState,
    revision: String,
}

#[derive(Serialize)]
struct Cancel<'a> {
    reason: &'a str,
    expected_revision: &'a str,
}

pub(crate) struct BridgeClient {
    http: reqwest::Client,
    api: String,
    credential: Zeroizing<String>,
}

impl BridgeClient {
    pub fn new(api: &str, credential: Zeroizing<String>) -> Result<Self, String> {
        Ok(Self {
            http: http_client()?,
            api: api.trim_end_matches('/').to_owned(),
            credential,
        })
    }

    fn post(&self, path: &str) -> reqwest::RequestBuilder {
        self.http
            .post(format!("{}{path}", self.api))
            .bearer_auth(self.credential.as_str())
    }

    /// The players connected to these Challonge usernames, asked for in
    /// lookups of at most `MAX_LOOKUP`.
    pub async fn lookup(
        &self,
        usernames: &[String],
    ) -> Result<Vec<FoundChallongePlayer>, BridgeError> {
        let mut found = Vec::new();
        for chunk in usernames.chunks(MAX_LOOKUP) {
            let body = Lookup {
                challonge: chunk.to_vec(),
                ..Lookup::default()
            };
            let answer: LookupAnswer = self.answer(self.post(LOOKUP_PATH).json(&body)).await?;
            found.extend(answer.challonge);
        }
        Ok(found)
    }

    /// Creates the match, or finds the one this `external_match_id` already
    /// made, and returns its ID.
    pub async fn create(&self, command: &CreateMatch) -> Result<String, BridgeError> {
        let request = self
            .post("/v1/matches")
            .header("idempotency-key", &command.external_match_id)
            .json(command);
        let created: MatchView = self.answer(request).await?;
        Ok(created.match_id)
    }

    /// Cancels the match unless it has already ended.
    pub async fn cancel(&self, match_id: &str, reason: &str) -> Result<(), BridgeError> {
        let request = self
            .http
            .get(format!("{}/v1/matches/{match_id}", self.api))
            .bearer_auth(self.credential.as_str());
        let found: MatchView = self.answer(request).await?;
        if found.state.is_terminal() {
            return Ok(());
        }
        let request = self
            .post(&format!("/v1/matches/{match_id}/cancel"))
            .header(
                "idempotency-key",
                format!("cancel:{match_id}:{}", found.revision),
            )
            .json(&Cancel {
                reason,
                expected_revision: &found.revision,
            });
        let _: MatchView = self.answer(request).await?;
        Ok(())
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
        let refusal: ApiError =
            json::parse_as(&body, MAX_ANSWER).map_err(|_| BridgeError::Unavailable)?;
        Err(BridgeError::Refused {
            code: refusal.error.code,
            message: refusal.error.message.chars().take(200).collect(),
        })
    }
}
