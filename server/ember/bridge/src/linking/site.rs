//! Links a platform gets from a player's sign-in on another site (Discord,
//! Challonge) instead of from a link code. The player connected the site's
//! account to their Ember ID (`routes::sign_in`); a platform that finds
//! players by that account (`find_and_link`) then gets a link approved by the
//! sign-in (`approved_via` the site's name, claim `<site>:<user ID>`).
//!
//! Such a link stands only while that account is connected to that Ember ID:
//! moving or disconnecting the account ends the links its sign-in approved,
//! in the same transaction, and the next lookup links the account again
//! where it now belongs. A link the player (or the platform account's holder)
//! removes stays removed (`withdraw`) until the player completes a sign-in
//! started after that.
use ember_protocol::{
    EmberId, challonge::username_key, discord::is_user_id, event::Kind, sign_in::Account,
};
use rusqlite::{OptionalExtension, Transaction, params};
use serde_json::json;

use super::{account, revoke_link, sign_ins};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    util::new_id,
};

/// A site whose sign-in connects an account to an Ember ID. Each has its own
/// accounts, sign-ins (`sign_ins`) and withdrawals tables, shaped alike
/// where this module reads them. (A third site is the time to fold them
/// into one set of tables with a `site` column.)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Site {
    Discord,
    Challonge,
}

/// An account a lookup found and linked on the connection.
pub struct Found<'a> {
    /// The key the lookup asked with.
    pub asked: &'a str,
    pub user_id: String,
    /// When the account connected.
    pub connected_at: u64,
    pub ember_id: EmberId,
    pub participant_id: String,
}

impl Site {
    pub const ALL: [Self; 2] = [Self::Discord, Self::Challonge];

