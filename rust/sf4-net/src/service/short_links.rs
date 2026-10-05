//! Short invitation links: publishing this room's sealed invitation to the
//! link service on request, keeping it current (whoever leads the room takes
//! over a link any member shared), and opening a pasted link.
use super::*;
use crate::short_invite::{self, Keys, ShortError};

/// A published record is stored again at least this often, so a restarted
/// service (it keeps records in memory) has the room back within minutes.
const REPUBLISH_SECS: u64 = 10 * 60;
/// After a background store fails, wait this long before the next try.
const RETRY_SECS: u64 = 60;
/// How many times a leader looks its room's record up when the service does
/// not answer.
const ADOPT_TRIES: u8 = 3;

pub(super) struct ShortLinks {
    /// Base URL of the service, `short_invite::service_base()`.
    pub(super) service: String,
    pub(super) room: Option<ShortRoom>,
    /// A lookup of this room's record is under way (`adopt_short_link`).
    pub(super) adopting: bool,
    /// When to look the record up: asked for by `schedule_adopt`, or again
    /// after the service did not answer. Zero when no lookup is due.
    pub(super) adopt_at: u64,
    /// Lookups made since the last event that asked for one.
    pub(super) adopt_tries: u8,
    /// The coordination term this game last led the room in; zero while it
    /// does not lead.
    pub(super) led_term: u64,
    /// Another member has led the room since this game opened or joined it.
    pub(super) followed: bool,
}

impl Default for ShortLinks {
    fn default() -> Self {
        Self {
            service: short_invite::service_base(),
            room: None,
            adopting: false,
            adopt_at: 0,
            adopt_tries: 0,
            led_term: 0,
            followed: false,
        }
    }
}

impl ShortLinks {
    /// Leaving a room forgets its link and any lookup of it.
    pub(super) fn clear(&mut self) {
        *self = Self {
            service: std::mem::take(&mut self.service),
            ..Self::default()
        };
    }

    /// A short link any member shared is kept current by whoever leads the
    /// room, so this game looks for one when the lead passes to it: from
    /// another member, or back to the game that opened the room. A game that
    /// opened the room and has led it since published any link itself. One
    /// lookup per change of leader, never one per refresh.
    pub(super) fn follow_leader(
        &mut self,
        leading: bool,
        term: u64,
        other_leads: bool,
        opened_here: bool,
        time: u64,
    ) {
        if !leading {
            self.led_term = 0;
            self.followed |= other_leads;
            return;
        }
        if self.led_term == term {
            return;
        }
        self.led_term = term;
        if !opened_here || self.followed {
            self.schedule_adopt(time);
        }
    }

    /// Something happened that can leave a shared link with nobody keeping
    /// it current (a member left, the lead moved): look the room's record up
    /// on the next pump. A burst of such events makes one lookup, and none is
    /// made while this game holds the link.
    pub(super) fn schedule_adopt(&mut self, time: u64) {
        if self.room.is_some() {
            return;
        }
        self.adopt_tries = 0;
        self.adopt_at = time.max(1);
    }
}

pub(super) struct ShortRoom {
    code: String,
    keys: Option<Keys>,
    /// The invitation text the service holds, and when it was stored.
    published: Option<String>,
    published_at: u64,
    retry_at: u64,
    in_flight: bool,
    /// The native side has the link; later failures retry quietly.
    announced: bool,
}

/// One finished store: the derived keys to keep, the text that was sent and
/// whether the service took it.
pub(super) struct ShortPublished {
    pub(super) keys: Option<Keys>,
    pub(super) invitation: String,
    pub(super) result: Result<(), ShortError>,
}

impl Actor {
    fn current_invite(&self) -> Option<Invite> {
        self.room_invite.clone().or_else(|| self.hosted.clone())
    }

    fn emit_short(&self, epoch: u64, code: Option<&str>) -> io::Result<()> {
        self.emit(Event::ShortInvite {
            epoch,
            link: code.map(short_invite::link).unwrap_or_default(),
            status: if code.is_some() {
                "ready"
            } else {
                "unavailable"
            }
            .into(),
        })
    }

