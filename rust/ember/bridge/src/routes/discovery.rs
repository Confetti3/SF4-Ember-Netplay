//! Public, unauthenticated discovery (spec 9.1, 19.3).
use axum::{extract::State, response::Response};
use ember_protocol::{
    api::{API_VERSION, BridgeProfile, Capabilities, ConnectionInfo, Limits, WELL_KNOWN_PATH},
    challenge, json,
    lobby::Rotation,
    tournament::Format,
};
use serde_json::json;

use crate::{AppState, auth, http::ok, routes::links::CODE_LIFETIME_SECS};

pub async fn well_known(State(state): State<AppState>) -> Response {
    let origin = &state.config.origin;
    debug_assert_eq!(WELL_KNOWN_PATH, "/.well-known/ember-bridge.json");
    ok(&BridgeProfile {
        bridge_id: state.config.bridge_id.clone(),
        origin: origin.clone(),
        display_name: state.config.display_name.clone(),
        api_versions: vec![API_VERSION.into()],
        capabilities_url: format!("{origin}/v1/capabilities"),
        signing_keys_url: format!("{origin}/v1/signing-keys"),
    })
}

pub async fn capabilities(State(state): State<AppState>) -> Response {
    let mut features = vec![
        "identity.challenge".to_owned(),
        "link.code".to_owned(),
        "link.provider_proxy".to_owned(),
        "matches.adjudication".to_owned(),
        "matches.play".to_owned(),
        "lobbies".to_owned(),
        "tournaments".to_owned(),
        "records".to_owned(),
        "events.poll".to_owned(),
        "events.sse".to_owned(),
        "webhooks.standard".to_owned(),
    ];
    if state.config.mock_browser {
        features.push("link.mock_browser".into());
    }
    features.push(ember_protocol::discord::ACCOUNTS_FEATURE.into());
    if crate::routes::discord::sign_in(&state).is_some() {
        features.push(ember_protocol::discord::FEATURE.into());
    }
    if crate::routes::rooms::enabled(&state) {
        features.push(crate::routes::rooms::FEATURE.into());
    }
    ok(&Capabilities {
        api_version: API_VERSION.into(),
        event_version: "v1".into(),
        games: vec!["usf4".into()],
        games_to_win: vec![1, 2, 3, 5],
        // `ember-room-v1` plays under the room's own settings. No translator
        // for a native USF4 rules profile is tested yet (spec 13.3).
        native_rules_profiles: vec![ember_protocol::play::PROFILE.into()],
        native_play: true,
        result_sources: vec!["organizer_adjudication".into(), "player_agreement".into()],
        features,
        lobby_rotations: Rotation::ALL
            .iter()
            .map(|rotation| rotation.as_str().to_owned())
            .collect(),
        tournament_formats: Format::ALL
            .iter()
            .map(|format| format.as_str().to_owned())
            .collect(),
        connections: state
            .config
            .tenants
            .iter()
            .flat_map(|tenant| &tenant.connections)
            .filter(|connection| connection.enabled)
            .map(|connection| ConnectionInfo {
                id: connection.id.clone(),
                display_name: connection.display_name.clone(),
                environment: connection.environment.clone(),
            })
            .collect(),
        limits: Limits {
            max_body_bytes: json::MAX_BODY,
            max_proof_body_bytes: challenge::MAX_PROOF_BODY,
            max_json_depth: json::MAX_DEPTH,
            challenge_lifetime_secs: challenge::LIFETIME_SECS,
            session_lifetime_secs: auth::SESSION_SECS,
            link_code_lifetime_secs: CODE_LIFETIME_SECS,
        },
    })
}

/// Public halves of the bridge's own signing keys, as JWKs. Clients trust
/// these only when fetched from a bridge they already approved.
pub async fn signing_keys(State(state): State<AppState>) -> Response {
    ok(&json!({
        "keys": [{
            "kty": "OKP",
            "crv": "Ed25519",
            "alg": "EdDSA",
            "use": "sig",
            "kid": state.keys.signing_kid(),
            "x": state.keys.signing_key().to_b64u(),
        }]
    }))
}
