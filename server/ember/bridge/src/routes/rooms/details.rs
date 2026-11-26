//! What a room host says about its room beyond the counts: the room's name and
//! capacity as its moderator last set them, and the listing details. The
//! supervisor passes the host's `details` object on untouched (an opaque JSON
//! value under a size limit), so each field is judged here and one that does
//! not hold is dropped by itself; a bad value never costs the room its poll.
use std::collections::HashMap;

use ember_protocol::{
    rooms::{
        FIGHTER_LIMIT, MAX_CAPACITY, MAX_ROOM_TYPE, MAX_ROTATION, MAX_WAITING, MIN_CAPACITY,
        NO_FIGHTER, WaitingPlayer, check_host_name, check_room_name, is_listed_code,
        set_format_valid,
    },
    skill::SKILL_BANDS,
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
    /// Classic, Arcade or King of the hill (`MAX_ROOM_TYPE`). The creator
    /// picks it just after the room opens, so a later report may change it.
    pub room_type: Option<u8>,
    /// The room's creator is a member of it now. A host that predates the
    /// field says nothing, which keeps what the row had.
    pub creator_in: Option<bool>,
    /// Whether the room host plays each fighter at their own input delay. The
    /// host writes `own_delay: true` only for such a room, so a report without
    /// it (an older host, or a room with the shared delay) reads as the shared
    /// delay and replaces what the room was created asking for: the host's
    /// word is the room's.
    pub own_delay: bool,
    /// Only players not on Wi-Fi take a seat. The host writes `wired_only:
    /// true` only for such a room and the room's host can turn it off, so a
    /// report without it reads as off and replaces what the row had.
    pub wired_only: bool,
}

fn small(value: &Value, limit: u64) -> Option<u8> {
    value
        .as_u64()
        .filter(|number| *number <= limit)
        .and_then(|number| u8::try_from(number).ok())
}

/// Who is in a room, as its host last reported: what Quick join weighs. Kept
/// in memory only, so a restarted bridge lists none until its first poll.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct QuickJoin {
    /// One band per member, ascending (`RoomSummary::classes`). `None` when
    /// not said or not valid.
    pub classes: Option<Vec<u8>>,
    /// The players a newcomer meets at once, in that order
    /// (`RoomSummary::waiting`). `None` when not said or when any entry has
    /// no valid class.
    pub waiting: Option<Vec<WaitingPlayer>>,
}

/// The `QuickJoin` of each room the last committed poll read, by room id.
pub type QuickJoins = HashMap<String, QuickJoin>;

impl QuickJoin {
    /// Reads `value`, a report's `details`, for a room of `members` members.
    /// Each field is judged alone, and a waiting player's area that is not a
    /// listed code reads as none, leaving the player.
    pub fn from_value(value: &Value, members: u8) -> Self {
        let classes = value
            .get("classes")
            .and_then(Value::as_array)
            .filter(|list| list.len() <= usize::from(members))
            .and_then(|list| {
                list.iter()
                    .map(|class| small(class, SKILL_BANDS.into()))
                    .collect::<Option<Vec<u8>>>()
            })
            .map(|mut classes| {
                classes.sort_unstable();
                classes
            });
        let waiting = value
            .get("waiting")
            .and_then(Value::as_array)
            .filter(|list| list.len() <= MAX_WAITING.min(usize::from(members)))
            .and_then(|list| {
                list.iter()
                    .map(waiting_player)
                    .collect::<Option<Vec<WaitingPlayer>>>()
            });
        Self { classes, waiting }
    }

    /// The lists that fit a room of `members` members: a longer one is left
    /// out, as for a report whose member count the room's capacity cut.
    pub fn within(self, members: u8) -> Self {
        let fits = |len: usize| len <= usize::from(members);
        Self {
            classes: self.classes.filter(|classes| fits(classes.len())),
            waiting: self.waiting.filter(|waiting| fits(waiting.len())),
        }
    }

