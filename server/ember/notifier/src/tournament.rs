//! Discord's `/tournament` command and its buttons: signing up, leaving and
//! checking in on the bracket site (docs/design/BRACKET_SITE.md) as the
//! Discord user who asked, and showing a tournament with those buttons.
//!
//! Discord signs each interaction, so the user in it is who pressed; the
//! site takes that user from the bot's token alone (`X-Discord-User`).
use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
};
use reqwest::Method;
use serde_json::{Value, json};

use crate::{
    Notifier,
    bot::BUSY,
    config::resolve,
    format::escape_markdown,
    interactions::{CommandData, Interaction, json_response},
};

pub(crate) const COMMAND: &str = "tournament";
const EPHEMERAL: u64 = 64;
const DEFERRED_MESSAGE: u8 = 5;

#[derive(Clone, Copy, PartialEq)]
enum Ask {
    SignUp,
    Leave,
    CheckIn,
    Show,
}

impl Ask {
    fn of(name: &str) -> Option<Self> {
        Some(match name {
            "signup" => Self::SignUp,
            "leave" => Self::Leave,
            "checkin" => Self::CheckIn,
            "show" => Self::Show,
            _ => return None,
        })
    }
}

/// What a command or a button asks, and of which tournament: a subcommand
/// with an `id`, or a button's `tournament:<ask>:<id>`.
fn asked(data: &CommandData) -> Option<(Ask, String)> {
    let (ask, id) = match data.custom_id.strip_prefix("tournament:") {
        Some(rest) => {
            let (ask, id) = rest.split_once(':')?;
            (Ask::of(ask)?, id.to_owned())
        }
        None => {
            let sub = data.options.first()?;
            let id = sub.options.iter().find(|option| option.name == "id")?;
            (Ask::of(&sub.name)?, id.value.as_ref()?.as_str()?.to_owned())
        }
    };
    let slug = (1..=64).contains(&id.len())
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-');
    slug.then_some((ask, id))
}

impl Notifier {
    /// Answers at once with a deferred reply, private but for `show`, and
    /// settles it once the site has answered.
    pub(crate) fn tournament(&self, interaction: Interaction) -> Response {
        let asked = interaction.data.as_ref().and_then(asked);
        let (Some((ask, id)), Some(user), true, true) = (
            asked,
            interaction.sender(),
            self.0.config.brackets.is_some(),
            interaction.token_valid(),
        ) else {
            return StatusCode::BAD_REQUEST.into_response();
        };
        let name = user
            .global_name
            .clone()
            .filter(|name| !name.is_empty())
            .unwrap_or_else(|| user.username.clone());
        // Posting a tournament to the channel is for the server's managers
        // and the tournament's organizers; anyone else is told so privately.
        let manager = interaction.manages_server();
        let public = ask == Ask::Show && manager;
        let (this, token, discord) = (self.clone(), interaction.token.clone(), user.id.clone());
        tokio::spawn(async move {
            let (message, organizers) = this.ask_site(ask, &id, &discord, &name).await;
            if ask != Ask::Show || public || message.get("components").is_none() {
                this.edit_original_with(&token, message).await;
            } else if organizers.contains(&discord) {
                // The private reply is settled first, so the followup is public.
                if this
                    .edit_original_with(&token, json!({ "content": "Posted." }))
                    .await
                {
                    this.webhook(Method::POST, &token, "", message).await;
                }
            } else {
                let refused =
                    "Only the server's managers and the tournament's organizers can post it.";
                this.edit_original_with(&token, json!({ "content": refused }))
                    .await;
            }
        });
        let flags = if ask == Ask::Show && public {
            0
        } else {
            EPHEMERAL
        };
        json_response(json!({ "type": DEFERRED_MESSAGE, "data": { "flags": flags } }))
    }

    /// The site's answer, as the message to show, and for `show` the
    /// tournament's organizers.
    async fn ask_site(
        &self,
        ask: Ask,
        id: &str,
        discord: &str,
        name: &str,
    ) -> (Value, Vec<String>) {
        let (message, answer) = self.answer(ask, id, discord, name).await;
        let organizers = answer["organizers"]
            .as_array()
            .map(|all| {
                all.iter()
                    .filter_map(|each| each.as_str().map(str::to_owned))
                    .collect()
            })
            .unwrap_or_default();
        (message, organizers)
    }

