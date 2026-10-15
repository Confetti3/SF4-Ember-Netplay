//! What a room host says about its room beyond the counts: the room's name and
//! capacity as its moderator last set them, and the listing details. The
//! supervisor passes the host's `details` object on untouched (an opaque JSON
//! value under a size limit), so each field is judged here and one that does
//! not hold is dropped by itself; a bad value never costs the room its poll.
use ember_protocol::rooms::{
    FIGHTER_LIMIT, MAX_CAPACITY, MAX_ROTATION, MIN_CAPACITY, NO_FIGHTER, check_host_name,
    check_room_name, set_format_valid,
};
use serde_json::Value;

/// The usable part of one report's `details`. `None` is "not said, or not
/// valid": it keeps what the row had, except where noted on the field.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Details {
    pub name: Option<String>,
    pub capacity: Option<u8>,
    pub locked: Option<bool>,
    /// Unlike the others, `None` here clears the stored name: a room whose
    /// moderator left has no moderator to show.
    pub host_name: Option<String>,
    /// One byte per member, moderator first. `None` clears the stored list.
    pub fighters: Option<Vec<u8>>,
    /// The member count to store: the report's, bounded by `MAX_CAPACITY` and
    /// by the capacity the room ends up with. `fighters` was judged against it.
    pub members: u8,
    pub set_format: Option<u8>,
    pub rotation: Option<u8>,
}

fn small(value: &Value, limit: u64) -> Option<u8> {
    value
        .as_u64()
        .filter(|number| *number <= limit)
        .and_then(|number| u8::try_from(number).ok())
}

impl Details {
    /// What a report that says nothing usable about the room leaves: the
    /// member count the room holds, and no field.
    pub fn absent(reported: u8, held: u8) -> Self {
        Self {
            members: reported.min(MAX_CAPACITY).min(held),
            ..Self::default()
        }
    }

    /// Reads `value` leniently. `reported` is the member count the same report
    /// gave and `held` the capacity the room has now. A capacity below the
    /// reported members cannot be right and is not taken (the room keeps
    /// `held`); the member count to store is then worked out once, from the
    /// capacity the room ends up with, and the fighters must fit it.
    pub fn from_value(value: &Value, reported: u8, held: u8) -> Self {
        let reported = reported.min(MAX_CAPACITY);
        let name = value
            .get("name")
            .and_then(Value::as_str)
            .filter(|name| check_room_name(name).is_ok())
            .map(str::to_owned);
        let capacity = value
            .get("capacity")
            .and_then(|capacity| small(capacity, MAX_CAPACITY.into()))
            .filter(|capacity| *capacity >= MIN_CAPACITY && *capacity >= reported);
        let members = reported.min(capacity.unwrap_or(held));
        let host_name = value
            .get("host_name")
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|name| check_host_name(name).is_ok())
            .map(str::to_owned);
        let fighters = value
            .get("fighters")
            .and_then(Value::as_array)
            .filter(|list| list.len() <= usize::from(members))
            .and_then(|list| {
                list.iter()
                    .map(|fighter| {
                        small(fighter, NO_FIGHTER.into())
                            .filter(|id| *id < FIGHTER_LIMIT || *id == NO_FIGHTER)
                    })
                    .collect::<Option<Vec<u8>>>()
            });
        Self {
            name,
            capacity,
            locked: value.get("locked").and_then(Value::as_bool),
            host_name,
            fighters,
            members,
            set_format: value
                .get("set_format")
                .and_then(Value::as_u64)
                .and_then(|format| u8::try_from(format).ok())
                .filter(|format| set_format_valid(*format)),
            rotation: value
                .get("rotation")
                .and_then(|rotation| small(rotation, MAX_ROTATION.into())),
        }
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    #[test]
    fn a_complete_report_is_read_whole() {
        let value = json!({ "name": "Late night", "capacity": 6, "locked": true,
            "host_name": "  Kate ", "fighters": [3, 255, 0], "set_format": 3, "rotation": 2,
            "unknown": [1] });
        assert_eq!(
            Details::from_value(&value, 3, 4),
            Details {
                name: Some("Late night".into()),
                capacity: Some(6),
                locked: Some(true),
                host_name: Some("Kate".into()),
                fighters: Some(vec![3, 255, 0]),
                members: 3,
                set_format: Some(3),
                rotation: Some(2),
            }
        );
    }

