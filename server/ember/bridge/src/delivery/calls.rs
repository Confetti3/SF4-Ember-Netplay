//! Discord calls (docs/design/TOURNAMENT_DESK.md): posts to the Discord
//! channel webhook an organizer gave their desk event. A set is called when
//! the desk creates its Ember match and each time the organizer calls it
//! again: its round, its two players (pinged when their Discord account is
//! connected to their Ember ID) and the match link, which each player also
//! gets privately (`delivery::dms`). With the event's
//! `post_results`, a finished set's score is posted too.
//!
//! A post is built when it is queued and kept as built; the webhook is read
//! sealed at each attempt, so a webhook the organizer changes or removes
//! takes the waiting posts with it. Discord's 429 carries `Retry-After`,
//! which the shared schedule honours; a webhook Discord no longer knows
//! (401, 403, 404) or a post it refuses (400) is not tried again.
use ember_protocol::play::play_url;
use reqwest::Url;
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::{Value, json};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled, webhooks::post_signed};
use crate::{AppState, error::Result};

/// Discord's own hosts for channel webhooks.
const DISCORD_HOSTS: [&str; 4] = [
    "discord.com",
    "discordapp.com",
    "ptb.discord.com",
    "canary.discord.com",
];

/// The webhook URL `text` names, when it is a Discord channel webhook:
/// `https://discord.com/api/webhooks/<id>/<token>`. A loopback `http` origin
/// is taken too with `allow_loopback_http`, for tests.
pub fn check_webhook(text: &str, allow_loopback_http: bool) -> Option<String> {
    let url = Url::parse(text.trim()).ok()?;
    let host = url.host_str()?;
    let discord = url.scheme() == "https" && url.port().is_none() && DISCORD_HOSTS.contains(&host);
    let loopback =
        allow_loopback_http && url.scheme() == "http" && matches!(host, "127.0.0.1" | "localhost");
    let parts: Vec<&str> = url.path_segments()?.collect();
    let shaped = matches!(parts.as_slice(), ["api", "webhooks", id, token]
        if (1..=24).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_digit())
            && (1..=128).contains(&token.len())
            && token.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_'));
    ((discord || loopback)
        && shaped
        && url.query().is_none()
        && url.fragment().is_none()
        && url.username().is_empty())
    .then(|| url.to_string())
}

/// What the desk shows of a webhook: its host and the last digits of its
/// ID, never its token.
pub fn masked(url: &str) -> Value {
    let url = Url::parse(url).ok();
    let id = url
        .as_ref()
        .and_then(|url| url.path_segments()?.nth(2).map(str::to_owned))
        .unwrap_or_default();
    let tail = &id[id.len().saturating_sub(4)..];
    json!({
        "host": url.as_ref().and_then(Url::host_str),
        "id": format!("…{tail}"),
    })
}

/// A set of a desk event as the desk shows it (`routes::organizer`): its
/// round, its players by Ember ID and name, and its Ember match.
pub struct Called<'a> {
    pub event_id: &'a str,
    pub set: &'a Value,
}

/// Queues a call for `set` when its event has a webhook, and with `dms` a
/// private message with the match link to each player (`delivery::dms`).
/// True when anything was queued.
pub fn call(
    tx: &Transaction<'_>,
    now: u64,
    bridge_id: &str,
    called: &Called,
    dms: bool,
) -> Result<bool> {
    let webhook = has_webhook(tx, called.event_id)?;
    let set = called.set;
    let match_id = set["match_id"].as_str();
    let dms = dms && match_id.is_some();
    if !webhook && !dms {
        return Ok(false);
    }
    let games_to_win: Option<u32> = match match_id {
        Some(id) => tx
            .query_row(
                "SELECT games_to_win FROM matches WHERE id = ?1",
                [id],
                |row| row.get(0),
            )
            .optional()?,
        None => None,
    };
    let (players, discord) = players(tx, set)?;
    let heading = heading(set, games_to_win);
    let link = match match_id {
        Some(id) => format!("Play in Ember: {}", play_url(bridge_id, id)),
        None => "The match is being set up in Ember.".to_owned(),
    };
    if webhook {
        let mentions: Vec<&String> = discord.iter().flatten().collect();
        let content = [format!("**{heading}**"), players.join(" vs "), link.clone()].join("\n");
        let body = json!({
            "content": content,
            "allowed_mentions": { "parse": [], "users": mentions },
        });
        queue(tx, now, called, "call", &body)?;
    }
    if dms {
        let names = names(set);
        let set_id = set["id"].as_str().unwrap_or_default();
        for (slot, user) in discord.iter().enumerate() {
            let content = format!(
                "Your set is ready: **{heading}**\n**{}** vs **{}**\n{link}",
                names[slot],
                names[1 - slot]
            );
            super::dms::queue(
                tx,
                now,
                called.event_id,
                set_id,
                slot,
                user.as_deref(),
                &content,
            )?;
        }
    }
    Ok(true)
}

