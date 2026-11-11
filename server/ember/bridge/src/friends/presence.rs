//! What the bridge keeps of friends in memory only (FRIENDS.md, "Kept in
//! memory"): each account's presence as its game last reported it, and the
//! invites friends send each other. A restart forgets both: players are back
//! within a sync, and an invite is ten minutes of convenience.
use std::{collections::HashMap, sync::Mutex};

use ember_protocol::friends::{Activity, InviteRoom, MAX_INVITES, PresenceReport, RoomReport};

use crate::util::new_id;

/// A sync this recent shows its account online.
pub const ONLINE_SECS: u64 = 75;
/// A presence not renewed for this long is dropped.
const KEPT_SECS: u64 = 10 * 60;
/// How long an invite stands.
pub const INVITE_SECS: u64 = 10 * 60;

/// What a player is doing, as their game last said.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Doing {
    Activity(Activity),
    Room(RoomReport),
}

/// An account's last presence and the build it was sent from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Seen {
    pub doing: Doing,
    pub build_id: String,
    pub beat_at: u64,
}

impl Seen {
    pub fn online(&self, now: u64) -> bool {
        now.saturating_sub(self.beat_at) <= ONLINE_SECS
    }
}

/// An invite one friend sent another.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Invite {
    /// `iv_`.
    pub id: String,
    /// The inviting account.
    pub inviter: String,
    pub room: InviteRoom,
    pub expires_at: u64,
}

/// Presence by account, and invites by the account invited.
#[derive(Default)]
pub struct Live(Mutex<Inner>);

#[derive(Default)]
struct Inner {
    seen: HashMap<String, Seen>,
    /// Oldest first, one per inviter, at most `MAX_INVITES`.
    invites: HashMap<String, Vec<Invite>>,
}

impl Live {
    fn inner(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.0
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// Records `account`'s presence; `offline` forgets it at once.
    pub fn report(&self, account: &str, presence: PresenceReport, build_id: String, now: u64) {
        let doing = match presence {
            PresenceReport::Menus {} => Doing::Activity(Activity::Menus),
            PresenceReport::OfflinePlay {} => Doing::Activity(Activity::OfflinePlay),
            PresenceReport::Room { room } => Doing::Room(room),
            PresenceReport::Offline {} => {
                self.inner().seen.remove(account);
                return;
            }
        };
        let seen = Seen {
            doing,
            build_id,
            beat_at: now,
        };
        self.inner().seen.insert(account.to_owned(), seen);
    }

    /// `account`'s last presence, online or not.
    pub fn seen(&self, account: &str) -> Option<Seen> {
        self.inner().seen.get(account).cloned()
    }

    pub fn online(&self, account: &str, now: u64) -> bool {
        self.seen(account).is_some_and(|seen| seen.online(now))
    }

    /// Offers `invitee` `inviter`'s room until `INVITE_SECS` from now, in
    /// place of any invite `inviter` sent them before. Past `MAX_INVITES` the
    /// invitee's oldest goes, so nobody can learn how many they hold.
    pub fn invite(&self, inviter: &str, invitee: &str, room: InviteRoom, now: u64) {
        let mut inner = self.inner();
        let held = inner.invites.entry(invitee.to_owned()).or_default();
        held.retain(|invite| invite.inviter != inviter && invite.expires_at > now);
        if held.len() >= MAX_INVITES {
            held.remove(0);
        }
        held.push(Invite {
            id: new_id("iv"),
            inviter: inviter.to_owned(),
            room,
            expires_at: now + INVITE_SECS,
        });
    }

    /// Drops the invite `id` that `invitee` received.
    pub fn dismiss(&self, invitee: &str, id: &str) {
        if let Some(held) = self.inner().invites.get_mut(invitee) {
            held.retain(|invite| invite.id != id);
        }
    }

    /// The live invites `invitee` received, oldest first.
    pub fn invites_for(&self, invitee: &str, now: u64) -> Vec<Invite> {
        self.inner()
            .invites
            .get(invitee)
            .into_iter()
            .flatten()
            .filter(|invite| invite.expires_at > now)
            .cloned()
            .collect()
    }

    /// Ends the invites between two accounts, both ways: they are no longer
    /// friends.
    pub fn unpair(&self, one: &str, other: &str) {
        let mut inner = self.inner();
        for (invitee, inviter) in [(one, other), (other, one)] {
            if let Some(held) = inner.invites.get_mut(invitee) {
                held.retain(|invite| invite.inviter != inviter);
            }
        }
    }

    /// Forgets everything of `account`: its presence and every invite it
    /// sent or received.
    pub fn forget(&self, account: &str) {
        let mut inner = self.inner();
        inner.seen.remove(account);
        inner.invites.remove(account);
        for held in inner.invites.values_mut() {
            held.retain(|invite| invite.inviter != account);
        }
    }

    /// Drops presences not renewed for `KEPT_SECS` and invites that lapsed.
    pub fn prune(&self, now: u64) {
        let mut inner = self.inner();
        inner
            .seen
            .retain(|_, seen| now.saturating_sub(seen.beat_at) < KEPT_SECS);
        inner.invites.retain(|_, held| {
            held.retain(|invite| invite.expires_at > now);
            !held.is_empty()
        });
    }
}

#[cfg(test)]
mod tests {
    use ember_protocol::friends::RoomStatus;

