//! The typed event data against the `json!` literals the bridge wrote before
//! it used them, copied from the bridge's writers with the same inputs. Field
//! names and null-versus-absent must match exactly; events are stored
//! JCS-canonical, so the canonical bytes are compared as well.
use std::collections::BTreeMap;

use ember_protocol::{
    EmberId,
    encoding::Counter,
    event::{
        EventParticipant, EventPlacement, EventPlayer, EventScore, EventSeed, EventStreak,
        GameConfirmedData, LobbyCreatedData, LobbySetCompletedData, MatchCreatedData,
        RoomEventData, ScoreChangedData, TournamentCompletedData, TournamentCreatedData,
        TournamentMatchCompletedData, TournamentStartedData,
    },
    json,
    lobby::Rotation,
    matches::{MatchState, Resolution, Score},
    report::Outcome,
    rooms::ConnectionRoom,
    tournament::{Format, Side},
};
use serde::{Serialize, de::DeserializeOwned};
use serde_json::{Value, json};

const A: &str = "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";
const B: &str = "emb1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

fn id(text: &str) -> EmberId {
    EmberId::parse(text).unwrap()
}

/// The typed data serializes to exactly the literal, and reads back from it,
/// also with a key it does not know.
fn same<T>(typed: &T, literal: &Value)
where
    T: Serialize + DeserializeOwned + PartialEq + std::fmt::Debug,
{
    let value = serde_json::to_value(typed).unwrap();
    assert_eq!(&value, literal);
    assert_eq!(
        json::canonical(typed).unwrap(),
        json::canonical(literal).unwrap()
    );
    let read: T = serde_json::from_value(literal.clone()).unwrap();
    assert_eq!(&read, typed);
    let mut extended = literal.clone();
    extended["added_by_a_later_bridge"] = json!({ "nested": [1, null] });
    let read: T = serde_json::from_value(extended).unwrap();
    assert_eq!(&read, typed);
}

/// `tournament_event` (bridge tournament/mod.rs) adds these to every
/// tournament event's data.
fn tournament_event(mut data: Value, tournament_id: &str, revision: u64) -> Value {
    data["tournament_id"] = tournament_id.to_owned().into();
    data["tournament_revision"] = revision.to_string().into();
    data
}

fn metadata() -> BTreeMap<String, String> {
    [
        ("round_label".to_owned(), "Winners final".to_owned()),
        ("title".to_owned(), "Friday night".to_owned()),
    ]
    .into()
}

fn scores() -> Vec<Score> {
    vec![
        Score {
            ember_id: id(A),
            wins: 2,
        },
        Score {
            ember_id: id(B),
            wins: 1,
        },
    ]
}

fn event_scores() -> Vec<EventScore> {
    scores().iter().map(EventScore::from).collect()
}

/// `ledger::Participant` as the bridge serializes it.
#[derive(Serialize)]
struct Participant {
    slot: u8,
    participant_id: String,
    ember_id: EmberId,
}

#[test]
fn match_created() {
    let id_ = "emt_1".to_owned();
    let external_match_id = "ext-1";
    let games_to_win: u8 = 2;
    let roster = vec![
        Participant {
            slot: 0,
            participant_id: "par_a".into(),
            ember_id: id(A),
        },
        Participant {
            slot: 1,
            participant_id: "par_b".into(),
            ember_id: id(B),
        },
    ];
    let new_metadata = metadata();
    // bridge routes/matches.rs, `create`.
    let literal = json!({
        "match_id": id_,
        "external_match_id": external_match_id,
        "match_revision": "1",
        "assignment_generation": "1",
        "state": MatchState::AwaitingPlayers,
        "games_to_win": games_to_win,
        "participants": roster,
        "metadata": &new_metadata,
    });
    let typed = MatchCreatedData {
        match_id: id_.clone(),
        external_match_id: external_match_id.into(),
        match_revision: Counter(1),
        assignment_generation: Counter(1),
        state: Some(MatchState::AwaitingPlayers),
        games_to_win,
        participants: roster
            .iter()
            .map(|p| EventParticipant {
                slot: p.slot,
                participant_id: p.participant_id.clone(),
                ember_id: p.ember_id.clone(),
            })
            .collect(),
        metadata: new_metadata.clone(),
    };
    same(&typed, &literal);
    // No metadata is an empty object, as before.
    let literal = json!({
        "match_id": id_, "external_match_id": external_match_id, "match_revision": "1",
        "assignment_generation": "1", "state": MatchState::AwaitingPlayers,
        "games_to_win": games_to_win, "participants": roster, "metadata": BTreeMap::<String, String>::new(),
    });
    same(
        &MatchCreatedData {
            metadata: BTreeMap::new(),
            ..typed
        },
        &literal,
    );
}