/// Queues the score of a finished set when its event posts results. True
/// when queued.
pub fn result(tx: &Transaction<'_>, now: u64, called: &Called) -> Result<bool> {
    let posts: Option<bool> = tx
        .query_row(
            "SELECT post_results AND discord_webhook IS NOT NULL FROM organizer_events WHERE id = ?1",
            [called.event_id],
            |row| row.get(0),
        )
        .optional()?;
    if posts != Some(true) {
        return Ok(false);
    }
    let set = called.set;
    let names = names(set);
    let score = &set["score"];
    let draws = match set["draws"].as_u64().unwrap_or(0) {
        0 => String::new(),
        1 => " (1 draw)".to_owned(),
        n => format!(" ({n} draws)"),
    };
    let content = format!(
        "**{}**: {} {} – {} {}{draws}",
        heading(set, None),
        names[0],
        score[0].as_u64().unwrap_or(0),
        score[1].as_u64().unwrap_or(0),
        names[1],
    );
    let body = json!({ "content": content, "allowed_mentions": { "parse": [] } });
    queue(tx, now, called, "result", &body)?;
    Ok(true)
}

fn has_webhook(tx: &Transaction<'_>, event_id: &str) -> Result<bool> {
    Ok(tx
        .query_row(
            "SELECT discord_webhook IS NOT NULL FROM organizer_events WHERE id = ?1",
            [event_id],
            |row| row.get(0),
        )
        .optional()?
        .unwrap_or(false))
}

fn queue(tx: &Transaction<'_>, now: u64, called: &Called, kind: &str, body: &Value) -> Result<()> {
    tx.execute(
        "INSERT INTO desk_posts (event_id, set_id, kind, body, next_attempt_at, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
        params![
            called.event_id,
            called.set["id"].as_str().unwrap_or_default(),
            kind,
            body.to_string(),
            now
        ],
    )?;
    crate::db::queued();
    Ok(())
}

/// "Losers Quarter-Final · Top 8 · first to 3", from what the set has.
fn heading(set: &Value, games_to_win: Option<u32>) -> String {
    let mut parts = vec![
        set["round_label"]
            .as_str()
            .filter(|label| !label.is_empty())
            .unwrap_or("Set")
            .to_owned(),
    ];
    if let Some(phase) = set["phase"].as_str().filter(|phase| !phase.is_empty()) {
        parts.push(phase.to_owned());
    }
    if let Some(games) = games_to_win {
        parts.push(format!("first to {games}"));
    }
    parts.join(" · ")
}

/// The set's two players by the name their site account has, or by slot.
fn names(set: &Value) -> [String; 2] {
    [0, 1].map(|slot| {
        set["players"][slot]
            .as_str()
            .map(str::to_owned)
            .unwrap_or_else(|| format!("Player {}", slot + 1))
    })
}

/// Each player as the call names them, with a ping when their Ember ID has
/// a Discord account connected, or else their bracket site has one, and
/// each player's Discord ID, if any.
fn players(tx: &Transaction<'_>, set: &Value) -> Result<(Vec<String>, Vec<Option<String>>)> {
    let names = names(set);
    let mut shown = Vec::new();
    let mut mentions = Vec::new();
    for (slot, name) in names.into_iter().enumerate() {
        let discord: Option<String> = match set["ember_ids"][slot].as_str() {
            Some(ember_id) => tx
                .query_row(
                    "SELECT user_id FROM discord_accounts WHERE ember_id = ?1",
                    [ember_id],
                    |row| row.get(0),
                )
                .optional()?,
            None => None,
        }
        // Else the Discord the player linked on the site, as the connector
        // read it (start.gg's), when it is a Discord user ID.
        .or_else(|| {
            set["discord"][slot]
                .as_str()
                .filter(|id| {
                    !id.is_empty() && id.len() <= 20 && id.bytes().all(|b| b.is_ascii_digit())
                })
                .map(str::to_owned)
        });
        match &discord {
            Some(id) => shown.push(format!("**{name}** (<@{id}>)")),
            None => shown.push(format!("**{name}**")),
        }
        mentions.push(discord);
    }
    Ok((shown, mentions))
}