    /// What a listing shows: all of it while classes are shown, otherwise no
    /// classes and every waiting player's class as 0, their areas kept.
    pub fn shown(&self, classes: bool) -> Self {
        if classes {
            return self.clone();
        }
        Self {
            classes: None,
            waiting: self.waiting.as_ref().map(|list| {
                list.iter()
                    .map(|player| WaitingPlayer {
                        class: 0,
                        area: player.area.clone(),
                    })
                    .collect()
            }),
        }
    }
}

fn waiting_player(entry: &Value) -> Option<WaitingPlayer> {
    Some(WaitingPlayer {
        class: entry
            .get("class")
            .and_then(|class| small(class, SKILL_BANDS.into()))?,
        area: entry
            .get("area")
            .and_then(Value::as_str)
            .filter(|area| is_listed_code(area))
            .map(str::to_owned),
    })
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
            room_type: value
                .get("room_type")
                .and_then(|kind| small(kind, MAX_ROOM_TYPE.into())),
            creator_in: value.get("creator_in").and_then(Value::as_bool),
            own_delay: value.get("own_delay").and_then(Value::as_bool) == Some(true),
            wired_only: value.get("wired_only").and_then(Value::as_bool) == Some(true),
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
            "room_type": 2, "creator_in": false, "own_delay": true, "wired_only": true, "unknown": [1] });
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
                room_type: Some(2),
                creator_in: Some(false),
                own_delay: true,
                wired_only: true,
            }
        );
    }

    #[test]
    fn a_report_says_own_delay_only_by_saying_true() {
        let own =
            |value: Value| Details::from_value(&json!({ "own_delay": value }), 2, 4).own_delay;
        assert!(own(json!(true)));
        for other in [
            json!(false),
            json!(null),
            json!(1),
            json!("true"),
            json!([true]),
        ] {
            assert!(!own(other.clone()), "{other}");
        }
        // Nothing said, or not a report at all: the shared delay.
        assert!(!Details::from_value(&json!({}), 2, 4).own_delay);
        assert!(!Details::absent(2, 4).own_delay);
    }

    #[test]
    fn a_report_says_wired_only_only_by_saying_true() {
        let wired =
            |value: Value| Details::from_value(&json!({ "wired_only": value }), 2, 4).wired_only;
        assert!(wired(json!(true)));
        for other in [json!(false), json!(null), json!(1), json!("true")] {
            assert!(!wired(other.clone()), "{other}");
        }
        assert!(!Details::from_value(&json!({}), 2, 4).wired_only);
        assert!(!Details::absent(2, 4).wired_only);
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
            "host_name": "x".repeat(33), "fighters": [64], "set_format": 11, "rotation": 3,
            "room_type": 3, "creator_in": 1 });
        assert_eq!(Details::from_value(&value, 0, 4), Details::default());
        let mixed = json!({ "name": "Fine", "capacity": 300, "locked": false, "host_name": "",
            "fighters": [1, 2, 3], "set_format": 5, "rotation": 0,
            "room_type": 1 });
        let read = Details::from_value(&mixed, 2, 4);
        assert_eq!(
            (read.name.as_deref(), read.capacity, read.locked),
            (Some("Fine"), None, Some(false))
        );
        // More fighters than members, and no host name that qualifies.
        assert_eq!((read.fighters, read.host_name), (None, None));
        assert_eq!(
            (read.set_format, read.rotation, read.room_type),
            (Some(5), Some(0), Some(1))
        );
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

    fn waiting(class: u8, area: Option<&str>) -> WaitingPlayer {
        WaitingPlayer {
            class,
            area: area.map(str::to_owned),
        }
    }

    #[test]
    fn classes_are_read_sorted_and_held_to_the_members_and_the_bound() {
        let classes = |value: Value, members: u8| {
            QuickJoin::from_value(&json!({ "classes": value }), members).classes
        };
        assert_eq!(classes(json!([5, 0, 3]), 3), Some(vec![0, 3, 5]));
        assert_eq!(classes(json!([]), 0), Some(Vec::new()));
        assert_eq!(classes(json!([10, 10]), 2), Some(vec![10, 10]));
        for refused in [
            json!([1, 2, 3, 4]),
            json!([11]),
            json!([1, -1]),
            json!([1, 2.5]),
            json!([1, "2"]),
            json!([1, null]),
            json!({ "0": 1 }),
            json!(3),
            json!(null),
        ] {
            assert_eq!(classes(refused.clone(), 3), None, "{refused}");
        }
    }

    #[test]
    fn waiting_players_are_read_in_order_and_an_unlisted_area_leaves_the_player() {
        let read = |value: Value, members: u8| {
            QuickJoin::from_value(&json!({ "waiting": value }), members).waiting
        };
        assert_eq!(
            read(
                json!([{ "class": 6, "area": "nae" }, { "class": 0 }, { "class": 9, "area": "n9" }]),
                3
            ),
            Some(vec![
                waiting(6, Some("nae")),
                waiting(0, None),
                waiting(9, Some("n9"))
            ])
        );
        for area in [
            json!("NAE"),
            json!("na-e"),
            json!(""),
            json!("toolong99"),
            json!(4),
            json!(null),
        ] {
            assert_eq!(
                read(json!([{ "class": 2, "area": area }]), 2),
                Some(vec![waiting(2, None)]),
                "{area}"
            );
        }
        assert_eq!(read(json!([]), 2), Some(Vec::new()));
    }

    #[test]
    fn a_waiting_list_that_does_not_hold_is_dropped_whole() {
        let read = |value: Value, members: u8| {
            QuickJoin::from_value(&json!({ "waiting": value }), members).waiting
        };
        let one = json!({ "class": 1 });
        for refused in [
            json!([one, { "class": 11 }]),
            json!([one, { "area": "nae" }]),
            json!([one, { "class": -1 }]),
            json!([one, { "class": "3" }]),
            json!([one, 7]),
            json!([one, null]),
            json!({ "class": 1 }),
            json!("waiting"),
        ] {
            assert_eq!(read(refused.clone(), 8), None, "{refused}");
        }
        // More than the members, or than `MAX_WAITING`.
        assert_eq!(read(json!([one, one, one]), 2), None);
        assert!(read(json!(vec![one.clone(); MAX_WAITING]), 12).is_some());
        assert_eq!(read(json!(vec![one; MAX_WAITING + 1]), 12), None);
    }

    #[test]
    fn the_two_fields_are_judged_alone_and_the_rest_of_the_report_is_not_touched() {
        let value = json!({ "classes": [4, 2], "waiting": [{ "class": 99 }], "name": "Fine" });
        let read = QuickJoin::from_value(&value, 2);
        assert_eq!((read.classes, read.waiting), (Some(vec![2, 4]), None));
        assert_eq!(
            Details::from_value(&value, 2, 4).name.as_deref(),
            Some("Fine")
        );
        for nothing in [json!({}), json!(null), json!("x"), json!([1])] {
            assert_eq!(QuickJoin::from_value(&nothing, 2), QuickJoin::default());
        }
    }

    #[test]
    fn a_list_longer_than_the_room_holds_is_left_out_alone() {
        let read = QuickJoin {
            classes: Some(vec![1, 2, 3]),
            waiting: Some(vec![waiting(4, None)]),
        };
        assert_eq!(read.clone().within(3), read);
        assert_eq!(
            read.within(2),
            QuickJoin {
                classes: None,
                waiting: Some(vec![waiting(4, None)]),
            }
        );
    }

    #[test]
    fn classes_not_shown_leave_no_class_and_zero_every_waiting_class() {
        let read = QuickJoin {
            classes: Some(vec![3, 5]),
            waiting: Some(vec![
                waiting(5, Some("nae")),
                waiting(0, None),
                waiting(9, None),
            ]),
        };
        assert_eq!(read.shown(true), read);
        assert_eq!(
            read.shown(false),
            QuickJoin {
                classes: None,
                waiting: Some(vec![
                    waiting(0, Some("nae")),
                    waiting(0, None),
                    waiting(0, None)
                ]),
            }
        );
        assert_eq!(QuickJoin::default().shown(false), QuickJoin::default());
    }

    #[test]
    fn a_value_that_is_not_an_object_reads_as_nothing() {
        for value in [json!(null), json!("x"), json!([1]), json!(7)] {
            assert_eq!(Details::from_value(&value, 2, 4), Details::absent(2, 4));
        }
    }
}
