//! start.gg's side of the wire: the queries sent, the answers read, and the
//! pure judgements made from them (what a set is, whether an answer shows a
//! score recorded). `startgg.rs` does the requests.
use serde::Deserialize;
use serde_json::Value;

use crate::{
    http::{id, optional_id},
    site::{Account, Extras, Phase, Status},
};

/// `Set.state` values (section 3.4): not started, started, done, called.
pub(super) const CREATED: i64 = 1;
pub(super) const STARTED: i64 = 2;
pub(super) const COMPLETED: i64 = 3;
pub(super) const CALLED: i64 = 6;

pub(super) const EVENT: &str = "query Event($slug: String) { event(slug: $slug) { id state } }";
pub(super) const ENTRANTS: &str = "query Entrants($id: ID!, $page: Int!, $perPage: Int!) { event(id: $id) { entrants(query: {page: $page, perPage: $perPage}) { pageInfo { totalPages } nodes { id name participants { user { id authorizations(types: [DISCORD]) { externalId } } } } } } }";
/// The query for a page of open sets. `phaseGroup` and `stream` are asked
/// only when a round rule names a phase or sets on a stream are held: each
/// adds objects to the count of the 1000 start.gg allows a request.
/// The sets list query: the open sets (created, started, called), and with
/// `completed` the decided ones too.
pub(super) fn sets_query(phases: bool, streams: bool, completed: bool) -> String {
    let states = if completed { "1, 2, 3, 6" } else { "1, 2, 6" };
    let phase = if phases {
        " phaseGroup { phase { id name } }"
    } else {
        ""
    };
    let stream = if streams {
        " stream { id streamName }"
    } else {
        ""
    };
    format!(
        "query Sets($id: ID!, $page: Int!, $perPage: Int!) {{ event(id: $id) {{ sets(page: $page, perPage: $perPage, sortType: ROUND, filters: {{state: [{states}], hideEmpty: true}}) {{ pageInfo {{ totalPages }} nodes {{ id state round fullRoundText winnerId slots {{ slotIndex entrant {{ id }} }}{phase}{stream} }} }} }} }}"
    )
}

/// One set as it is now: its entrants, result and the games entered, which
/// a report would replace. Not its phase or stream, which only a list of
/// open sets uses.
pub(super) const SET: &str = "query Set($id: ID!) { set(id: $id) { id state round fullRoundText winnerId slots { slotIndex entrant { id } } games { orderNum winnerId entrant1Score entrant2Score stage { id } selections { id } } } }";
pub(super) const UNDERWAY: &str =
    "mutation Underway($setId: ID!) { markSetInProgress(setId: $setId) { id state } }";
pub(super) const REPORT: &str = "mutation Report($setId: ID!, $winnerId: ID!, $gameData: [BracketSetGameDataInput]) { reportBracketSet(setId: $setId, winnerId: $winnerId, gameData: $gameData) { id state winnerId } }";

/// Whether every top-level field of a GraphQL `data` object is there.
pub(super) fn is_whole(data: &Value) -> bool {
    data.as_object()
        .is_some_and(|fields| !fields.is_empty() && fields.values().all(|value| !value.is_null()))
}

/// start.gg's "no such set" for this ID: `Set not found for id: <id>`.
pub(super) fn is_set_not_found(message: &str, id: &str) -> bool {
    message.strip_prefix("Set not found").is_some_and(|rest| {
        rest.split(|c: char| !c.is_ascii_digit())
            .any(|word| word == id)
    })
}

/// A real set's ID: decimal digits. Preview sets carry other IDs.
pub(super) fn is_set_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 20 && id.bytes().all(|byte| byte.is_ascii_digit())
}

#[derive(Deserialize)]
pub(super) struct EventData {
    pub(super) event: Option<EventNode>,
}

#[derive(Deserialize)]
pub(super) struct EventNode {
    #[serde(deserialize_with = "id")]
    pub(super) id: String,
    #[serde(default)]
    pub(super) state: Option<String>,
}

#[derive(Deserialize)]
pub(super) struct Page<T> {
    #[serde(rename = "pageInfo")]
    pub(super) page_info: PageInfo,
    #[serde(default = "Vec::new")]
    pub(super) nodes: Vec<T>,
}

#[derive(Deserialize)]
pub(super) struct PageInfo {
    #[serde(rename = "totalPages", default)]
    pub(super) total_pages: Option<u32>,
}

#[derive(Deserialize)]
pub(super) struct EntrantsData {
    pub(super) event: Option<EntrantsEvent>,
}

