//! Whether a committed checkpoint seats a probe's pair.

use super::*;

pub(super) fn probe_pair_identities_valid(
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    !(source_incarnation == 0
        || target_incarnation == 0
        || source_incarnation == target_incarnation
        || source_endpoint.is_empty()
        || target_endpoint.is_empty()
        || source_endpoint == target_endpoint
        || pair_revision == 0)
}

/// Validate the native room projection which a committed probe is allowed to
/// use.  This is deliberately a small JSON boundary rather than a native
/// model dependency: the helper must reject malformed or stale projections
/// before opening a GAME_ALPN stream.
pub fn committed_probe_pair_bound(
    checkpoint: &str,
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    if checkpoint.is_empty()
        || !probe_pair_identities_valid(
            source_incarnation,
            target_incarnation,
            source_endpoint,
            target_endpoint,
            pair_revision,
        )
    {
        return false;
    }
    let Some(envelope) = envelope_view(checkpoint) else {
        return false;
    };
    probe_pair_bound_in(
        &envelope,
        source_incarnation,
        target_incarnation,
        source_endpoint,
        target_endpoint,
        pair_revision,
    )
}

/// `committed_probe_pair_bound` on a parsed envelope, or its view.
pub(super) fn probe_pair_bound_in(
    envelope: &Value,
    source_incarnation: u64,
    target_incarnation: u64,
    source_endpoint: &str,
    target_endpoint: &str,
    pair_revision: u64,
) -> bool {
    // Raft stores the complete native SessionProposal JSON.  The portable
    // RecoveryCheckpoint used for pair binding is its nested `checkpoint`
    // member; accepting a flat object here would allow tests or a malformed
    // peer to bypass the proposal identity boundary.
    let Some(root) = envelope.get("checkpoint") else {
        return false;
    };
    let Some(stable_members) = root.get("members").and_then(Value::as_array) else {
        return false;
    };
    let member_for = |incarnation: u64, endpoint: &str| {
        stable_members.iter().find_map(|row| {
            let row_incarnation = row
                .get("incarnation")
                .and_then(Value::as_u64)
                .or_else(|| row.get("data")?.get("incarnation")?.as_u64())?;
            let row_endpoint = row
                .get("data")
                .and_then(|data| {
                    data.get("authenticatedEndpoint")
                        .or_else(|| data.get("authenticated_endpoint"))
                })
                .and_then(Value::as_str)?;
            if row_incarnation == incarnation && row_endpoint == endpoint {
                row.get("member").and_then(Value::as_u64)
            } else {
                None
            }
        })
    };
    let Some(source_member) = member_for(source_incarnation, source_endpoint) else {
        return false;
    };
    let Some(target_member) = member_for(target_incarnation, target_endpoint) else {
        return false;
    };
    if source_member == target_member {
        return false;
    }
    let Some(room_checkpoint) = root.get("room") else {
        return false;
    };
    let Some(room) = room_checkpoint.get("snapshot") else {
        return false;
    };
    let Some(room_members) = room.get("members").and_then(Value::as_array) else {
        return false;
    };
    let member_present = |member: u64| {
        room_members
            .iter()
            .any(|row| row.get("id").and_then(Value::as_u64) == Some(member))
    };
    // Probing measures the authorized seated pair before Ready publishes
    // native fighter selections. Selection is not a network permission.
    if !member_present(source_member) || !member_present(target_member) {
        return false;
    }
    room.get("tables")
        .and_then(Value::as_array)
        .is_some_and(|tables| {
            tables.iter().any(|table| {
                table.get("revision").and_then(Value::as_u64) == Some(pair_revision)
                    && table.get("p1").and_then(Value::as_u64).is_some_and(|p1| {
                        table.get("p2").and_then(Value::as_u64).is_some_and(|p2| {
                            (p1 == source_member && p2 == target_member)
                                || (p1 == target_member && p2 == source_member)
                        })
                    })
            })
        })
}
