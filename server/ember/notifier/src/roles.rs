//! Discord roles for awards (docs/design/BRACKET_SITE.md, "Ranks, titles,
//! emblems and awards"), in the one server `brackets.roles` names: every
//! `feed_secs` the notifier reads the bracket site's awards past the last
//! it handled, and gives each award's player its kind's role, which it made
//! itself. It never changes or removes a role, and touches none it did not
//! create. A server without the setting gets nothing.
use std::time::Duration;

use reqwest::{Method, StatusCode};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{Notifier, config::resolve};

/// Discord's "Unknown Role": the role this bot made is gone.
const UNKNOWN_ROLE: u64 = 10011;

#[derive(Deserialize)]
struct Awards {
    awards: Vec<Award>,
}

#[derive(Deserialize)]
struct Award {
    n: u64,
    discord: String,
    kind: String,
    title: String,
}

enum Given {
    /// Done with this award: given, or nothing to give (not a member).
    Done,
    /// Try it again next round: Discord refused or did not answer.
    Later(String),
}

impl Notifier {
    pub(crate) fn gives_roles(&self) -> bool {
        self.0
            .config
            .brackets
            .as_ref()
            .is_some_and(|brackets| brackets.roles.is_some())
    }

    pub(crate) async fn follow_awards(self) {
        let secs = self
            .0
            .config
            .brackets
            .as_ref()
            .map_or(60, |brackets| brackets.feed_secs.max(10));
        loop {
            self.give_roles().await;
            tokio::time::sleep(Duration::from_secs(secs)).await;
        }
    }

    /// One round: each new award, in order, until one must wait.
    pub(crate) async fn give_roles(&self) {
        let Some(brackets) = &self.0.config.brackets else {
            return;
        };
        let Some(roles) = &brackets.roles else {
            return;
        };
        let Ok(token) = resolve(&brackets.token) else {
            return;
        };
        let after = self.award_cursor();
        let url = format!(
            "{}/awards?after={after}",
            brackets.base.trim_end_matches('/')
        );
        let awards = match self
            .0
            .http
            .get(url)
            .bearer_auth(token.as_str())
            .send()
            .await
        {
            Ok(response) if response.status().is_success() => response.json::<Awards>().await.ok(),
            _ => None,
        };
        for award in awards.map(|awards| awards.awards).unwrap_or_default() {
            if roles.kinds.contains(&award.kind)
                && let Given::Later(why) = self.give(&roles.guild_id, &roles.prefix, &award).await
            {
                eprintln!("ember-notifier: the {} role waits: {why}", award.kind);
                return;
            }
            self.set_award_cursor(award.n);
        }
    }

    async fn give(&self, guild: &str, prefix: &str, award: &Award) -> Given {
        let role = match self.award_role(guild, &award.kind) {
            Some(role) => role,
            None => {
                let name: String = format!("{prefix} {}", award.title)
                    .chars()
                    .take(100)
                    .collect();
                let body = json!({ "name": name, "permissions": "0", "mentionable": false, "hoist": false });
                match self
                    .discord_api(Method::POST, &format!("/guilds/{guild}/roles"), Some(body))
                    .await
                {
                    Ok((status, answer)) if status.is_success() => match answer["id"].as_str() {
                        Some(id) => {
                            self.set_award_role(guild, &award.kind, id);
                            id.to_owned()
                        }
                        None => return Given::Later("Discord made a role without an ID".into()),
                    },
                    Ok((status, _)) => {
                        return Given::Later(format!(
                            "making the role: status {}",
                            status.as_u16()
                        ));
                    }
                    Err(why) => return Given::Later(why),
                }
            }
        };
        let path = format!("/guilds/{guild}/members/{}/roles/{role}", award.discord);
        match self.discord_api(Method::PUT, &path, None).await {
            Ok((status, _)) if status.is_success() => Given::Done,
            Ok((StatusCode::NOT_FOUND, answer)) if answer["code"] == UNKNOWN_ROLE => {
                // Someone deleted it: make a new one next round.
                self.forget_award_role(guild, &award.kind);
                Given::Later("its role was deleted".into())
            }
            // Not a member of the server: nothing to give.
            Ok((StatusCode::NOT_FOUND, _)) => Given::Done,
            Ok((status, _)) => Given::Later(format!("giving the role: status {}", status.as_u16())),
            Err(why) => Given::Later(why),
        }
    }

    /// A request to Discord's API as the bot; its status and JSON answer.
    async fn discord_api(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<(StatusCode, Value), String> {
        let interactions = self
            .0
            .config
            .discord
            .as_ref()
            .and_then(|discord| discord.interactions.as_ref())
            .ok_or("discord.interactions is not configured")?;
        let token = resolve(
            interactions
                .bot_token
                .as_deref()
                .ok_or("discord.interactions.bot_token is not configured")?,
        )?;
        let mut request = self
            .0
            .http
            .request(
                method,
                format!("{}{path}", interactions.api_base.trim_end_matches('/')),
            )
            .header("authorization", format!("Bot {}", token.as_str()))
            .header("x-audit-log-reason", "Ember award");
        if let Some(body) = body {
            request = request.json(&body);
        }
        let response = request
            .send()
            .await
            .map_err(|_| "Discord could not be reached".to_owned())?;
        let status = response.status();
        Ok((status, response.json().await.unwrap_or(Value::Null)))
    }

    fn award_cursor(&self) -> u64 {
        self.0
            .db
            .lock()
            .ok()
            .and_then(|db| {
                db.query_row("SELECT n FROM award_cursor WHERE id = 1", [], |row| {
                    row.get(0)
                })
                .ok()
            })
            .unwrap_or(0)
    }

    fn set_award_cursor(&self, n: u64) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "INSERT INTO award_cursor (id, n) VALUES (1, ?1) ON CONFLICT (id) DO UPDATE SET n = MAX(n, excluded.n)",
                [n],
            );
        }
    }

    fn award_role(&self, guild: &str, kind: &str) -> Option<String> {
        let db = self.0.db.lock().ok()?;
        db.query_row(
            "SELECT role FROM award_roles WHERE guild = ?1 AND kind = ?2",
            [guild, kind],
            |row| row.get(0),
        )
        .ok()
    }

    fn set_award_role(&self, guild: &str, kind: &str, role: &str) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "INSERT OR REPLACE INTO award_roles (guild, kind, role) VALUES (?1, ?2, ?3)",
                [guild, kind, role],
            );
        }
    }

    fn forget_award_role(&self, guild: &str, kind: &str) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "DELETE FROM award_roles WHERE guild = ?1 AND kind = ?2",
                [guild, kind],
            );
        }
    }
}