#[derive(Deserialize)]
pub(super) struct EntrantsEvent {
    pub(super) entrants: Page<WireEntrant>,
}

#[derive(Deserialize)]
pub(super) struct WireEntrant {
    #[serde(deserialize_with = "id")]
    pub(super) id: String,
    /// The entrant's name: the gamer tag, with its sponsor prefix.
    #[serde(default)]
    pub(super) name: Option<String>,
    #[serde(default)]
    pub(super) participants: Vec<WireParticipant>,
}

#[derive(Deserialize)]
pub(super) struct WireParticipant {
    #[serde(default)]
    pub(super) user: Option<WireUser>,
}

#[derive(Deserialize)]
pub(super) struct WireUser {
    #[serde(deserialize_with = "id")]
    pub(super) id: String,
    #[serde(default)]
    pub(super) authorizations: Option<Vec<WireAuthorization>>,
}

#[derive(Deserialize)]
pub(super) struct WireAuthorization {
    #[serde(rename = "externalId", default)]
    pub(super) external_id: Option<String>,
}

#[derive(Deserialize)]
pub(super) struct SetsData {
    pub(super) event: Option<SetsEvent>,
}

#[derive(Deserialize)]
pub(super) struct SetsEvent {
    pub(super) sets: Page<WireSet>,
}

#[derive(Deserialize)]
pub(super) struct SetData {
    pub(super) set: Option<WireSet>,
}

#[derive(Deserialize)]
pub(super) struct WireSet {
    #[serde(deserialize_with = "id")]
    pub(super) id: String,
    #[serde(default)]
    pub(super) state: Option<i64>,
    #[serde(default)]
    pub(super) round: Option<i32>,
    #[serde(rename = "fullRoundText", default)]
    pub(super) full_round_text: Option<String>,
    #[serde(rename = "winnerId", default, deserialize_with = "optional_id")]
    pub(super) winner_id: Option<String>,
    #[serde(default)]
    pub(super) slots: Vec<WireSlot>,
    /// The games entered so far: read only before a report.
    #[serde(default)]
    pub(super) games: Option<Vec<WireGame>>,
    #[serde(rename = "phaseGroup", default)]
    pub(super) phase_group: Option<WirePhaseGroup>,
    #[serde(default)]
    pub(super) stream: Option<WireStream>,
}

#[derive(Deserialize)]
pub(super) struct WirePhaseGroup {
    #[serde(default)]
    pub(super) phase: Option<WirePhase>,
}

#[derive(Deserialize)]
pub(super) struct WirePhase {
    #[serde(default, deserialize_with = "optional_id")]
    pub(super) id: Option<String>,
    #[serde(default)]
    pub(super) name: Option<String>,
}

/// The stream a set is assigned to. Its fields are read from the schema;
/// that start.gg fills them for a set given a stream awaits a live check.
#[derive(Deserialize)]
pub(super) struct WireStream {
    #[serde(default, deserialize_with = "optional_id")]
    pub(super) id: Option<String>,
    #[serde(rename = "streamName", default)]
    pub(super) stream_name: Option<String>,
}

#[derive(Deserialize)]
pub(super) struct WireGame {
    #[serde(rename = "orderNum", default)]
    pub(super) order_num: Option<u32>,
    #[serde(rename = "winnerId", default, deserialize_with = "optional_id")]
    pub(super) winner_id: Option<String>,
    /// Anything else the organizer entered for the game, which a report
    /// replaces: its score, stage or characters.
    #[serde(rename = "entrant1Score", default)]
    pub(super) entrant1_score: Option<Value>,
    #[serde(rename = "entrant2Score", default)]
    pub(super) entrant2_score: Option<Value>,
    #[serde(default)]
    pub(super) stage: Option<Value>,
    #[serde(default)]
    pub(super) selections: Option<Value>,
}

impl WireGame {
    /// Whether the game holds anything but its winner: a score other than
    /// zero, a stage, or characters. Zero and null scores are empty, since
    /// start.gg may give them for a game entered with only a winner
    /// (STARTGG_API.md, section 8, item 20). A negative score is kept: start.gg
    /// records a DQ as -1.
    fn has_details(&self) -> bool {
        let score = |value: &Option<Value>| match value {
            None | Some(Value::Null) => false,
            Some(Value::Number(number)) => number.as_f64().is_none_or(|score| score != 0.0),
            Some(_) => true,
        };
        let stage = self.stage.as_ref().is_some_and(has_id);
        let selections = match &self.selections {
            None | Some(Value::Null) => false,
            Some(Value::Array(items)) => items.iter().any(has_id),
            Some(other) => has_id(other),
        };
        score(&self.entrant1_score) || score(&self.entrant2_score) || stage || selections
    }
}

