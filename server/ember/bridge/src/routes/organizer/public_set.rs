//! What anyone may read of a desk set. The desk's own view of a set
//! (`sets::desk_sets`) carries the connector's JSON with the bridge's
//! additions, including site and Ember IDs; this is the type a public view
//! is built from, so a field is published only when it is named here.
use super::*;

/// A set as `GET /v1/public/events/{id}` serves it. A field the connector or
/// the bridge may leave out is absent when it is absent from the desk's view;
/// a field present there as null stays null. Add a field here to publish it:
/// the site's entry IDs (so not the winner's: `status` says a set is
/// decided), the Ember match and the entrants' Discord accounts are not
/// fields.
#[derive(Serialize)]
pub(super) struct PublicSet {
    id: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    round: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    round_label: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    phase: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    status: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    held: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    progress: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    called_at: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    call: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    dms: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    score: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    draws: Option<Value>,
    players: Value,
    side: Value,
    desk_state: Value,
}

impl PublicSet {
    /// Copies the allowed fields out of a set of the desk's view.
    pub(super) fn of(set: &Value) -> Self {
        let field = |name: &str| set.get(name).cloned();
        Self {
            id: set["id"].clone(),
            round: field("round"),
            round_label: field("round_label"),
            phase: field("phase"),
            status: field("status"),
            held: field("held"),
            progress: field("progress"),
            called_at: field("called_at"),
            call: field("call"),
            dms: field("dms"),
            score: field("score"),
            draws: field("draws"),
            players: set["players"].clone(),
            side: set["side"].clone(),
            desk_state: set["desk_state"].clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_public_set_is_what_the_type_names_and_nothing_the_connector_adds() {
        let desk_view = json!({
            "id": "s-1",
            "round": 2,
            "round_label": "Round 2",
            "phase": null,
            "status": "decided",
            "held": false,
            "progress": "live",
            "called_at": 1700000000,
            "call": { "at": 1700000000, "posted": "delivered" },
            "dms": ["delivered", "no_discord"],
            "score": [1, 0],
            "draws": 0,
            "players": ["Player 1", "Player 2"],
            "side": "winners",
            "desk_state": "playing",
            // Private: the site's and Ember's IDs (the winner's among them), and the entrants' Discord.
            "winner": "p-1",
            "match_id": "emt_1",
            "entries": ["p-1", "p-2"],
            "discord": ["player1#0", null],
            "ember_ids": ["emb_1", "emb_2"],
            "revision": "3",
            "games": [{ "attempt_id": "att_1", "outcome": "win", "state": "accepted" }],
            // A field a connector adds later is private until it is named.
            "seed_notes": "Player 1 is the host",
        });
        assert_eq!(
            serde_json::to_value(PublicSet::of(&desk_view)).unwrap(),
            json!({
                "id": "s-1",
                "round": 2,
                "round_label": "Round 2",
                "phase": null,
                "status": "decided",
                "held": false,
                "progress": "live",
                "called_at": 1700000000,
                "call": { "at": 1700000000, "posted": "delivered" },
                "dms": ["delivered", "no_discord"],
                "score": [1, 0],
                "draws": 0,
                "players": ["Player 1", "Player 2"],
                "side": "winners",
                "desk_state": "playing",
            })
        );
    }

    #[test]
    fn a_field_absent_from_the_desk_view_stays_absent() {
        let desk_view = json!({
            "id": "s-2",
            "players": [null, null],
            "side": "losers",
            "desk_state": "waiting",
            "match_id": "emt_2",
        });
        assert_eq!(
            serde_json::to_value(PublicSet::of(&desk_view)).unwrap(),
            json!({
                "id": "s-2",
                "players": [null, null],
                "side": "losers",
                "desk_state": "waiting",
            })
        );
    }
}
