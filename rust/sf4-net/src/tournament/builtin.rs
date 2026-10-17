//! Bridges this build trusts without an approval: Ember's staging bridge in
//! Nightly builds (cargo feature `staging-bridge`), none otherwise. Each is
//! pinned by its bridge ID, origin and Ed25519 signing keys. The helper never
//! replaces those keys from the network, and before each new session checks
//! that the origin still describes the pinned bridge (`Client::check_pin`).
//! The store keeps built-ins out of `bridges.json`; a player can still remove
//! one, which `removed.json` records like any other removal.
use ember_protocol::api::SigningKey;

use super::bridges::Approved;

/// A pinned bridge.
#[derive(Clone, Debug)]
pub struct Builtin {
    pub bridge_id: String,
    pub origin: String,
    pub display_name: String,
    /// The keys that sign its bindings, permits and tickets, as the JWK
    /// `kid` and base64url `x` its `/v1/signing-keys` publishes.
    pub keys: Vec<(String, String)>,
}

impl Builtin {
    /// The trusted entry, before the store checks it.
    pub(super) fn entry(&self) -> Approved {
        Approved {
            bridge_id: self.bridge_id.clone(),
            origin: self.origin.clone(),
            display_name: self.display_name.clone(),
            approved_at: 0,
            keys: self
                .keys
                .iter()
                .map(|(kid, x)| SigningKey {
                    kty: "OKP".into(),
                    crv: "Ed25519".into(),
                    alg: "EdDSA".into(),
                    usage: "sig".into(),
                    kid: kid.clone(),
                    x: x.clone(),
                })
                .collect(),
        }
    }
}

/// The staging bridge's pin, read from its public documents:
/// `https://bridge-staging.embernetplay.link/.well-known/ember-bridge.json`
/// for the ID, `/v1/signing-keys` for the key. A new ID or key there needs a
/// new build; until then the bridge reads as not trusted.
#[cfg(feature = "staging-bridge")]
mod staging {
    pub const STAGING_BRIDGE_ID: &str = "brg_1195478b-ac7e-4bc9-90af-c74eac5c747b";
    pub const STAGING_KEY_KID: &str = "fGpBMbqH-Jnx5h0M";
    pub const STAGING_KEY_X: &str = "OXhVSHlgdGw7UY-q44K7j6dzeO7gM2_fe97bThpjST0";
    pub const STAGING_ORIGIN: &str = "https://bridge-staging.embernetplay.link";
}

/// The bridges this build trusts out of the box.
pub fn builtins() -> Vec<Builtin> {
    #[cfg(feature = "staging-bridge")]
    {
        vec![Builtin {
            bridge_id: staging::STAGING_BRIDGE_ID.into(),
            origin: staging::STAGING_ORIGIN.into(),
            display_name: "Ember bridge (staging)".into(),
            keys: vec![(
                staging::STAGING_KEY_KID.into(),
                staging::STAGING_KEY_X.into(),
            )],
        }]
    }
    #[cfg(not(feature = "staging-bridge"))]
    {
        Vec::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(not(feature = "staging-bridge"))]
    #[test]
    fn a_build_without_the_feature_trusts_nothing_by_itself() {
        assert!(builtins().is_empty());
    }

    // An invalid pin would be skipped by the store and trust nothing.
    #[cfg(feature = "staging-bridge")]
    #[test]
    fn staging_pin_is_filled() {
        use ember_protocol::encoding::{OriginPolicy, check_origin, is_prefixed_id};
        assert!(
            is_prefixed_id(staging::STAGING_BRIDGE_ID, "brg"),
            "fill in STAGING_BRIDGE_ID in builtin.rs"
        );
        assert!(check_origin(staging::STAGING_ORIGIN, OriginPolicy::HttpsOnly).is_ok());
        let entry = builtins()[0].entry();
        assert!(
            entry.key(staging::STAGING_KEY_KID).is_some(),
            "fill in STAGING_KEY_KID and STAGING_KEY_X in builtin.rs"
        );
        let dir = std::env::temp_dir().join(format!("sf4-builtin-staging-{}", std::process::id()));
        let store = super::super::bridges::Store::open(dir.clone(), &builtins());
        assert!(
            store.pins(staging::STAGING_BRIDGE_ID)
                && store.get(staging::STAGING_BRIDGE_ID).is_some(),
            "the staging pin is not trusted"
        );
        let _ = std::fs::remove_dir_all(dir);
    }
}