/// Whether a stage or selection start.gg sent is one: not null, and, if an
/// object, with an `id`.
fn has_id(value: &Value) -> bool {
    match value {
        Value::Null => false,
        Value::Object(fields) => fields.get("id").is_some_and(|id| !id.is_null()),
        _ => true,
    }
}

/// A game the organizer entered, as far as a report's check reads it.
pub(super) struct EnteredGame {
    pub(super) order: Option<u32>,
    pub(super) winner: Option<String>,
    /// It holds more than its winner (a score, stage or characters).
    pub(super) details: bool,
}

#[derive(Deserialize)]
pub(super) struct WireSlot {
    #[serde(rename = "slotIndex", default)]
    pub(super) slot_index: Option<u32>,
    #[serde(default)]
    pub(super) entrant: Option<WireEntrantId>,
}

#[derive(Deserialize)]
pub(super) struct WireEntrantId {
    #[serde(deserialize_with = "id")]
    pub(super) id: String,
}

/// A GraphQL answer, or start.gg's flat refusal (rate limit, complexity).
#[derive(Deserialize)]
pub(super) struct Envelope {
    #[serde(default)]
    pub(super) data: Option<Value>,
    #[serde(default)]
    pub(super) errors: Vec<GraphqlError>,
    #[serde(default)]
    pub(super) success: Option<bool>,
    #[serde(default)]
    pub(super) message: Option<String>,
}

#[derive(Deserialize)]
pub(super) struct GraphqlError {
    #[serde(default)]
    pub(super) message: String,
}

/// What a set that is not a preview or a bye says, before its entrants'
/// accounts are attached.
pub(super) struct RawSet {
    pub(super) id: String,
    pub(super) round: i32,
    pub(super) round_label: String,
    pub(super) status: Status,
    pub(super) underway: bool,
    pub(super) winner: Option<String>,
    /// Entrant IDs, slot 0 first.
    pub(super) entrants: Option<[String; 2]>,
    /// Each game entered so far.
    pub(super) games: Vec<EnteredGame>,
    /// Its phase, and the name (or ID) of the stream it is assigned to.
    pub(super) extras: Extras,
}

impl WireSet {
    pub(super) fn into_raw(self) -> Option<RawSet> {
        if !is_set_id(&self.id) {
            return None;
        }
        let mut slots: Vec<(u32, Option<String>)> = self
            .slots
            .into_iter()
            .map(|slot| {
                (
                    slot.slot_index.unwrap_or(u32::MAX),
                    slot.entrant.map(|entrant| entrant.id),
                )
            })
            .collect();
        slots.sort_by_key(|(index, _)| *index);
        let entrants = match slots.as_slice() {
            [(0, Some(one)), (1, Some(two))] => Some([one.clone(), two.clone()]),
            _ => None,
        };
        let state = self.state.unwrap_or(0);
        let status = match (&self.winner_id, state) {
            (Some(_), _) | (None, COMPLETED) => Status::Decided,
            (None, CREATED | STARTED | CALLED) if entrants.is_some() => Status::Open,
            _ => Status::Pending,
        };
        let round = self.round.unwrap_or(0);
        let games = self
            .games
            .unwrap_or_default()
            .into_iter()
            .map(|game| EnteredGame {
                details: game.has_details(),
                order: game.order_num,
                winner: game.winner_id,
            })
            .collect();
        let phase = self
            .phase_group
            .and_then(|group| group.phase)
            .and_then(|phase| Phase::new(phase.name.as_deref(), phase.id));
        let stream = self.stream.and_then(|stream| {
            stream
                .stream_name
                .filter(|name| !name.trim().is_empty())
                .or(stream.id)
        });
        Some(RawSet {
            round_label: self
                .full_round_text
                .filter(|text| !text.is_empty())
                .unwrap_or_else(|| format!("Round {round}")),
            id: self.id,
            round,
            status,
            underway: state == STARTED,
            winner: self.winner_id,
            entrants,
            games,
            extras: Extras { phase, stream },
        })
    }
}

