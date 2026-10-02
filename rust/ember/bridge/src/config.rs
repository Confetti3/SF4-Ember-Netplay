//! Bridge configuration. Tenants and provider connections are declared here;
//! credentials are issued with `ember-bridge credential` and kept in the
//! database as keyed hashes.
use std::{fs, path::Path, path::PathBuf};

use ember_protocol::encoding::{OriginPolicy, check_origin, is_prefixed_id};
use serde::{Deserialize, Serialize};

/// Provider kinds this build can serve. `direct` is a platform that calls
/// the generic API itself with its provider credential; `mock` also gets the
/// mock login pages when `mock_browser` is set. Adapters that call a
/// platform's own API are added only after its contract is verified
/// (spec 22).
pub const SUPPORTED_KINDS: &[&str] = &["direct", "mock"];

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub bridge_id: String,
    pub display_name: String,
    /// Public origin, the challenge audience and event `source`.
    pub origin: String,
    pub listen: String,
    pub database: PathBuf,
    pub secrets: PathBuf,
    /// Accept `http://` loopback origins. Local development only.
    #[serde(default)]
    pub allow_loopback_http: bool,
    /// Allow webhook destinations on loopback and private networks. Tests
    /// and local receivers only; never in a deployed bridge.
    #[serde(default)]
    pub allow_private_webhooks: bool,
    /// Serve the mock provider's login and approval pages.
    #[serde(default)]
    pub mock_browser: bool,
    pub tenants: Vec<Tenant>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Tenant {
    pub id: String,
    pub name: String,
    pub connections: Vec<Connection>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Connection {
    pub id: String,
    pub kind: String,
    /// `local`, `staging` or `production`; each is a separate connection.
    pub environment: String,
    pub display_name: String,
    #[serde(default = "enabled")]
    pub enabled: bool,
}

fn enabled() -> bool {
    true
}

impl Config {
    pub fn load(path: &Path) -> Result<Self, String> {
        let text =
            fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        let mut config: Self = ember_protocol::json::parse_as(&text, 64 * 1024)
            .map_err(|error| format!("invalid configuration: {error}"))?;
        // Relative paths are relative to the configuration file.
        let base = path.parent().unwrap_or(Path::new("."));
        for file in [&mut config.database, &mut config.secrets] {
            if file.is_relative() {
                *file = base.join(&*file);
            }
        }
        config.validate()?;
        Ok(config)
    }

    pub fn policy(&self) -> OriginPolicy {
        if self.allow_loopback_http {
            OriginPolicy::AllowLoopbackHttp
        } else {
            OriginPolicy::HttpsOnly
        }
    }

    pub fn validate(&self) -> Result<(), String> {
        if !is_prefixed_id(&self.bridge_id, "brg") {
            return Err("bridge_id must be brg_<uuid v4>".into());
        }
        check_origin(&self.origin, self.policy())
            .map_err(|_| "origin must be a bare https origin".to_owned())?;
        if self.display_name.is_empty() || self.display_name.len() > 128 {
            return Err("display_name must be 1 to 128 bytes".into());
        }
        let mut seen = std::collections::BTreeSet::new();
        for tenant in &self.tenants {
            if !is_slug(&tenant.id) || !seen.insert(tenant.id.clone()) {
                return Err(format!("invalid or duplicate tenant id {:?}", tenant.id));
            }
            for connection in &tenant.connections {
                if !is_slug(&connection.id) || !seen.insert(connection.id.clone()) {
                    return Err(format!(
                        "invalid or duplicate connection id {:?}",
                        connection.id
                    ));
                }
                if !SUPPORTED_KINDS.contains(&connection.kind.as_str()) {
                    return Err(format!("unsupported provider kind {:?}", connection.kind));
                }
                if !matches!(
                    connection.environment.as_str(),
                    "local" | "staging" | "production"
                ) {
                    return Err(format!("invalid environment for {}", connection.id));
                }
            }
        }
        Ok(())
    }

    pub fn connection(&self, id: &str) -> Option<(&Tenant, &Connection)> {
        self.tenants.iter().find_map(|tenant| {
            tenant
                .connections
                .iter()
                .find(|connection| connection.id == id)
                .map(|connection| (tenant, connection))
        })
    }
}

/// `^[a-z][a-z0-9-]{0,63}$`.
pub fn is_slug(text: &str) -> bool {
    let bytes = text.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 64
        && bytes[0].is_ascii_lowercase()
        && bytes
            .iter()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || *b == b'-')
}
