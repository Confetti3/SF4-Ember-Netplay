//! The response body for a tournament: its settings, entrants, sets and, for
//! round robin, the standings.
use ember_protocol::tournament::{Format, Status};
use rusqlite::Transaction;
use serde_json::json;

use crate::{
    error::Result,
    events,
    routes::matches,
    tournament::{Tournament, entrants, load_bracket, round_robin_table, set_json},
};

pub fn snapshot(tx: &Transaction<'_>, tournament: &Tournament) -> Result<serde_json::Value> {
    let all = entrants(tx, &tournament.id)?;
    let entrants_json: Vec<_> = all
        .iter()
        .map(|entrant| {
            json!({
                "ember_id": entrant.ember_id,
                "participant_id": entrant.participant_id,
                "state": if entrant.withdrawn { "withdrawn" } else { "registered" },
                "seed": entrant.seed.map(|seed| seed + 1),
                "placement": entrant.placement,
            })
        })
        .collect();
    let started = matches!(tournament.state.as_str(), "running" | "completed")
        || (tournament.state == "cancelled" && all.iter().any(|entrant| entrant.seed.is_some()));
    let (sets, standings) = if started {
        let bracket = load_bracket(tx, tournament)?;
        let mut sets = Vec::new();
        for (node, state) in bracket.states.iter().enumerate() {
            let mut set = set_json(&bracket, node);
            set["status"] = state.status.as_str().into();
            set["players"] = json!(state.slots.map(|slot| bracket.slot(slot, false)));
            set["winner_slot"] = state.winner.into();
            set["match_id"] = bracket.matches[node].clone().into();
            set["games_to_win"] = if bracket.plan[node].finals {
                tournament.finals_games_to_win
            } else {
                tournament.games_to_win
            }
            .into();
            if let Some(match_id) = &bracket.matches[node]
                && matches!(state.status, Status::Playing | Status::Played)
            {
                set["wins"] = json!(matches::scores(tx, match_id)?.0);
            }
            sets.push(set);
        }
        let standings = if tournament.format == Format::RoundRobin {
            round_robin_table(tx, &bracket)?
                .into_iter()
                .map(|row| {
                    let mut player = bracket.seeds[row.seed].player();
                    player["sets_won"] = row.sets_won.into();
                    player["sets_lost"] = row.sets_lost.into();
                    player["games_won"] = row.games_won.into();
                    player["games_lost"] = row.games_lost.into();
                    player
                })
                .collect()
        } else {
            Vec::new()
        };
        (sets, standings)
    } else {
        (Vec::new(), Vec::new())
    };
    Ok(json!({
        "tournament_id": tournament.id,
        "external_tournament_id": tournament.external_tournament_id,
        "state": tournament.state,
        "revision": tournament.revision.to_string(),
        "format": tournament.format,
        "games_to_win": tournament.games_to_win,
        "finals_games_to_win": tournament.finals_games_to_win,
        "grand_final_reset": tournament.grand_final_reset,
        "required_build_id": tournament.required_build_id,
        "metadata": tournament.metadata,
        "entrants": entrants_json,
        "sets": sets,
        "standings": standings,
        "event_cursor": events::head(tx)?.to_string(),
    }))
}
