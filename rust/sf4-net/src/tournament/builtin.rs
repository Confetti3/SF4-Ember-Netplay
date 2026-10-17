//! Bridges this build trusts without an approval: Ember's staging bridge in
//! Nightly builds (cargo feature `staging-bridge`), none otherwise. Each is
//! pinned by its bridge ID, origin and Ed25519 signing keys. The helper never
//! replaces those keys from the network, and checks once per run that the
//! origin still describes the pinned bridge (`Client::check_pin`). The store
//! keeps built-ins out of `bridges.json`; a player can still remove one,
//! which `removed.json` records like any other removal.
use ember_protocol::api::SigningKey;

use super::bridges::Approved;

/// A pinned bridge. Borrowed so the compiled-in list is a constant and tests
/// can pin a loopback bridge whose origin is only known at run time.
#[derive(Clone, Copy, Debug)]
pub struct Builtin<'a> {
    pub bridge_id: &'a str,
    pub origin: &'a str,
    pub display_name: &'a str,
    /// The keys that sign its bindings, permits and tickets, as the JWK
    /// `kid` and base64url `x` its `/v1/signing-keys` publishes.
    pub keys: &'a [(&'a str, &'a str)],
}

impl Builtin<'_> {
    /// The trusted entry, before the store checks it.
    pub(super) fn entry(&self) -> Approved {
        Approved {
            bridge_id: self.bridge_id.into(),
            origin: self.origin.into(),
            display_name: self.display_name.into(),
            approved_at: 0,
            keys: self
                .keys
                .iter()
                .map(|&(kid, x)| SigningKey {
                    kty: "OKP".into(),
                    crv: "Ed25519".into(),
                    alg: "EdDSA".into(),
                    usage: "sig".into(),
                    kid: kid.into(),
                    x: x.into(),
                })
                .collect(),
            builtin: true,
        }
    }
}

/// The staging bridge's pin, read from its public documents:
/// `https://bridge-staging.embernetplay.link/.well-known/ember-bridge.json`
/// for the ID, `/v1/signing-keys` for the key. Until they are filled in the
/// entry is invalid, so the store skips it and nothing is trusted, and
/// `staging_pin_is_filled` fails.
#[cfg(feature = "staging-bridge")]
mod staging {
    pub const STAGING_BRIDGE_ID: &str = "brg_FILL_IN_STAGING_BRIDGE_ID";
    pub const STAGING_KEY_KID: &str = "FILL_IN_STAGING_KEY_KID";
    pub const STAGING_KEY_X: &str = "FILL_IN_STAGING_KEY_X";
    pub const STAGING_ORIGIN: &str = "https://bridge-staging.embernetplay.link";
}

#[cfg(feature = "staging-bridge")]
const STAGING: Builtin<'static> = Builtin {
    bridge_id: staging::STAGING_BRIDGE_ID,
    origin: staging::STAGING_ORIGIN,
    display_name: "Ember bridge (staging)",
    keys: &[(staging::STAGING_KEY_KID, staging::STAGING_KEY_X)],
};

/// The bridges this build trusts out of the box.
pub fn builtins() -> &'static [Builtin<'static>] {
    #[cfg(feature = "staging-bridge")]
    {
        &[STAGING]
    }
    #[cfg(not(feature = "staging-bridge"))]
    {
        &[]
    }
}

/// The entries for `list`, unchecked.
pub(super) fn entries(list: &[Builtin]) -> Vec<Approved> {
    list.iter().map(Builtin::entry).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(not(feature = "staging-bridge"))]
    #[test]
    fn a_build_without_the_feature_trusts_nothing_by_itself() {
        assert!(builtins().is_empty());
    }

    // Fails until the placeholders above hold the staging bridge's real pin.
    #[cfg(feature = "staging-bridge")]
    #[test]
    fn staging_pin_is_filled() {
        use ember_protocol::encoding::{OriginPolicy, check_origin, is_prefixed_id};
        assert!(
            is_prefixed_id(staging::STAGING_BRIDGE_ID, "brg"),
            "fill in STAGING_BRIDGE_ID in builtin.rs"
        );
        assert!(check_origin(staging::STAGING_ORIGIN, OriginPolicy::HttpsOnly).is_ok());
        let entry = STAGING.entry();
        assert!(
            entry.key(staging::STAGING_KEY_KID).is_some(),
            "fill in STAGING_KEY_KID and STAGING_KEY_X in builtin.rs"
        );
        let dir = std::env::temp_dir().join(format!("sf4-builtin-staging-{}", std::process::id()));
        let store = super::super::bridges::Store::open(dir.clone(), entries(builtins()));
        assert!(
            store
                .get(staging::STAGING_BRIDGE_ID)
                .is_some_and(|bridge| bridge.builtin),
            "the staging pin is not trusted"
        );
        let _ = std::fs::remove_dir_all(dir);
    }
}