    /// The message to show, and the site's answer it was made from (null on
    /// a failure).
    async fn answer(&self, ask: Ask, id: &str, discord: &str, name: &str) -> (Value, Value) {
        let Some(brackets) = &self.0.config.brackets else {
            return (json!({ "content": BUSY }), Value::Null);
        };
        let base = brackets.base.trim_end_matches('/');
        let (method, path, body) = match ask {
            Ask::SignUp => (
                Method::POST,
                "/entrants".to_owned(),
                Some(json!({ "name": name })),
            ),
            Ask::Leave => (Method::DELETE, format!("/entrants/{discord}"), None),
            Ask::CheckIn => (Method::POST, "/check-in".to_owned(), Some(json!({}))),
            Ask::Show => (Method::GET, String::new(), None),
        };
        let Ok(token) = resolve(&brackets.token) else {
            return (json!({ "content": BUSY }), Value::Null);
        };
        let mut request = self
            .0
            .http
            .request(method, format!("{base}/tournaments/{id}{path}"))
            .bearer_auth(token.as_str())
            .header("x-discord-user", discord);
        if let Some(body) = body {
            request = request.json(&body);
        }
        let Ok(response) = request.send().await else {
            return (json!({ "content": BUSY }), Value::Null);
        };
        let status = response.status();
        let answer: Value = response.json().await.unwrap_or(Value::Null);
        if status.is_server_error() {
            return (json!({ "content": BUSY }), Value::Null);
        }
        if !status.is_success() {
            let said = answer["error"]
                .as_str()
                .unwrap_or("the bracket site refused it");
            return (
                json!({ "content": format!("That did not work: {}.", escape_markdown(said)) }),
                Value::Null,
            );
        }
        let link = format!("{base}/t/{id}");
        let message = json!({
            "content": match ask {
                Ask::SignUp if answer["ember_id"].is_null() => format!(
                    "You are signed up ({link}). Connect Discord in Ember (Online play, Ember ID, Discord) before the start, or your entry is dropped."
                ),
                Ask::SignUp => format!("You are signed up ({link})."),
                Ask::Leave => format!("You left the tournament ({link})."),
                Ask::CheckIn => format!("You are checked in ({link})."),
                Ask::Show => return (shown(&answer, id, &link), answer),
            }
        });
        (message, answer)
    }
}

/// A tournament as a message anyone in the channel can act on.
fn shown(tournament: &Value, id: &str, link: &str) -> Value {
    let state = tournament["state"].as_str().unwrap_or_default();
    let said = match state {
        "draft" | "planned" => "sign-ups are not open yet",
        "sign_ups" => "sign-ups are open",
        "check_in" => "check-in is open",
        "live" => "live",
        "cancelled" => "cancelled",
        _ => "finished",
    };
    let content = format!(
        "**{}**: {said}. First to {}, {} signed up. {link}",
        escape_markdown(tournament["title"].as_str().unwrap_or("Tournament")),
        tournament["games_to_win"].as_u64().unwrap_or(2),
        tournament["entrants"].as_array().map_or(0, Vec::len),
    );
    let button = |label: &str, ask: &str, style: u8| json!({ "type": 2, "style": style, "label": label, "custom_id": format!("tournament:{ask}:{id}") });
    let buttons: Vec<Value> = match state {
        "sign_ups" => vec![button("Sign up", "signup", 1), button("Leave", "leave", 4)],
        "check_in" => vec![
            button("Check in", "checkin", 1),
            button("Leave", "leave", 4),
        ],
        _ => Vec::new(),
    };
    if buttons.is_empty() {
        return json!({ "content": content, "components": [] });
    }
    json!({ "content": content, "components": [{ "type": 1, "components": buttons }] })
}

/// The command `discord-register` registers beside `/room`.
pub(crate) fn command() -> Value {
    let sub = |name: &str, description: &str| {
        json!({
            "type": 1, "name": name, "description": description,
            "options": [{ "type": 3, "name": "id", "description": "The tournament's ID", "required": true, "max_length": 64 }],
        })
    };
    json!({
        "name": COMMAND,
        "description": "Ember tournaments",
        "type": 1,
        "options": [
            sub("signup", "Sign up for a tournament"),
            sub("leave", "Leave a tournament before it starts"),
            sub("checkin", "Check in for a tournament"),
            sub("show", "Show a tournament with its sign-up buttons"),
        ],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn data(value: Value) -> CommandData {
        serde_json::from_value(value).unwrap()
    }

    #[test]
    fn a_subcommand_or_a_button_names_what_and_which() {
        let command = data(
            json!({ "name": "tournament", "options": [{ "name": "signup", "options": [{ "name": "id", "value": "weekly-12" }] }] }),
        );
        assert!(asked(&command) == Some((Ask::SignUp, "weekly-12".into())));
        let button = data(json!({ "custom_id": "tournament:checkin:weekly-12" }));
        assert!(asked(&button) == Some((Ask::CheckIn, "weekly-12".into())));
        for bad in [
            json!({ "custom_id": "tournament:delete:weekly" }),
            json!({ "custom_id": "tournament:signup:a/b" }),
            json!({ "name": "tournament", "options": [{ "name": "signup", "options": [] }] }),
            json!({ "name": "tournament", "options": [{ "name": "show", "options": [{ "name": "id", "value": 7 }] }] }),
        ] {
            assert!(asked(&data(bad.clone())).is_none(), "{bad}");
        }
    }

    #[test]
    fn a_tournament_shows_the_buttons_its_state_allows() {
        let tournament = |state: &str| json!({ "title": "Weekly *12*", "state": state, "games_to_win": 2, "entrants": [{}, {}] });
        let open = shown(&tournament("sign_ups"), "weekly", "https://b/t/weekly");
        assert_eq!(
            open["content"],
            "**Weekly \\*12\\***: sign-ups are open. First to 2, 2 signed up. https://b/t/weekly"
        );
        let ids: Vec<&str> = open["components"][0]["components"]
            .as_array()
            .unwrap()
            .iter()
            .map(|button| button["custom_id"].as_str().unwrap())
            .collect();
        assert_eq!(ids, ["tournament:signup:weekly", "tournament:leave:weekly"]);
        assert_eq!(
            shown(&tournament("check_in"), "weekly", "")["components"][0]["components"][0]["label"],
            "Check in"
        );
        assert_eq!(
            shown(&tournament("live"), "weekly", "")["components"],
            json!([])
        );
    }
}
