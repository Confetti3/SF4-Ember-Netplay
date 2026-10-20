//! What `deploy/setup.sh` installs: the deployment's rules for bridge.json,
//! kept apart from loading a configuration. `ember-bridge apply-tenants` runs
//! `Config::deployed`.
use super::Config;

/// The file names `deploy/backup.sh` and `deploy/restore.sh` work on, which a
/// deployed configuration must keep.
const DEPLOYED_FILES: [(&str, &str); 2] = [
    ("database", "bridge.sqlite3"),
    ("secrets", "bridge-secrets.json"),
];

impl Config {
    /// What `deploy/setup.sh` installs: `installed`, a bridge.json, with the
    /// top-level settings of a tenants file over it and the development
    /// switches off. Returns the file to write and the configuration it loads
    /// as, and refuses one that `Config::validate` or the strict parse
    /// refuses, or that names other files than the deployment's backups
    /// expect. A change only the database refuses at start (a connection
    /// changing kind) still shows at the restart, which puts the previous
    /// files back.
    pub fn deployed(installed: &[u8], tenants: &[u8]) -> Result<(Vec<u8>, Self), String> {
        let object = |text: &[u8], what: &str| match serde_json::from_slice(text) {
            Ok(serde_json::Value::Object(members)) => Ok(members),
            Ok(_) => Err(format!("{what} is not a JSON object")),
            Err(error) => Err(format!("{what} is not valid JSON: {error}")),
        };
        let mut config = object(installed, "bridge.json")?;
        config.extend(object(tenants, "the tenants file")?);
        for (key, expected) in DEPLOYED_FILES {
            let found = config.get(key);
            if found.and_then(serde_json::Value::as_str) != Some(expected) {
                let found = found.map_or("nothing".to_owned(), ToString::to_string);
                return Err(format!(
                    "bridge.json has {key} {found}; this deployment expects {expected:?}"
                ));
            }
        }
        for switch in [
            "allow_loopback_http",
            "allow_private_webhooks",
            "mock_browser",
        ] {
            config.insert(switch.into(), false.into());
        }
        let mut text = serde_json::to_vec_pretty(&config).map_err(|error| error.to_string())?;
        text.push(b'\n');
        let parsed = Self::parse(&text)?;
        parsed.validate()?;
        Ok((text, parsed))
    }
}