/// The latest call of each set of an event: when it was queued and whether
/// Discord has it (`pending`, `delivered`, `failed`).
pub fn calls(
    tx: &Transaction<'_>,
    event_id: &str,
) -> Result<std::collections::HashMap<String, Value>> {
    Ok(tx
        .prepare(
            "SELECT set_id, created_at, state FROM desk_posts
             WHERE id IN (SELECT MAX(id) FROM desk_posts
                          WHERE event_id = ?1 AND kind = 'call' GROUP BY set_id)",
        )?
        .query_map([event_id], |row| {
            let state: String = row.get(2)?;
            Ok((
                row.get(0)?,
                json!({
                    "at": row.get::<_, u64>(1)?,
                    "posted": if state == "dead" { "failed" } else { state.as_str() },
                }),
            ))
        })?
        .collect::<rusqlite::Result<_>>()?)
}

pub struct Due {
    id: i64,
    body: Vec<u8>,
    webhook: Vec<u8>,
}

/// Posts waiting for Discord, whose event still has a webhook.
pub struct Calls;

impl Queue for Calls {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let rows = tx
            .prepare(
                "SELECT p.id, p.attempts, p.first_attempt_at, p.body, e.discord_webhook
                 FROM desk_posts p JOIN organizer_events e ON e.id = p.event_id
                 WHERE p.state = 'pending' AND p.next_attempt_at <= ?1
                   AND e.discord_webhook IS NOT NULL
                 ORDER BY p.next_attempt_at, p.id LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                Ok(Leased {
                    attempt: row.get::<_, u32>(1)? + 1,
                    first_attempt_at: row.get(2)?,
                    item: Due {
                        id: row.get(0)?,
                        body: row.get::<_, String>(3)?.into_bytes(),
                        webhook: row.get(4)?,
                    },
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE desk_posts SET next_attempt_at = ?1, attempts = ?2 WHERE id = ?3",
                params![until, row.attempt, row.item.id],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let Some(url) = state
            .keys
            .open(&item.webhook)
            .and_then(|url| String::from_utf8(url.to_vec()).ok())
        else {
            return Outcome::Refused("webhook unreadable".to_owned());
        };
        match post_signed(state, &url, None, item.body.clone(), async { true }).await {
            Err(outcome) => outcome,
            Ok(answer) if (200..300).contains(&answer.status) => Outcome::Delivered,
            Ok(answer) if matches!(answer.status, 400 | 401 | 403 | 404) => {
                Outcome::Refused(format!("status {}", answer.status))
            }
            Ok(answer) => Outcome::Retry {
                error: format!("status {}", answer.status),
                retry_after: answer.retry_after,
            },
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (status, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("pending", *at, Some(error)),
            Settled::Refused(error) | Settled::Expired(error) => ("dead", record.at, Some(error)),
        };
        tx.execute(
            "UPDATE desk_posts SET state = ?1, next_attempt_at = ?2,
                first_attempt_at = COALESCE(first_attempt_at, ?3), last_error = ?4
             WHERE id = ?5 AND state = 'pending' AND attempts = ?6",
            params![
                status,
                next,
                record.first_attempt_at,
                error,
                leased.item.id,
                leased.attempt
            ],
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_discord_channel_webhooks_are_taken_and_shown_masked() {
        let good = "https://discord.com/api/webhooks/123456789012345678/abc-DEF_123";
        assert_eq!(check_webhook(good, false).as_deref(), Some(good));
        assert!(check_webhook("https://discordapp.com/api/webhooks/1/t", false).is_some());
        for bad in [
            "http://discord.com/api/webhooks/1/t",
            "https://evil.example/api/webhooks/1/t",
            "https://discord.com.evil.example/api/webhooks/1/t",
            "https://discord.com:8443/api/webhooks/1/t",
            "https://discord.com/api/webhooks/x/t",
            "https://discord.com/api/webhooks/1/t?wait=true",
            "https://discord.com/api/webhooks/1",
            "http://127.0.0.1:9/api/webhooks/1/t",
        ] {
            assert!(check_webhook(bad, false).is_none(), "{bad}");
        }
        assert!(check_webhook("http://127.0.0.1:9/api/webhooks/1/t", true).is_some());
        let shown = masked(good);
        assert_eq!(shown, json!({ "host": "discord.com", "id": "…5678" }));
        assert!(!shown.to_string().contains("abc"));
    }

    #[test]
    fn a_heading_says_what_the_set_has() {
        let set = json!({ "round_label": "Losers Quarter-Final", "phase": "Top 8" });
        assert_eq!(
            heading(&set, Some(3)),
            "Losers Quarter-Final · Top 8 · first to 3"
        );
        assert_eq!(heading(&json!({}), None), "Set");
    }
}
