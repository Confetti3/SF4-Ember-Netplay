//! What every bridge operation needs inside a blocking transaction.
use std::sync::Arc;

use crate::{
    AppState, Keys,
    config::Config,
    error::{ApiFailure, Result},
};

/// What every operation needs, cloned into the blocking transaction.
#[derive(Clone)]
pub struct Ctx {
    pub config: Arc<Config>,
    pub keys: Arc<Keys>,
    pub now: u64,
}

impl Ctx {
    pub fn of(state: &AppState) -> Self {
        Self {
            config: state.config.clone(),
            keys: state.keys.clone(),
            now: state.now(),
        }
    }

    pub fn tenant_of(&self, connection_id: &str) -> Result<String> {
        self.config
            .connection(connection_id)
            .map(|(tenant, _)| tenant.id.clone())
            .ok_or_else(ApiFailure::not_found)
    }

    pub fn provider_label(&self, connection_id: &str) -> String {
        self.config
            .connection(connection_id)
            .map_or_else(String::new, |(_, connection)| {
                connection.display_name.clone()
            })
    }
}