    /// `links.approved_via`, and the prefix of the subject and claim.
    pub fn name(self) -> &'static str {
        match self {
            Self::Discord => "discord",
            Self::Challonge => "challonge",
        }
    }

    fn of(approved_via: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|site| site.name() == approved_via)
    }

    fn accounts(self) -> &'static str {
        match self {
            Self::Discord => "discord_accounts",
            Self::Challonge => "challonge_accounts",
        }
    }

    fn withdrawals(self) -> &'static str {
        match self {
            Self::Discord => "discord_withdrawals",
            Self::Challonge => "challonge_withdrawals",
        }
    }

    fn subject(self, user_id: &str) -> String {
        format!("{}:{user_id}", self.name())
    }

    /// The Ember ID's connected account, if any.
    pub fn account(self, tx: &Transaction<'_>, ember_id: &str) -> Result<Option<Account>> {
        Ok(tx
            .query_row(
                &format!(
                    "SELECT user_id, username, connected_at FROM {} WHERE ember_id = ?1",
                    self.accounts()
                ),
                [ember_id],
                |row| {
                    Ok(Account {
                        user_id: row.get(0)?,
                        username: row.get(1)?,
                        connected_at: row.get(2)?,
                    })
                },
            )
            .optional()?)
    }

    /// The Ember ID account `user_id` is connected to, if any.
    pub fn owner(self, tx: &Transaction<'_>, user_id: &str) -> Result<Option<String>> {
        Ok(tx
            .query_row(
                &format!(
                    "SELECT ember_id FROM {} WHERE user_id = ?1",
                    self.accounts()
                ),
                [user_id],
                |row| row.get(0),
            )
            .optional()?)
    }

    /// Connects account `user_id` to `ember_id`. The latest sign-in wins: this
    /// account leaves any other Ember ID and this Ember ID leaves any other
    /// account, each ending the links its sign-in approved. The new row is
    /// fresh consent, since the withdrawals of the old one went with it.
    pub fn connect(
        self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        ember_id: &str,
        user_id: &str,
        username: &str,
    ) -> Result<()> {
        self.release(tx, ctx, ember_id, user_id)?;
        match self {
            Self::Discord => tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, ?4)",
                params![user_id, ember_id, username, ctx.now],
            )?,
            Self::Challonge => {
                // Challonge lets an account rename, and a name it gave up can
                // be taken by another. An older account stored under this
                // name answers for it no more; it answers again for the name
                // it next connects with.
                let key = username_key(username);
                tx.execute(
                    "UPDATE challonge_accounts SET username_key = NULL WHERE username_key = ?1 AND user_id != ?2",
                    params![key, user_id],
                )?;
                tx.execute(
                    "INSERT INTO challonge_accounts (user_id, ember_id, username, username_key, connected_at)
                     VALUES (?1, ?2, ?3, ?4, ?5)",
                    params![user_id, ember_id, username, key, ctx.now],
                )?
            }
        };
        Ok(())
    }

    fn release(self, tx: &Transaction<'_>, ctx: &Ctx, ember_id: &str, user_id: &str) -> Result<()> {
        let replaced: Vec<(String, String)> = tx
            .prepare(&format!(
                "DELETE FROM {} WHERE user_id = ?1 OR ember_id = ?2 RETURNING user_id, ember_id",
                self.accounts()
            ))?
            .query_map(params![user_id, ember_id], |row| {
                Ok((row.get(0)?, row.get(1)?))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (replaced_user, owner) in replaced {
            if (replaced_user.as_str(), owner.as_str()) != (user_id, ember_id) {
                self.end_links(tx, ctx, &replaced_user, "replaced")?;
            }
        }
        Ok(())
    }

    /// Disconnects the Ember ID's account, ending its sign-ins in flight
    /// (including one whose answer is still being checked with the site) and
    /// the links its sign-in approved.
    pub fn disconnect(self, tx: &Transaction<'_>, ctx: &Ctx, ember_id: &str) -> Result<()> {
        sign_ins::end(tx, self, ember_id)?;
        let removed: Option<String> = tx
            .query_row(
                &format!(
                    "DELETE FROM {} WHERE ember_id = ?1 RETURNING user_id",
                    self.accounts()
                ),
                [ember_id],
                |row| row.get(0),
            )
            .optional()?;
        match removed {
            Some(user_id) => {
                self.end_links(tx, ctx, &user_id, &format!("{}_disconnected", self.name()))
            }
            None => Ok(()),
        }
    }

    /// The account connected for a lookup key: a Discord user ID (anything
    /// else, such as the emails BluMint also sends, finds nothing), or a
    /// Challonge username compared without case, where the account connected
    /// most recently under that name answers. Its user ID, Ember ID,
    /// username and when it connected.
    fn find(
        self,
        tx: &Transaction<'_>,
        key: &str,
    ) -> Result<Option<(String, String, String, u64)>> {
        let (sql, key) = match self {
            Self::Discord if !is_user_id(key) => return Ok(None),
            Self::Discord => (
                "SELECT user_id, ember_id, username, connected_at FROM discord_accounts WHERE user_id = ?1",
                key.to_owned(),
            ),
            Self::Challonge => (
                "SELECT user_id, ember_id, username, connected_at FROM challonge_accounts WHERE username_key = ?1
                 ORDER BY connected_at DESC, user_id LIMIT 1",
                username_key(key),
            ),
        };
        Ok(tx
            .query_row(sql, [key], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
            })
            .optional()?)
    }

    /// The players whose Ember IDs are connected to the accounts `keys` name
    /// and linked on `connection_id`, so its platform can create matches for
    /// them, in order and once per Ember ID. An account `link` cannot link is
    /// left out.
    pub fn find_and_link<'a>(
        self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        connection_id: &str,
        keys: &'a [String],
    ) -> Result<Vec<Found<'a>>> {
        let mut found: Vec<Found<'a>> = Vec::new();
        for asked in keys {
            let Some((user_id, ember_id, username, connected_at)) = self.find(tx, asked)? else {
                continue;
            };
            let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
            if found.iter().any(|player| player.ember_id == ember_id) {
                continue;
            }
            let Some(participant_id) =
                self.link(tx, ctx, connection_id, &user_id, &username, &ember_id)?
            else {
                continue;
            };
            found.push(Found {
                asked,
                user_id,
                connected_at,
                ember_id,
                participant_id,
            });
        }
        Ok(found)
    }

    /// Links account `user_id`'s subject on the connection to `ember_id`,
    /// approved by the player's sign-in, and gives the participant `ember_id`
    /// is linked as there now, if it is. A link the player removed since that
    /// sign-in is not made again. A link from a code the player claimed, or
    /// from another site's sign-in, is kept: on the Ember ID, which then
    /// needs no other, or on the subject (a provider can name its accounts
    /// `<site>:<user ID>` too), which leaves this Ember ID unlinked.
    fn link(
        self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        connection_id: &str,
        user_id: &str,
        username: &str,
        ember_id: &EmberId,
    ) -> Result<Option<String>> {
        let subject = self.subject(user_id);
        let linked: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL
               AND (approved_via != ?4 OR claim_id = ?3))",
            params![ember_id.as_str(), connection_id, subject, self.name()],
            |row| row.get(0),
        )?;
        if !linked && !self.make_link(tx, ctx, connection_id, user_id, username, ember_id)? {
            return Ok(None);
        }
        // The participant the Ember ID is linked as there: this sign-in's
        // account, or the one a code or another sign-in linked it as.
        Ok(Some(tx.query_row(
            "SELECT a.participant_id FROM links l JOIN external_accounts a ON a.id = l.account_id
             WHERE l.ember_id = ?1 AND l.connection_id = ?2 AND l.revoked_at IS NULL",
            params![ember_id.as_str(), connection_id],
            |row| row.get(0),
        )?))
    }

    /// Makes the link `link` describes, unless the player withdrew it or the
    /// subject is linked to someone already.
    fn make_link(
        self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        connection_id: &str,
        user_id: &str,
        username: &str,
        ember_id: &EmberId,
    ) -> Result<bool> {
        let withdrawn: bool = tx.query_row(
            &format!(
                "SELECT EXISTS (SELECT 1 FROM {} WHERE user_id = ?1 AND connection_id = ?2)",
                self.withdrawals()
            ),
            params![user_id, connection_id],
            |row| row.get(0),
        )?;
        if withdrawn {
            return Ok(false);
        }
        let subject = self.subject(user_id);
        let tenant = ctx.tenant_of(connection_id)?;
        let (account_id, participant_id) = account(tx, connection_id, &subject, username, ctx.now)?;
        let taken: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM links WHERE account_id = ?1 AND revoked_at IS NULL)",
            [&account_id],
            |row| row.get(0),
        )?;
        if taken {
            return Ok(false);
        }
        let link_id = new_id("lnk");
        tx.execute(
            "INSERT INTO links (id, account_id, connection_id, ember_id, approved_via, claim_id, consented_at, approved_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?7)",
            params![link_id, account_id, connection_id, ember_id.as_str(), self.name(), subject, ctx.now],
        )?;
        emit(
            tx,
            &ctx.config,
            ctx.now,
            NewEvent {
                kind: Kind::LinkCompleted,
                tenant_id: &tenant,
                connection_id: Some(connection_id),
                subject: format!("links/{link_id}"),
                match_id: None,
                ember_id: Some(ember_id),
                lobby_id: None,
                tournament_id: None,
                data: json!({
                    "link_id": link_id,
                    "ember_id": ember_id,
                    "connection_id": connection_id,
                    "participant_id": participant_id,
                    "approved_via": self.name(),
                }),
            },
        )?;
        Ok(true)
    }

    /// Ends every link this account's sign-in approved, on every connection,
    /// when the account moves to another Ember ID or is disconnected.
    fn end_links(self, tx: &Transaction<'_>, ctx: &Ctx, user_id: &str, reason: &str) -> Result<()> {
        let links: Vec<(String, String, String, String, String)> = tx
            .prepare(
                "SELECT l.id, l.ember_id, l.connection_id, a.participant_id, c.tenant_id
                 FROM links l
                 JOIN external_accounts a ON a.id = l.account_id
                 JOIN provider_connections c ON c.id = l.connection_id
                 WHERE l.approved_via = ?1 AND l.claim_id = ?2 AND l.revoked_at IS NULL",
            )?
            .query_map(params![self.name(), self.subject(user_id)], |row| {
                Ok((
                    row.get(0)?,
                    row.get(1)?,
                    row.get(2)?,
                    row.get(3)?,
                    row.get(4)?,
                ))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (link_id, ember_id, connection_id, participant_id, tenant) in links {
            let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
            revoke_link(
                tx,
                ctx,
                &tenant,
                &link_id,
                &ember_id,
                &connection_id,
                &participant_id,
                reason,
            )?;
        }
        Ok(())
    }
}

/// The player (or the platform account's holder) removed `link_id`. If a
/// site's sign-in approved it and that account is still connected, the
/// consent is withdrawn on that connection until the account's row is
/// replaced by a new sign-in, and the player's sign-ins in flight on that
/// site end, so only one started from now on gives it again.
pub fn withdraw(tx: &Transaction<'_>, link_id: &str) -> Result<()> {
    let link: Option<(String, String, String, String)> = tx
        .query_row(
            "SELECT approved_via, claim_id, connection_id, ember_id FROM links WHERE id = ?1",
            [link_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .optional()?;
    let Some((approved_via, claim, connection_id, ember_id)) = link else {
        return Ok(());
    };
    let Some(site) = Site::of(&approved_via) else {
        return Ok(());
    };
    let user_id = claim
        .strip_prefix(site.name())
        .and_then(|rest| rest.strip_prefix(':'))
        .unwrap_or_default();
    tx.execute(
        &format!(
            "INSERT OR IGNORE INTO {} (user_id, connection_id)
             SELECT user_id, ?2 FROM {} WHERE user_id = ?1 AND ember_id = ?3",
            site.withdrawals(),
            site.accounts()
        ),
        params![user_id, connection_id, ember_id],
    )?;
    sign_ins::end(tx, site, &ember_id)
}