    use super::*;

    const NOW: u64 = 1_731_000_000;

    fn room() -> InviteRoom {
        InviteRoom::RoomId("0f0123456789abcdef0123456789abcd".into())
    }

    #[test]
    fn a_presence_is_online_for_75_seconds_and_kept_for_ten_minutes() {
        let live = Live::default();
        let training = RoomReport {
            room_id: None,
            private: true,
            status: RoomStatus::Training,
            table: Some(2),
        };
        live.report(
            "a",
            PresenceReport::Room {
                room: training.clone(),
            },
            "build".into(),
            NOW,
        );
        assert_eq!(
            live.seen("a"),
            Some(Seen {
                doing: Doing::Room(training),
                build_id: "build".into(),
                beat_at: NOW,
            })
        );
        assert!(live.online("a", NOW + ONLINE_SECS));
        assert!(!live.online("a", NOW + ONLINE_SECS + 1));
        live.prune(NOW + KEPT_SECS - 1);
        assert!(live.seen("a").is_some());
        live.prune(NOW + KEPT_SECS);
        assert_eq!(live.seen("a"), None);
        assert!(!live.online("nobody", NOW));
    }

    #[test]
    fn an_offline_report_forgets_the_presence_at_once() {
        let live = Live::default();
        live.report("a", PresenceReport::Menus {}, "build".into(), NOW);
        assert_eq!(
            live.seen("a").map(|seen| seen.doing),
            Some(Doing::Activity(Activity::Menus))
        );
        live.report("a", PresenceReport::Offline {}, "build".into(), NOW + 1);
        assert_eq!(live.seen("a"), None);
    }

    #[test]
    fn one_invite_a_pair_and_at_most_twenty_an_invitee() {
        let live = Live::default();
        live.invite("a", "b", room(), NOW);
        let first = live.invites_for("b", NOW);
        assert_eq!(first.len(), 1);
        assert_eq!(first[0].expires_at, NOW + INVITE_SECS);
        // A second invite from the same friend replaces the first.
        let secret = InviteRoom::Secret(format!("emd1:{}", "A".repeat(122)));
        live.invite("a", "b", secret.clone(), NOW + 5);
        let second = live.invites_for("b", NOW + 5);
        assert_eq!(second.len(), 1);
        assert_ne!(second[0].id, first[0].id);
        assert_eq!(second[0].room, secret);

        // Past twenty, the oldest goes.
        for inviter in 0..MAX_INVITES {
            live.invite(&format!("f{inviter}"), "b", room(), NOW + 10);
        }
        let held = live.invites_for("b", NOW + 10);
        assert_eq!(held.len(), MAX_INVITES);
        assert!(held.iter().all(|invite| invite.inviter != "a"));

        // Lapsed invites are not live, and pruning drops them.
        assert!(live.invites_for("b", NOW + 10 + INVITE_SECS).is_empty());
        live.prune(NOW + 10 + INVITE_SECS);
        assert!(live.0.lock().unwrap().invites.is_empty());
    }

    #[test]
    fn dismissing_unpairing_and_forgetting_end_invites() {
        let live = Live::default();
        live.invite("a", "b", room(), NOW);
        live.invite("b", "a", room(), NOW);
        live.invite("c", "b", room(), NOW);
        let from_c = live.invites_for("b", NOW)[1].id.clone();
        // Only the invitee dismisses their own invite.
        live.dismiss("a", &from_c);
        assert_eq!(live.invites_for("b", NOW).len(), 2);
        live.dismiss("b", &from_c);
        assert_eq!(live.invites_for("b", NOW).len(), 1);

        live.unpair("b", "a");
        assert!(live.invites_for("a", NOW).is_empty());
        assert!(live.invites_for("b", NOW).is_empty());

        live.invite("a", "b", room(), NOW);
        live.invite("b", "c", room(), NOW);
        live.report("b", PresenceReport::Menus {}, "build".into(), NOW);
        live.forget("b");
        assert!(live.invites_for("b", NOW).is_empty());
        assert!(live.invites_for("c", NOW).is_empty());
        assert_eq!(live.seen("b"), None);
    }
}