#[test]
fn score_changed() {
    let found_id = "emt_1".to_owned();
    let revision: u64 = 7;
    let revision_text = revision.to_string();
    let n: u8 = 3;
    let score_rows = scores();
    // bridge routes/ledger.rs, `settle`.
    let literal = json!({ "match_id": found_id, "match_revision": revision_text, "games_to_win": n, "scores": score_rows });
    same(
        &ScoreChangedData {
            match_id: found_id.clone(),
            match_revision: Counter(revision),
            games_to_win: n,
            scores: event_scores(),
        },
        &literal,
    );
}

#[test]
fn game_confirmed_by_an_organizer_and_by_two_reports() {
    let found_id = "emt_1".to_owned();
    let revision_text = 4u64.to_string();
    for (outcome, winner) in [("p1_win", Some(id(A))), ("draw", None)] {
        let attempt_id = "ega_1".to_owned();
        let seq: u64 = 2;
        let adjudication_id = "adj_1".to_owned();
        // bridge routes/matches.rs, `adjudicate`.
        let mut literal = json!({
            "match_id": found_id,
            "attempt_id": attempt_id,
            "seq": seq,
            "outcome": outcome,
            "winner_id": winner,
            "resolution": Resolution::OrganizerAdjudication,
            "adjudication_id": adjudication_id,
        });
        let mut typed = GameConfirmedData {
            match_id: found_id.clone(),
            match_revision: None,
            attempt_id: attempt_id.clone(),
            seq,
            outcome: outcome.into(),
            winner_id: winner.clone(),
            resolution: Some(Resolution::OrganizerAdjudication),
            adjudication_id: Some(adjudication_id.clone()),
            report_ids: None,
        };
        same(&typed, &literal);
        let expected = if outcome == "draw" {
            Outcome::Draw
        } else {
            Outcome::P1Win
        };
        assert_eq!(typed.result(), Some(expected));
        // `settle` adds the revision before it emits.
        literal["match_revision"] = revision_text.clone().into();
        typed.match_revision = Some(Counter(4));
        same(&typed, &literal);

        let attempt_id = "ega_2";
        let report_ids = vec!["rpt_1".to_owned(), "rpt_2".to_owned()];
        // bridge routes/reports.rs, `decide`.
        let mut literal = json!({
            "match_id": found_id,
            "attempt_id": attempt_id,
            "seq": seq,
            "outcome": outcome,
            "winner_id": winner,
            "resolution": ember_protocol::matches::Resolution::PlayerAgreement,
            "report_ids": report_ids,
        });
        let mut typed = GameConfirmedData {
            match_id: found_id.clone(),
            match_revision: None,
            attempt_id: attempt_id.into(),
            seq,
            outcome: outcome.into(),
            winner_id: winner.clone(),
            resolution: Some(Resolution::PlayerAgreement),
            adjudication_id: None,
            report_ids: Some(report_ids.clone()),
        };
        same(&typed, &literal);
        literal["match_revision"] = revision_text.clone().into();
        typed.match_revision = Some(Counter(4));
        same(&typed, &literal);
    }
}

