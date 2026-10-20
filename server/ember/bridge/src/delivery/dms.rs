//! Private match messages (docs/design/TOURNAMENT_DESK.md): when the desk
//! calls a set, each player whose Ember ID has a Discord account connected
//! gets the match link in a direct message from the bridge's Discord bot
//! (`discord_bot_token` in the integration secrets). Discord lets a bot
//! message only someone who shares a server with it and takes messages from
//! it; a refused message (Discord's 400, 403 or 404) is not tried again, and
//! the desk says which player was not reached. A player without Discord is
//! kept as `no_discord` for the same reason.
use std::collections::HashMap;

use rusqlite::{Transaction, params};
use serde_json::{Value, json};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled};
use crate::{AppState, error::Result};

/// Whether the bridge sends private match messages: a Discord app and its
/// bot's token.
pub fn on(state: &AppState) -> bool {
    state.config.discord.is_some() && state.integrations.discord_bot_token.is_some()
}

/// Queues a message to the set's player in `slot`, or keeps them as
/// `no_discord` when `user_id` is none.
pub fn queue(
    tx: &Transaction<'_>,
    now: u64,
    event_id: &str,
    set_id: &str,
    slot: usize,
    user_id: Option<&str>,
    content: &str,
) -> Result<()> {
    let body = json!({ "content": content, "allowed_mentions": { "parse": [] } });
    tx.execute(
        "INSERT INTO desk_dms (event_id, set_id, slot, user_id, body, state, next_attempt_at, created_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7)",
        params![
            event_id,
            set_id,
            slot,
            user_id,
            body.to_string(),
            if user_id.is_some() { "pending" } else { "no_discord" },
            now
        ],
    )?;
    Ok(())
}

/// Each set's latest call's messages, by player slot: `pending`,
/// `delivered`, `failed` (Discord refused it) or `no_discord`.
pub fn status(tx: &Transaction<'_>, event_id: &str) -> Result<HashMap<String, Value>> {
    let mut found: HashMap<String, Value> = HashMap::new();
    let rows = tx
        .prepare(
            "SELECT set_id, slot, state FROM desk_dms d
             WHERE event_id = ?1 AND id IN (SELECT MAX(id) FROM desk_dms
                                            WHERE event_id = ?1 GROUP BY set_id, slot)",
        )?
        .query_map([event_id], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, usize>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for (set, slot, state) in rows {
        let entry = found.entry(set).or_insert_with(|| json!([null, null]));
        entry[slot] = json!(if state == "dead" {
            "failed"
        } else {
            state.as_str()
        });
    }
    Ok(found)
}

pub struct Due {
    id: i64,
    user_id: String,
    body: Vec<u8>,
}

/// Messages waiting for Discord.
pub struct Dms;

impl Queue for Dms {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let rows = tx
            .prepare(
                "SELECT id, attempts, first_attempt_at, user_id, body FROM desk_dms
                 WHERE state = 'pending' AND next_attempt_at <= ?1
                 ORDER BY next_attempt_at, id LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                Ok(Leased {
                    attempt: row.get::<_, u32>(1)? + 1,
                    first_attempt_at: row.get(2)?,
                    item: Due {
                        id: row.get(0)?,
                        user_id: row.get(3)?,
                        body: row.get::<_, String>(4)?.into_bytes(),
                    },
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE desk_dms SET next_attempt_at = ?1, attempts = ?2 WHERE id = ?3",
                params![until, row.attempt, row.item.id],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        let (Some(discord), Some(token)) =
            (&state.config.discord, &state.integrations.discord_bot_token)
        else {
            return Outcome::Refused("no Discord bot".to_owned());
        };
        let Ok(client) = crate::util::outbound_client().build() else {
            return retry("client", None);
        };
        let send = |url: String, body: Vec<u8>| {
            client
                .post(url)
                .header("authorization", format!("Bot {}", token.as_str()))
                .header("content-type", "application/json")
                .body(body)
                .send()
        };
        // A direct message goes to the DM channel Discord opens for the user.
        let opened = send(
            format!("{}/v10/users/@me/channels", discord.api_base),
            json!({ "recipient_id": item.user_id })
                .to_string()
                .into_bytes(),
        )
        .await;
        let channel = match answer(opened).await {
            Ok(body) => match body["id"].as_str() {
                Some(id) if id.bytes().all(|b| b.is_ascii_digit()) => id.to_owned(),
                _ => return retry("no channel", None),
            },
            Err(outcome) => return outcome,
        };
        let sent = send(
            format!("{}/v10/channels/{channel}/messages", discord.api_base),
            item.body.clone(),
        )
        .await;
        match answer(sent).await {
            Ok(_) => Outcome::Delivered,
            Err(outcome) => outcome,
        }
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (status, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("pending", *at, Some(error)),
            Settled::Refused(error) | Settled::Expired(error) => ("dead", record.at, Some(error)),
        };
        tx.execute(
            "UPDATE desk_dms SET state = ?1, next_attempt_at = ?2,
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

fn retry(error: &str, retry_after: Option<u64>) -> Outcome {
    Outcome::Retry {
        error: error.to_owned(),
        retry_after,
    }
}

/// Discord's answer as JSON, or what the queue does with a failure: a
/// refusal (400, 401, 403, 404, such as a user who takes no messages from
/// the bot) is final, a 429 waits its `Retry-After`, anything else retries.
async fn answer(sent: reqwest::Result<reqwest::Response>) -> std::result::Result<Value, Outcome> {
    let Ok(response) = sent else {
        return Err(retry("unreachable", None));
    };
    let status = response.status().as_u16();
    let retry_after = response
        .headers()
        .get("retry-after")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.parse::<f64>().ok())
        .map(|seconds| seconds.ceil() as u64);
    match status {
        200..=299 => Ok(response
            .bytes()
            .await
            .ok()
            .and_then(|body| serde_json::from_slice(&body).ok())
            .unwrap_or(Value::Null)),
        400 | 401 | 403 | 404 => Err(Outcome::Refused(format!("status {status}"))),
        _ => Err(retry(&format!("status {status}"), retry_after)),
    }
}