    /// The native side asked for the room's short link. It hears back with
    /// one `short_invite` event: the link, or `unavailable`, after which it
    /// shares the full invitation instead.
    pub(super) fn short_invite_command(&mut self, epoch: u64) -> io::Result<()> {
        let current = self.current_invite().filter(|_| self.matches(epoch));
        let Some(current) = current else {
            return self.emit_short(epoch, None);
        };
        let code = current.short_code();
        match self.short.room.as_ref() {
            Some(room) if room.code == code && room.announced => {
                return self.emit_short(epoch, Some(&code));
            }
            // Its answer is on the way.
            Some(room) if room.code == code && room.in_flight => return Ok(()),
            _ => {}
        }
        self.short.room = Some(ShortRoom {
            code,
            keys: None,
            published: None,
            published_at: 0,
            retry_at: 0,
            in_flight: false,
            announced: false,
        });
        let time = now().unwrap_or(0);
        if !self.spawn_short_publish(time) {
            self.short.room = None;
            return self.emit_short(epoch, None);
        }
        Ok(())
    }

    fn spawn_short_publish(&mut self, time: u64) -> bool {
        let Some(current) = self.current_invite() else {
            return false;
        };
        let Ok(invitation) = current.encode() else {
            return false;
        };
        if self.tasks.len() >= MAX_TASKS {
            return false;
        }
        let Some(room) = self.short.room.as_mut() else {
            return false;
        };
        room.in_flight = true;
        let ttl = current.expires().saturating_sub(time);
        let (service, keys, code, epoch) = (
            self.short.service.clone(),
            room.keys.clone(),
            room.code.clone(),
            self.epoch,
        );
        self.tasks.spawn(async move {
            let keys = match keys {
                Some(keys) => Ok(keys),
                None => short_invite::derive_async(code).await,
            };
            let published = match keys {
                Ok(keys) => {
                    let result = short_invite::publish(&service, &keys, &invitation, ttl).await;
                    ShortPublished {
                        keys: Some(keys),
                        invitation,
                        result,
                    }
                }
                Err(error) => ShortPublished {
                    keys: None,
                    invitation,
                    result: Err(error),
                },
            };
            Completion::ShortPublished(epoch, published)
        });
        true
    }

    pub(super) fn completed_short_publish(
        &mut self,
        epoch: u64,
        published: ShortPublished,
    ) -> io::Result<()> {
        if epoch != self.epoch {
            return Ok(());
        }
        let time = now().unwrap_or(0);
        let Some(room) = self.short.room.as_mut() else {
            return Ok(());
        };
        room.in_flight = false;
        if published.keys.is_some() {
            room.keys = published.keys;
        }
        match published.result {
            Ok(()) => {
                room.published = Some(published.invitation);
                room.published_at = time;
                if !room.announced {
                    room.announced = true;
                    let code = room.code.clone();
                    return self.emit_short(epoch, Some(&code));
                }
            }
            Err(_) if room.announced => room.retry_at = time + RETRY_SECS,
            Err(_) => {
                self.short.room = None;
                return self.emit_short(epoch, None);
            }
        }
        Ok(())
    }

    /// This game leads the room and may have a shared short link to keep
    /// current (`ShortLinks::schedule_adopt`). A shared short link stays
    /// current only while the room's leader stores the invitation again, and
    /// whoever copied it may have left, so the leader takes the link over
    /// when the link service holds one for this room. A room nobody shared a
    /// link for has no record there, and nothing is published for it. The
    /// code comes from the room's own secrets, so a record under it is this
    /// room's.
    pub(super) fn adopt_short_link(&mut self) {
        // A lookup under way keeps a newer request: it runs once this one is
        // answered, in case the link was shared after the record was read.
        if self.short.adopting {
            return;
        }
        if self.short.room.is_some() || self.short.adopt_tries >= ADOPT_TRIES {
            self.short.adopt_at = 0;
            return;
        }
        // Busy workers or an invitation not yet at hand defer the lookup to
        // a later pump; it is not dropped.
        let current = match self.current_invite() {
            Some(current) if self.tasks.len() < MAX_TASKS => current,
            _ => {
                self.short.adopt_at = self.short.adopt_at.max(1);
                return;
            }
        };
        self.short.adopt_at = 0;
        let code = current.short_code();
        self.short.adopting = true;
        self.short.adopt_tries += 1;
        let (service, epoch) = (self.short.service.clone(), self.epoch);
        self.tasks.spawn(async move {
            let found = match short_invite::derive_async(code.clone()).await {
                Ok(keys) => short_invite::fetch(&service, &keys)
                    .await
                    .map(|invitation| (keys, invitation)),
                Err(error) => Err(error),
            };
            Completion::ShortAdopted(epoch, code, found)
        });
    }