#[test]
fn tournament_created() {
    let external_tournament_id = "ext-t".to_owned();
    for (format, finals, reset) in [
        (Format::DoubleElimination, 3u8, true),
        (Format::RoundRobin, 2u8, false),
    ] {
        let games_to_win: u8 = 2;
        let command_metadata = metadata();
        // bridge tournament/lifecycle.rs, `create`.
        let literal = json!({
            "external_tournament_id": external_tournament_id,
            "format": format,
            "games_to_win": games_to_win,
            "finals_games_to_win": finals,
            "grand_final_reset": reset,
            "metadata": command_metadata,
        });
        let mut typed = TournamentCreatedData {
            tournament_id: None,
            tournament_revision: None,
            external_tournament_id: external_tournament_id.clone(),
            format: Some(format),
            games_to_win,
            finals_games_to_win: Some(finals),
            grand_final_reset: reset,
            metadata: command_metadata.clone(),
        };
        same(&typed, &literal);
        typed.tournament_id = Some("etn_1".into());
        typed.tournament_revision = Some(Counter(1));
        same(&typed, &tournament_event(literal, "etn_1", 1));
    }
}

#[test]
fn tournament_started() {
    let format = Format::SingleElimination;
    let order = [id(A), id(B)];
    // bridge tournament/lifecycle.rs, `start`, with `Entrant::public`.
    let seeds: Vec<_> = order
        .iter()
        .enumerate()
        .map(|(seed, ember_id)| {
            let mut player = json!({ "ember_id": ember_id });
            player["seed"] = (seed + 1).into();
            player
        })
        .collect();
    let sets: usize = 1;
    let literal = json!({ "format": format, "entrants": seeds, "sets": sets });
    let mut typed = TournamentStartedData {
        tournament_id: None,
        tournament_revision: None,
        format: Some(format),
        entrants: order
            .iter()
            .enumerate()
            .map(|(seed, ember_id)| EventSeed {
                ember_id: ember_id.clone(),
                seed: seed + 1,
            })
            .collect(),
        sets,
    };
    same(&typed, &literal);
    typed.tournament_id = Some("etn_1".into());
    typed.tournament_revision = Some(Counter(3));
    same(&typed, &tournament_event(literal, "etn_1", 3));
}

/// bridge tournament/state.rs, `set_json`.
fn set_json(node: usize, side: Side, round: u32, label: &str) -> Value {
    json!({
        "node": node,
        "side": side.as_str(),
        "round": round,
        "label": label,
    })
}

fn public(ember_id: &EmberId) -> Value {
    json!({ "ember_id": ember_id })
}

#[test]
fn tournament_match_completed_by_walkover() {
    let a = id(A);
    let b = id(B);
    for (loser, eliminated, winner_next) in [
        (Some(&b), Some(&b), Some("Winners final")),
        (None, None, None),
        (Some(&b), None, Some("Grand final")),
    ] {
        let winner = Some(&a);
        // bridge tournament/progress.rs, `progress`.
        let mut data = set_json(2, Side::Losers, 1, "Losers round 1");
        data["walkover"] = true.into();
        data["winner"] = winner.map(public).into();
        data["loser"] = loser.map(public).into();
        data["eliminated"] = json!(eliminated);
        data["winner_next"] = winner_next.into();
        let mut typed = TournamentMatchCompletedData {
            tournament_id: None,
            tournament_revision: None,
            node: 2,
            side: Some(Side::Losers),
            round: 1,
            label: Some("Losers round 1".into()),
            match_id: None,
            walkover: true,
            winner: winner.map(|id| EventPlayer {
                ember_id: id.clone(),
            }),
            loser: loser.map(|id| EventPlayer {
                ember_id: id.clone(),
            }),
            scores: None,
            eliminated: eliminated.cloned(),
            winner_next: winner_next.map(str::to_owned),
            loser_next: None,
        };
        same(&typed, &data);
        assert!(data.get("loser_next").is_none() && data.get("match_id").is_none());
        typed.tournament_id = Some("etn_1".into());
        typed.tournament_revision = Some(Counter(9));
        same(&typed, &tournament_event(data, "etn_1", 9));
    }
}

