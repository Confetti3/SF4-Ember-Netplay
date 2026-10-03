//! The coordination rules of a server-owned (public) room: the host is the
//! room's only voter for its whole life, and every other node is a learner
//! that takes the log from that host and from nobody else. A room that loses
//! its host is not recovered; see `docs/design/PUBLIC_ROOMS.md`.
use super::MAX_MEMBERS;

/// How a coordinator takes part in its room.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Ownership {
    /// A private room: voters are chosen from the members and the leader can
    /// change.
    #[default]
    Private,
    /// The sole voter and leader of a server-owned room.
    Host,
    /// A learner of a server-owned room. `host` is the incarnation of the host
    /// it was admitted by, named in the invitation it joined with.
    Member { host: u64 },
}

impl Ownership {
    pub fn is_server_owned(self) -> bool {
        self != Self::Private
    }

    /// The most nodes the room's Raft membership may hold. A private host is
    /// one of the `MAX_MEMBERS` members; a server-owned host is not a member,
    /// so it comes on top of them.
    pub fn max_nodes(self) -> usize {
        if self.is_server_owned() {
            MAX_MEMBERS + 1
        } else {
            MAX_MEMBERS
        }
    }

    /// Whether this node may start an election when it hears nothing from the
    /// leader. A learner never does, and never would be asked to.
    pub fn campaigns(self) -> bool {
        !matches!(self, Self::Member { .. })
    }

    /// Whether an RPC `method` from the node `source` is served at all. A
    /// refusal looks like the one a non-member gets.
    ///
    /// A host is the leader for good, so nothing appends to it, asks it for a
    /// vote or hands it a snapshot. A member takes everything from its host
    /// and from nothing else, and grants no votes.
    pub fn serves(self, source: u64, method: &str) -> bool {
        match self {
            Self::Private => true,
            Self::Host => !matches!(method, "append" | "vote" | "snapshot" | "elect"),
            Self::Member { host } => source == host && method != "vote",
        }
    }

    /// The only voter set a server-owned room can hold, for a node that
    /// is `own`: itself, when it is the host.
    pub fn permits_voters(self, own: u64, voters: &std::collections::BTreeSet<u64>) -> bool {
        match self {
            Self::Private => true,
            Self::Host => voters.len() == 1 && voters.contains(&own),
            Self::Member { .. } => false,
        }
    }
}