    #[test]
    fn every_set_length_is_read_and_longer_ones_are_not() {
        let format =
            |value: Value| Details::from_value(&json!({ "set_format": value }), 2, 4).set_format;
        for length in 0..=ember_protocol::matches::MAX_GAMES_TO_WIN {
            assert_eq!(format(json!(length)), Some(length));
        }
        for refused in [json!(11), json!(255), json!(256), json!(-1), json!(2.5)] {
            assert_eq!(format(refused.clone()), None, "{refused}");
        }
    }

    #[test]
    fn each_field_is_judged_alone() {
        let value = json!({ "name": "two\nlines", "capacity": 1, "locked": "yes",
            "host_name": "x".repeat(33), "fighters": [64], "set_format": 11, "rotation": 3 });
        assert_eq!(Details::from_value(&value, 0, 4), Details::default());
        let mixed = json!({ "name": "Fine", "capacity": 300, "locked": false, "host_name": "",
            "fighters": [1, 2, 3], "set_format": 5, "rotation": 0 });
        let read = Details::from_value(&mixed, 2, 4);
        assert_eq!(
            (read.name.as_deref(), read.capacity, read.locked),
            (Some("Fine"), None, Some(false))
        );
        // More fighters than members, and no host name that qualifies.
        assert_eq!((read.fighters, read.host_name), (None, None));
        assert_eq!((read.set_format, read.rotation), (Some(5), Some(0)));
    }

    #[test]
    fn a_capacity_below_the_members_is_refused() {
        let value = json!({ "capacity": 4 });
        assert_eq!(Details::from_value(&value, 4, 2).capacity, Some(4));
        assert_eq!(Details::from_value(&value, 5, 2).capacity, None);
        assert_eq!(
            Details::from_value(&json!({ "capacity": 17 }), 0, 4).capacity,
            None
        );
        assert_eq!(
            Details::from_value(&json!({ "capacity": -4 }), 0, 4).capacity,
            None
        );
        assert_eq!(
            Details::from_value(&json!({ "capacity": 4.5 }), 0, 4).capacity,
            None
        );
    }

    #[test]
    fn the_stored_members_and_the_fighters_follow_the_capacity_the_room_ends_with() {
        // A room growing from 2 to 8 with seven members keeps all seven.
        let grown = json!({ "capacity": 8, "fighters": [0, 1, 2, 3, 4, 5, 6] });
        let read = Details::from_value(&grown, 7, 2);
        assert_eq!((read.capacity, read.members), (Some(8), 7));
        assert_eq!(read.fighters, Some(vec![0, 1, 2, 3, 4, 5, 6]));
        // A capacity below the reported members is refused; the old one holds
        // the members and the fighters to it.
        let below = json!({ "capacity": 4, "fighters": [0, 1, 2, 3] });
        let read = Details::from_value(&below, 7, 2);
        assert_eq!(
            (read.capacity, read.members, read.fighters),
            (None, 2, None)
        );
        // No capacity said: the held one bounds the members.
        let read = Details::from_value(&json!({ "fighters": [1, 2] }), 7, 2);
        assert_eq!((read.members, read.fighters), (2, Some(vec![1, 2])));
        assert_eq!(Details::absent(7, 2).members, 2);
        assert_eq!(Details::absent(3, 8).members, 3);
        assert_eq!(Details::absent(40, 200).members, MAX_CAPACITY);
    }

    #[test]
    fn a_value_that_is_not_an_object_reads_as_nothing() {
        for value in [json!(null), json!("x"), json!([1]), json!(7)] {
            assert_eq!(Details::from_value(&value, 2, 4), Details::absent(2, 4));
        }
    }
}