#[test]
fn tournament_match_completed_by_a_played_set() {
    let a = id(A);
    let b = id(B);
    for (eliminated, winner_next, loser_next) in [
        (None, Some("Winners final"), Some("Losers round 2")),
        (
            Some(&b),
            Some("Grand final"),
            Some("ignored when eliminated"),
        ),
        (Some(&b), None, None),
        (None, None, None),
    ] {
        let match_id = "emt_9";
        let scores = scores();
        // bridge tournament/correction.rs, `on_match_completed`.
        let mut data = set_json(0, Side::Winners, 1, "Winners round 1");
        data["match_id"] = match_id.into();
        data["walkover"] = false.into();
        data["winner"] = Some(&a).map(public).into();
        data["loser"] = Some(&b).map(public).into();
        data["scores"] = json!(scores);
        data["eliminated"] = json!(eliminated);
        data["winner_next"] = winner_next.into();
        data["loser_next"] = if eliminated.is_some() {
            None
        } else {
            loser_next
        }
        .into();
        let mut typed = TournamentMatchCompletedData {
            tournament_id: None,
            tournament_revision: None,
            node: 0,
            side: Some(Side::Winners),
            round: 1,
            label: Some("Winners round 1".into()),
            match_id: Some(match_id.into()),
            walkover: false,
            winner: Some(EventPlayer {
                ember_id: a.clone(),
            }),
            loser: Some(EventPlayer {
                ember_id: b.clone(),
            }),
            scores: Some(event_scores()),
            eliminated: eliminated.cloned(),
            winner_next: winner_next.map(str::to_owned),
            loser_next: Some(if eliminated.is_some() {
                None
            } else {
                loser_next.map(str::to_owned)
            }),
        };
        same(&typed, &data);
        // `loser_next` is present, as `null` when there is no next set.
        assert!(data.get("loser_next").is_some());
        typed.tournament_id = Some("etn_1".into());
        typed.tournament_revision = Some(Counter(10));
        same(&typed, &tournament_event(data, "etn_1", 10));
    }
}

#[test]
fn tournament_completed() {
    let format = Format::DoubleElimination;
    let seeds = [id(A), id(B)];
    let places = [1u32, 2u32];
    // bridge tournament/progress.rs, `finish`, with `Entrant::public`.
    let placements: Vec<_> = (0..2)
        .map(|seed| {
            let mut player = json!({ "ember_id": seeds[seed] });
            player["placement"] = places[seed].into();
            player
        })
        .collect();
    let literal = json!({ "format": format, "placements": placements });
    let mut typed = TournamentCompletedData {
        tournament_id: None,
        tournament_revision: None,
        format: Some(format),
        placements: (0..2)
            .map(|seed| EventPlacement {
                ember_id: seeds[seed].clone(),
                placement: places[seed],
            })
            .collect(),
    };
    same(&typed, &literal);
    typed.tournament_id = Some("etn_1".into());
    typed.tournament_revision = Some(Counter(12));
    same(&typed, &tournament_event(literal, "etn_1", 12));
}

#[test]
fn lobby_created() {
    for rotation in [Rotation::WinnerStays, Rotation::LoserStays] {
        let id_ = "elb_1".to_owned();
        let external_lobby_id = "ext-l".to_owned();
        let games_to_win: u8 = 2;
        let command_metadata = metadata();
        // bridge routes/lobbies.rs, `create`.
        let literal = json!({
            "lobby_id": id_,
            "external_lobby_id": external_lobby_id,
            "lobby_revision": "1",
            "games_to_win": games_to_win,
            "rotation": rotation,
            "metadata": command_metadata,
        });
        same(
            &LobbyCreatedData {
                lobby_id: id_.clone(),
                external_lobby_id: external_lobby_id.clone(),
                lobby_revision: Counter(1),
                games_to_win,
                rotation,
                metadata: command_metadata.clone(),
            },
            &literal,
        );
    }
}