impl WireEntrant {
    /// The accounts behind a one-player entrant: its start.gg user, then
    /// the Discord account that user linked. A team entrant has none.
    pub(super) fn accounts(self, bridge_site: &str) -> (String, Vec<Account>) {
        let user = match <[WireParticipant; 1]>::try_from(self.participants) {
            Ok([participant]) => participant.user,
            Err(_) => None,
        };
        let accounts = user.map_or_else(Vec::new, |user| {
            let discord = user
                .authorizations
                .unwrap_or_default()
                .into_iter()
                .filter_map(|authorization| authorization.external_id)
                .filter(|external| !external.is_empty())
                .map(|user_id| Account::Discord { user_id });
            std::iter::once(Account::Site {
                site: bridge_site.to_owned(),
                id: user.id,
            })
            .chain(discord.take(1))
            .collect()
        });
        (self.id, accounts)
    }
}

/// Whether start.gg's answer to `reportBracketSet` shows set `id` decided
/// for `winner`.
pub(super) fn shows_recorded(answer: Option<&Value>, id: &str, winner: &str) -> bool {
    let Some(sets) = answer
        .and_then(|data| data.get("reportBracketSet"))
        .and_then(Value::as_array)
    else {
        return false;
    };
    sets.iter()
        .filter_map(|set| WireSet::deserialize(set).ok())
        .any(|set| set.id == id && set.winner_id.as_deref() == Some(winner))
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn raw(value: Value) -> Option<RawSet> {
        serde_json::from_value::<WireSet>(value).unwrap().into_raw()
    }

    #[test]
    fn a_report_counts_only_when_the_answer_shows_it() {
        let shows = |answer: Value| shows_recorded(Some(&answer), "65089253", "2401412");
        assert!(shows(
            json!({ "reportBracketSet": [{ "id": 65089253, "state": 3, "winnerId": 2401412 }] })
        ));
        assert!(shows(json!({ "reportBracketSet": [
            { "id": "65089254", "state": 1, "winnerId": null },
            { "id": "65089253", "state": 3, "winnerId": "2401412" },
        ] })));
        assert!(!shows_recorded(None, "65089253", "2401412"));
        for answer in [
            json!({}),
            json!({ "reportBracketSet": null }),
            json!({ "reportBracketSet": [] }),
            json!({ "reportBracketSet": [{ "id": 65089254, "state": 3, "winnerId": 2401412 }] }),
            json!({ "reportBracketSet": [{ "id": 65089253, "state": 2, "winnerId": null }] }),
            json!({ "reportBracketSet": [{ "id": 65089253, "state": 3, "winnerId": 2401411 }] }),
        ] {
            assert!(!shows(answer.clone()), "{answer}");
        }
    }

    #[test]
    fn only_start_gg_naming_the_set_says_it_is_not_found() {
        assert!(is_set_not_found(
            "Set not found for id: 65089253",
            "65089253"
        ));
        assert!(!is_set_not_found(
            "Set not found for id: 650892530",
            "65089253"
        ));
        assert!(!is_set_not_found("Set not found", "65089253"));
        assert!(!is_set_not_found(
            "An unknown error has occurred",
            "65089253"
        ));
        // Partial data is used only when nothing at the top was nulled.
        assert!(is_whole(&json!({ "event": { "entrants": {} } })));
        assert!(!is_whole(&json!({ "event": null })));
        assert!(!is_whole(&json!({})));
    }

    #[test]
    fn ids_are_numbers_or_strings_and_previews_are_left_out() {
        let set = raw(json!({
            "id": 65089253, "state": 1, "round": -2, "fullRoundText": "Losers Round 2",
            "slots": [
                { "slotIndex": 1, "entrant": { "id": "14250090" } },
                { "slotIndex": 0, "entrant": { "id": 14259653 } },
            ],
        }))
        .unwrap();
        assert_eq!(set.id, "65089253");
        assert_eq!(set.entrants, Some(["14259653".into(), "14250090".into()]));
        assert_eq!(
            (set.status, set.round_label.as_str()),
            (Status::Open, "Losers Round 2")
        );
        assert!(raw(json!({ "id": "preview_5093733_1_0", "state": 1, "slots": [] })).is_none());
    }

    #[test]
    fn a_set_is_open_only_with_two_entrants_and_no_winner() {
        let status = |state: i64, winner: Value, slots: Value| {
            raw(json!({ "id": "1", "state": state, "winnerId": winner, "slots": slots }))
                .map(|set| (set.status, set.underway))
        };
        let both = json!([
            { "slotIndex": 0, "entrant": { "id": 1 } },
            { "slotIndex": 1, "entrant": { "id": 2 } },
        ]);
        assert_eq!(
            status(1, Value::Null, both.clone()),
            Some((Status::Open, false))
        );
        assert_eq!(
            status(2, Value::Null, both.clone()),
            Some((Status::Open, true))
        );
        assert_eq!(
            status(6, Value::Null, both.clone()),
            Some((Status::Open, false))
        );
        assert_eq!(
            status(3, Value::Null, both.clone()),
            Some((Status::Decided, false))
        );
        assert_eq!(status(2, json!(2), both), Some((Status::Decided, true)));
        // A bye or a slot still waiting on another set.
        let one = json!([
            { "slotIndex": 0, "entrant": { "id": 1 } },
            { "slotIndex": 1, "entrant": null },
        ]);
        assert_eq!(status(1, Value::Null, one), Some((Status::Pending, false)));
    }

    #[test]
    fn an_entrant_is_found_by_user_then_discord() {
        let entrant = |value: Value| {
            serde_json::from_value::<WireEntrant>(value)
                .unwrap()
                .accounts("startgg")
        };
        let (id, accounts) = entrant(json!({
            "id": 2401411,
            "participants": [{ "user": { "id": 1234567, "authorizations": [{ "externalId": "123456789012345678" }] } }],
        }));
        assert_eq!(id, "2401411");
        assert_eq!(
            accounts,
            [
                Account::Site {
                    site: "startgg".into(),
                    id: "1234567".into()
                },
                Account::Discord {
                    user_id: "123456789012345678".into()
                },
            ]
        );
        // Its name, as the bracket shows it, comes in the same read.
        let named: WireEntrant =
            serde_json::from_value(json!({ "id": 1, "name": "ZZ | Kuro" })).unwrap();
        assert_eq!(named.name.as_deref(), Some("ZZ | Kuro"));
        assert!(ENTRANTS.contains("nodes { id name participants"));
        // No start.gg account behind it, or a team: nobody to find.
        assert!(
            entrant(json!({ "id": 1, "participants": [{ "user": null }] }))
                .1
                .is_empty()
        );
        assert!(
            entrant(json!({ "id": 1, "participants": [{ "user": { "id": 1 } }, { "user": { "id": 2 } }] }))
                .1
                .is_empty()
        );
    }

    #[test]
    fn a_sets_query_asks_for_a_phase_or_a_stream_only_when_used() {
        let asked = |phases, streams| sets_query(phases, streams, false);
        let plain = asked(false, false);
        assert!(!plain.contains("phaseGroup") && !plain.contains("stream"));
        let phases = asked(true, false);
        assert!(phases.contains("phaseGroup { phase { id name } }") && !phases.contains("stream"));
        let streams = asked(false, true);
        assert!(streams.contains("stream { id streamName }") && !streams.contains("phaseGroup"));
        let both = asked(true, true);
        assert!(both.contains("phaseGroup") && both.contains("stream"));
        // Braces and a page stay balanced whichever fields are asked for.
        for query in [plain, phases, streams, both] {
            assert_eq!(query.matches('{').count(), query.matches('}').count());
            assert!(query.contains("$page") && query.contains("slots { slotIndex"));
        }
        // A single set read never asks for either.
        assert!(!SET.contains("phaseGroup") && !SET.contains("stream"));
    }

    #[test]
    fn a_game_holds_more_than_its_winner_only_with_a_nonzero_score_a_stage_or_characters() {
        let game = |extra: Value| {
            let mut game = json!({ "orderNum": 1, "winnerId": 2 });
            for (key, value) in extra.as_object().unwrap() {
                game[key] = value.clone();
            }
            serde_json::from_value::<WireGame>(game)
                .unwrap()
                .has_details()
        };
        // Only its winner, or what start.gg may fill in by default.
        for empty in [
            json!({}),
            json!({ "entrant1Score": null, "entrant2Score": null }),
            json!({ "entrant1Score": 0, "entrant2Score": 0 }),
            json!({ "stage": null, "selections": null }),
            json!({ "stage": { "id": null }, "selections": [] }),
            json!({ "selections": [null, { "id": null }] }),
        ] {
            assert!(!game(empty.clone()), "{empty}");
        }
        for filled in [
            json!({ "entrant1Score": 1 }),
            json!({ "entrant2Score": 3 }),
            json!({ "entrant1Score": -1, "entrant2Score": 0 }),
            json!({ "stage": { "id": 4 } }),
            json!({ "selections": [{ "id": 9 }] }),
        ] {
            assert!(game(filled.clone()), "{filled}");
        }
    }
}
