//! The bracket site's news in a Discord channel: every `feed_secs` the
//! notifier reads each live tournament's feed past what it already posted
//! and posts what is new as one message, so a busy bracket costs one
//! message a minute. A league's news (a closed season's awards) is listed
//! the same way, with its own feed path. What was posted is kept in its
//! database, so a restart posts nothing twice.
use std::time::Duration;

use serde::Deserialize;
use serde_json::json;

use crate::{
    Notifier,
    config::resolve,
    format::escape_markdown,
    sent::{Reset, Sent, classify},
};

/// Discord takes 2000 characters a message.
const MAX_MESSAGE: usize = 1900;

#[derive(Deserialize)]
struct Live {
    tournaments: Vec<Listed>,
}

#[derive(Deserialize)]
struct Listed {
    id: String,
    last: u64,
    /// Where its feed is, when not a tournament's (a league's news).
    #[serde(default)]
    feed: Option<String>,
    /// Its organizers' own Discord channel, which gets its news too.
    #[serde(default)]
    webhook: Option<String>,
}

#[derive(Deserialize)]
struct Feed {
    items: Vec<Item>,
}

#[derive(Deserialize)]
struct Item {
    n: u64,
    text: String,
}

impl Notifier {
    pub(crate) fn follows_feeds(&self) -> bool {
        // The shared channel or a tournament's own: either is somewhere to post.
        self.0
            .config
            .brackets
            .as_ref()
            .is_some_and(|brackets| brackets.feed)
    }

    pub(crate) async fn follow_feeds(self) {
        let secs = self
            .0
            .config
            .brackets
            .as_ref()
            .map_or(60, |brackets| brackets.feed_secs.max(10));
        loop {
            self.post_news().await;
            tokio::time::sleep(Duration::from_secs(secs)).await;
        }
    }

    /// One round: what is new in every live tournament, as one post.
    pub(crate) async fn post_news(&self) {
        let Some(brackets) = &self.0.config.brackets else {
            return;
        };
        let base = brackets.base.trim_end_matches('/');
        let Ok(token) = resolve(&brackets.token) else {
            return;
        };
        let get = |url: String| self.0.http.get(url).bearer_auth(token.as_str()).send();
        let Some(live) = (match get(format!("{base}/live")).await {
            Ok(response) if response.status().is_success() => response.json::<Live>().await.ok(),
            _ => None,
        }) else {
            return;
        };
        // Each tournament with a channel of its own gets its news there too,
        // kept apart from what the shared channel was sent.
        for tournament in &live.tournaments {
            let Some(url) = tournament
                .webhook
                .as_deref()
                .and_then(|url| own_channel(&brackets.webhooks, url))
            else {
                continue;
            };
            let key = format!("hook:{}", tournament.id);
            let after = self.posted(&key);
            if tournament.last <= after {
                continue;
            }
            let Ok(response) = get(format!(
                "{base}/tournaments/{}/feed?after={after}",
                tournament.id
            ))
            .await
            else {
                continue;
            };
            let Ok(feed) = response.json::<Feed>().await else {
                continue;
            };
            let Some(last) = feed.items.last().map(|item| item.n) else {
                continue;
            };
            let lines: Vec<_> = feed
                .items
                .iter()
                .map(|item| escape_markdown(&item.text))
                .collect();
            if let Sent::Done = self.post_to(&url, &message(&lines)).await {
                self.set_posted(&key, last);
            }
        }
        let mut lines = Vec::new();
        let mut seen = Vec::new();
        for tournament in live.tournaments {
            let after = self.posted(&tournament.id);
            if tournament.last <= after {
                continue;
            }
            let path = match &tournament.feed {
                Some(path)
                    if path.starts_with('/') && !path.contains("..") && !path.contains("//") =>
                {
                    path.clone()
                }
                Some(_) => continue,
                None => format!("/tournaments/{}/feed", tournament.id),
            };
            let Ok(response) = get(format!("{base}{path}?after={after}")).await else {
                continue;
            };
            let Ok(feed) = response.json::<Feed>().await else {
                continue;
            };
            if let Some(last) = feed.items.last() {
                seen.push((tournament.id.clone(), last.n));
            }
            lines.extend(
                feed.items
                    .into_iter()
                    .map(|item| escape_markdown(&item.text)),
            );
        }
        if lines.is_empty() {
            return;
        }
        let Some(url) = self.0.discord_url.clone() else {
            return;
        };
        if let Sent::Done = self.post_to(url.as_str(), &message(&lines)).await {
            for (id, n) in seen {
                self.set_posted(&id, n);
            }
        }
    }

