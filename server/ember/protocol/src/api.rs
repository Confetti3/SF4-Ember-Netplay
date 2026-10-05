//! The public error contract (spec 19.5) and bridge discovery documents.
use serde::{Deserialize, Serialize};

pub const API_VERSION: &str = "v1";
pub const WELL_KNOWN_PATH: &str = "/.well-known/ember-bridge.json";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    InvalidRequest,
    UnsupportedVersion,
    Unauthenticated,
    Forbidden,
    NotFound,
    ChallengeExpired,
    ChallengeUsed,
    InvalidSignature,
    IdentityUnavailable,
    LinkExpired,
    LinkPendingApproval,
    LinkConflict,
    IdempotencyConflict,
    StaleRevision,
    StaleAssignment,
    LeaseConflict,
    UnsupportedRules,
    IncompatibleBuild,
    ReportConflict,
    NeedsReview,
    RateLimited,
    ResyncRequired,
    ServiceUnavailable,
}

impl ErrorCode {
    pub fn status(self) -> u16 {
        match self {
            Self::InvalidRequest | Self::UnsupportedVersion => 400,
            Self::Unauthenticated
            | Self::ChallengeExpired
            | Self::ChallengeUsed
            | Self::InvalidSignature => 401,
            Self::Forbidden | Self::IdentityUnavailable => 403,
            Self::NotFound => 404,
            Self::LinkExpired
            | Self::LinkPendingApproval
            | Self::LinkConflict
            | Self::IdempotencyConflict
            | Self::StaleRevision
            | Self::StaleAssignment
            | Self::LeaseConflict
            | Self::ReportConflict
            | Self::NeedsReview
            | Self::ResyncRequired => 409,
            Self::UnsupportedRules | Self::IncompatibleBuild => 422,
            Self::RateLimited => 429,
            Self::ServiceUnavailable => 503,
        }
    }

    pub fn retryable(self) -> bool {
        matches!(
            self,
            Self::RateLimited | Self::ServiceUnavailable | Self::ResyncRequired
        )
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ErrorBody {
    pub code: ErrorCode,
    pub message: String,
    pub retryable: bool,
    pub request_id: String,
    #[serde(default, skip_serializing_if = "serde_json::Map::is_empty")]
    pub details: serde_json::Map<String, serde_json::Value>,
}

/// `{"error": {...}}`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ApiError {
    pub error: ErrorBody,
}

/// `/.well-known/ember-bridge.json`. Informational only: fetching it never
/// makes a bridge trusted (spec 9.1).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BridgeProfile {
    pub bridge_id: String,
    pub origin: String,
    pub display_name: String,
    pub api_versions: Vec<String>,
    pub capabilities_url: String,
    pub signing_keys_url: String,
}

/// One public bridge key from `/v1/signing-keys`, as an Ed25519 JWK. A
/// client trusts it only when fetched from a bridge the player approved.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SigningKey {
    pub kty: String,
    pub crv: String,
    pub alg: String,
    #[serde(rename = "use")]
    pub usage: String,
    pub kid: String,
    pub x: String,
}

impl SigningKey {
    /// The key, when this is an Ed25519 signature key.
    pub fn public_key(&self) -> Option<crate::PublicKey> {
        let shape = self.kty == "OKP"
            && self.crv == "Ed25519"
            && self.alg == "EdDSA"
            && self.usage == "sig";
        let kid = !self.kid.is_empty() && self.kid.len() <= 64 && self.kid.is_ascii();
        (shape && kid)
            .then(|| crate::PublicKey::from_b64u(&self.x).ok())
            .flatten()
    }
}

/// `/v1/signing-keys`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SigningKeys {
    pub keys: Vec<SigningKey>,
}

/// `/v1/capabilities`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Capabilities {
    pub api_version: String,
    pub event_version: String,
    pub games: Vec<String>,
    pub games_to_win: Vec<u8>,
    /// Native rules profiles the room layer can actually enforce. Empty until
    /// a rules translator is tested (spec 13.3).
    pub native_rules_profiles: Vec<String>,
    pub native_play: bool,
    pub result_sources: Vec<String>,
    pub features: Vec<String>,
    /// Seat rotations a lobby can use. Absent on a bridge without lobbies.
    #[serde(default)]
    pub lobby_rotations: Vec<String>,
    /// Bracket formats a tournament can use. Absent on a bridge without tournaments.
    #[serde(default)]
    pub tournament_formats: Vec<String>,
    /// Provider connections a player can link to on this bridge.
    #[serde(default)]
    pub connections: Vec<ConnectionInfo>,
    pub limits: Limits,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConnectionInfo {
    pub id: String,
    pub display_name: String,
    /// `local`, `staging` or `production`.
    pub environment: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Limits {
    pub max_body_bytes: usize,
    pub max_proof_body_bytes: usize,
    pub max_json_depth: usize,
    pub challenge_lifetime_secs: u64,
    pub session_lifetime_secs: u64,
    pub link_code_lifetime_secs: u64,
}