    pub(super) fn completed_short_adopt(
        &mut self,
        epoch: u64,
        code: String,
        found: Result<(Keys, String), ShortError>,
    ) -> io::Result<()> {
        if epoch != self.epoch {
            return Ok(());
        }
        self.short.adopting = false;
        let time = now().unwrap_or(0);
        let current = self.current_invite().map(|invite| invite.short_code());
        if self.short.room.is_some() || current.as_deref() != Some(code.as_str()) {
            return Ok(());
        }
        match found {
            Ok((keys, invitation)) => {
                // Stored again on the next pump: the record still names the
                // leader that published it.
                self.short.adopt_at = 0;
                self.short.room = Some(ShortRoom {
                    code,
                    keys: Some(keys),
                    published: Some(invitation),
                    published_at: 0,
                    retry_at: 0,
                    in_flight: false,
                    announced: true,
                });
            }
            // Nobody shared a link for this room.
            Err(ShortError::Unknown) => {}
            Err(_) if self.short.adopt_tries < ADOPT_TRIES => {
                self.short.adopt_at = time + RETRY_SECS
            }
            Err(_) => {}
        }
        Ok(())
    }

    /// Once a second: store the room's invitation again when it has changed
    /// (renewal, a new leader) or has not been stored for a while.
    pub(super) fn pump_short_link(&mut self, time: u64) {
        let Some(room) = self.short.room.as_ref() else {
            let leading = self
                .last_coordination_state
                .is_some_and(|marker| marker.3);
            if self.short.adopt_at != 0 && time >= self.short.adopt_at && leading {
                self.adopt_short_link();
            }
            return;
        };
        if !room.announced || room.in_flight || time < room.retry_at {
            return;
        }
        let Some(current) = self.current_invite() else {
            self.short.room = None;
            return;
        };
        if current.short_code() != room.code {
            self.short.room = None;
            return;
        }
        let changed = current.encode().ok() != room.published;
        if (changed || time >= room.published_at + REPUBLISH_SECS)
            && !self.spawn_short_publish(time)
            && let Some(room) = self.short.room.as_mut()
        {
            room.retry_at = time + RETRY_SECS;
        }
    }

    /// Join with a pasted short link: resolve it first, then join with the
    /// invitation it stands for.
    pub(super) fn join_short(
        &mut self,
        id: u64,
        epoch: u64,
        code: String,
        build: String,
    ) -> io::Result<()> {
        if self.tasks.len() >= MAX_TASKS {
            self.clear_room();
            return self.error_because(
                id,
                "invalid_or_incompatible_invitation",
                Some(ShortError::Unavailable.reason()),
            );
        }
        let service = self.short.service.clone();
        self.tasks.spawn(async move {
            let result = short_invite::resolve(&service, code.clone()).await;
            Completion::ShortResolved(id, epoch, build, code, result)
        });
        Ok(())
    }

    pub(super) fn completed_short_resolve(
        &mut self,
        id: u64,
        epoch: u64,
        build: String,
        code: String,
        result: Result<String, ShortError>,
    ) -> io::Result<()> {
        // A Leave or a newer room since the lookup started owns the epoch.
        if !self.matches(epoch) || !self.opening || self.room.is_some() {
            return Ok(());
        }
        match result {
            Ok(invitation) => self.join_invitation(id, &invitation, build, Some(&code)),
            Err(error) => {
                self.clear_room();
                self.error_because(
                    id,
                    "invalid_or_incompatible_invitation",
                    Some(error.reason()),
                )
            }
        }
    }
}
