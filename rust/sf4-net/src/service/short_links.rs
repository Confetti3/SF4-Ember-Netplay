//! Short invitation links: publishing this room's sealed invitation to the
//! link service on request, keeping it current, and opening a pasted link.
use super::*;
use crate::short_invite::{self, Keys, ShortError};

/// A published record is stored again at least this often, so a restarted
/// service (it keeps records in memory) has the room back within minutes.
const REPUBLISH_SECS: u64 = 10 * 60;
/// After a background store fails, wait this long before the next try.
const RETRY_SECS: u64 = 60;

pub(super) struct ShortLinks {
    /// Base URL of the service, `short_invite::service_base()`.
    pub(super) service: String,
    pub(super) room: Option<ShortRoom>,
}

impl Default for ShortLinks {
    fn default() -> Self {
        Self {
            service: short_invite::service_base(),
            room: None,
        }
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

    /// Once a second: store the room's invitation again when it has changed
    /// (renewal, a new leader) or has not been stored for a while.
    pub(super) fn pump_short_link(&mut self, time: u64) {
        let Some(room) = self.short.room.as_ref() else {
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