    async fn post_to(&self, url: &str, text: &str) -> Sent {
        let username = self
            .0
            .config
            .discord
            .as_ref()
            .map_or("Ember", |config| config.username.as_str());
        // News never pings anyone.
        let body =
            json!({ "username": username, "content": text, "allowed_mentions": { "parse": [] } });
        classify(
            self.0.http.post(url).json(&body).send().await,
            Reset::After("x-ratelimit-reset-after"),
            self.0.clock.now(),
        )
        .await
    }

    fn posted(&self, id: &str) -> u64 {
        self.0
            .db
            .lock()
            .ok()
            .and_then(|db| {
                db.query_row(
                    "SELECT posted FROM feeds WHERE tournament = ?1",
                    [id],
                    |row| row.get(0),
                )
                .ok()
            })
            .unwrap_or(0)
    }

    fn set_posted(&self, id: &str, n: u64) {
        if let Ok(db) = self.0.db.lock() {
            let _ = db.execute(
                "INSERT INTO feeds (tournament, posted) VALUES (?1, ?2)
                 ON CONFLICT (tournament) DO UPDATE SET posted = MAX(posted, excluded.posted)",
                rusqlite::params![id, n],
            );
        }
    }
}

/// The lines as one message, cut where Discord would refuse it.
fn message(lines: &[String]) -> String {
    let mut text = String::new();
    for line in lines {
        if text.len() + line.len() + 1 > MAX_MESSAGE {
            text.push('…');
            break;
        }
        text.push_str(line);
        text.push('\n');
    }
    text.trim_end().to_owned()
}

/// A tournament's own channel, posted to under `base`: only a Discord
/// webhook's ID and token are taken from what the site lists, so no other
/// address is ever called.
fn own_channel(base: &str, url: &str) -> Option<String> {
    let rest = url
        .strip_prefix("https://discord.com/api/webhooks/")
        .or_else(|| url.strip_prefix("https://discordapp.com/api/webhooks/"))?;
    let (id, token) = rest.split_once('/')?;
    let id_ok = (17..=20).contains(&id.len()) && id.bytes().all(|byte| byte.is_ascii_digit());
    let token_ok = (1..=100).contains(&token.len())
        && token
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-');
    (id_ok && token_ok).then(|| format!("{}/{id}/{token}", base.trim_end_matches('/')))
}

#[cfg(test)]
mod tests {
    use super::own_channel;

    #[test]
    fn only_a_discord_webhook_is_a_channel() {
        let base = "https://discord.com/api/webhooks";
        assert_eq!(
            own_channel(
                base,
                "https://discord.com/api/webhooks/123456789012345678/abc_DEF-1"
            )
            .as_deref(),
            Some("https://discord.com/api/webhooks/123456789012345678/abc_DEF-1")
        );
        for url in [
            "https://example.com/api/webhooks/123456789012345678/abc",
            "https://discord.com/api/webhooks/123/abc",
            "https://discord.com/api/webhooks/123456789012345678/a/../b",
            "https://discord.com/api/webhooks/123456789012345678/",
        ] {
            assert_eq!(own_channel(base, url), None, "{url}");
        }
    }
}