#[test]
fn lobby_set_completed() {
    let a = id(A);
    let b = id(B);
    for (streak_holder, dropped, waiting) in [
        (Some(A.to_owned()), vec![], vec![&b]),
        (None, vec![b.clone()], vec![]),
    ] {
        let after_id = "elb_1".to_owned();
        let revision: u64 = 5;
        let match_id = "emt_3";
        let sets_started: u64 = 4;
        let games_to_win: u8 = 2;
        let rotation = Rotation::WinnerStays;
        let streak: u64 = 3;
        let scores = scores();
        // bridge routes/lobbies.rs, `on_set_completed`; `ids(&waiting)` is a
        // `Vec<&EmberId>`.
        let literal = json!({
            "lobby_id": after_id,
            "lobby_revision": revision.to_string(),
            "match_id": match_id,
            "set": sets_started,
            "games_to_win": games_to_win,
            "rotation": rotation,
            "winner_id": a,
            "loser_id": b,
            "scores": scores,
            "streak": streak_holder.as_ref().map(|holder| json!({ "ember_id": holder, "sets": streak })),
            "removed": dropped,
            "queue": waiting,
        });
        same(
            &LobbySetCompletedData {
                lobby_id: after_id.clone(),
                lobby_revision: Counter(revision),
                match_id: match_id.into(),
                set: sets_started,
                games_to_win,
                rotation: Some(rotation),
                winner_id: a.clone(),
                loser_id: Some(b.clone()),
                scores: event_scores(),
                streak: streak_holder.as_ref().map(|holder| EventStreak {
                    ember_id: holder.clone(),
                    sets: streak,
                }),
                removed: dropped.clone(),
                queue: waiting.iter().map(|&id| id.clone()).collect(),
            },
            &literal,
        );
    }
}

#[test]
fn room_events() {
    let room: ConnectionRoom = serde_json::from_value(json!({
        "room": {
            "room_id": "0123456789abcdef0123456789abcdef",
            "name": "Fight Night",
            "build_id": "test-build",
            "members": 1,
            "capacity": 8,
            "tables_playing": 0,
            "region": "use1",
            "created_at": 1_800_000_000u64,
        },
        "state": "open",
        "creator_ember_id": A,
        "join_url": "https://embernetplay.link/r#brg_1/0123456789abcdef0123456789abcdef",
    }))
    .unwrap();
    for reason in [None, Some("ended")] {
        // bridge routes/rooms/mod.rs, `announce`.
        let mut data = serde_json::to_value(&room).unwrap();
        if let (Some(reason), Value::Object(fields)) = (reason, &mut data) {
            fields.insert("reason".into(), reason.into());
        }
        same(
            &RoomEventData {
                room: room.clone(),
                reason: reason.map(str::to_owned),
            },
            &data,
        );
    }
}

#[test]
fn reading_tells_null_from_absent() {
    let base = json!({ "winner": { "ember_id": A }, "walkover": true });
    let absent: TournamentMatchCompletedData = serde_json::from_value(base.clone()).unwrap();
    assert_eq!(absent.loser_next, None);
    let mut null = base.clone();
    null["loser_next"] = Value::Null;
    let null: TournamentMatchCompletedData = serde_json::from_value(null).unwrap();
    assert_eq!(null.loser_next, Some(None));
    let mut set = base;
    set["loser_next"] = "Losers final".into();
    let set: TournamentMatchCompletedData = serde_json::from_value(set).unwrap();
    assert_eq!(set.loser_next, Some(Some("Losers final".into())));
    // A key an announcement needs is required.
    assert!(
        serde_json::from_value::<TournamentCompletedData>(json!({ "format": "round_robin" }))
            .is_err()
    );
    assert!(serde_json::from_value::<LobbyCreatedData>(json!({ "lobby_id": "elb_1" })).is_err());
    // Defaults where the announcement has one.
    let created: LobbyCreatedData =
        serde_json::from_value(json!({ "rotation": "winner_stays" })).unwrap();
    assert_eq!(created.games_to_win, 1);
}
