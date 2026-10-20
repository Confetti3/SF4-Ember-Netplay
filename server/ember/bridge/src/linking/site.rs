//! Links a platform gets from a player's sign-in instead of from a link
//! code. The player connected an account to their Ember ID on Discord or on
//! a configured sign-in site (`routes::sign_in`); a platform that finds
//! players by that account (`find_and_link`) then gets a link approved by the
//! sign-in: `approved_via` `discord` with claim `discord:<user ID>`, or
//! `approved_via` `site` with claim `<site>:<user ID>`.
//!
//! Such a link stands only while that account is connected to that Ember ID:
//! moving or disconnecting the account ends the links its sign-in approved,
//! in the same transaction, and the next lookup links the account again
//! where it now belongs. A link the player (or the platform account's holder)
//! removes stays removed (`withdraw`): no account the player has connected
//! makes it again until the player completes a sign-in started after that,
//! which gives that account's consent back.
use ember_protocol::{EmberId, discord, event::Kind, sign_in::Account, sites::LookupBy};
use rusqlite::{OptionalExtension, ToSql, Transaction, named_params, params};
use serde_json::json;

use super::{account, revoke_link, sign_ins};
use crate::{
    ctx::Ctx,
    error::{ApiFailure, Result},
    events::{NewEvent, emit},
    util::new_id,
};

/// Where a sign-in's accounts live: Discord's own tables, or a configured
/// site's rows in the site tables (`site_accounts`, `site_sign_ins`,
/// `site_withdrawals`), which every configured site shares by its ID.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Site {
    Discord,
    /// A configured sign-in site, by its ID (`sites::is_site_id`).
    Configured(String),
}

/// An account a lookup asks for: on `site`, the one `key` names by `by`,
/// as the lookup named it (`asked`) and as stored (`key`: for a username,
/// the site's `username_key` form).
pub struct Ask {
    pub site: Site,
    pub by: LookupBy,
    pub asked: String,
    pub key: String,
}

impl Ask {
    /// A Discord account, by its user ID.
    pub fn discord(user_id: &str) -> Self {
        Self {
            site: Site::Discord,
            by: LookupBy::UserId,
            asked: user_id.to_owned(),
            key: user_id.to_owned(),
        }
    }
}

/// An account a lookup found and linked on the connection.
pub struct Found {
    pub ask: Ask,
    pub user_id: String,
    /// When the account connected.
    pub connected_at: u64,
    pub ember_id: EmberId,
    pub participant_id: String,
}

/// The tables of one kind of row, Discord's and the configured sites'.
#[derive(Clone, Copy)]
pub(super) struct Tables {
    pub discord: &'static str,
    pub sites: &'static str,
}

const ACCOUNTS: Tables = Tables {
    discord: "discord_accounts",
    sites: "site_accounts",
};
const WITHDRAWALS: Tables = Tables {
    discord: "discord_withdrawals",
    sites: "site_withdrawals",
};

impl Site {
    /// The site's ID: `discord`, or the configured site's.
    pub fn id(&self) -> &str {
        match self {
            Self::Discord => "discord",
            Self::Configured(id) => id,
        }
    }

    /// `links.approved_via` of the links its sign-in approves.
    fn approved_via(&self) -> &'static str {
        match self {
            Self::Discord => "discord",
            Self::Configured(_) => "site",
        }
    }

    /// The subject and claim of account `user_id`: `<site>:<user ID>`.
    fn subject(&self, user_id: &str) -> String {
        format!("{}:{user_id}", self.id())
    }

    /// The table of `tables` holding this site's rows, and what a
    /// statement's `WHERE` starts with to pick them out (`WHERE
    /// {scope}user_id = ...`), with its parameters passed through `bind`.
    /// Discord's tables hold only Discord's rows and have no `site` column,
    /// so nothing; the site tables hold every configured site's, so
    /// `site = :site AND `.
    pub(super) fn scope(&self, tables: Tables) -> (&'static str, &'static str) {
        match self {
            Self::Discord => (tables.discord, ""),
            Self::Configured(_) => (tables.sites, "site = :site AND "),
        }
    }

    /// A `scope`d statement's named parameters: `params`, and `:site` for a
    /// configured site.
    pub(super) fn bind<'a>(
        &'a self,
        params: &[(&'a str, &'a dyn ToSql)],
    ) -> Vec<(&'a str, &'a dyn ToSql)> {
        let mut all = params.to_vec();
        if let Self::Configured(id) = self {
            all.push((":site", id as &dyn ToSql));
        }
        all
    }

    /// The Ember ID's connected account, if any.
    pub fn account(&self, tx: &Transaction<'_>, ember_id: &str) -> Result<Option<Account>> {
        let (table, scope) = self.scope(ACCOUNTS);
        Ok(tx
            .query_row(
                &format!(
                    "SELECT user_id, username, connected_at FROM {table} WHERE {scope}ember_id = :ember_id"
                ),
                &*self.bind(named_params! { ":ember_id": ember_id }),
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
    pub fn owner(&self, tx: &Transaction<'_>, user_id: &str) -> Result<Option<String>> {
        let (table, scope) = self.scope(ACCOUNTS);
        Ok(tx
            .query_row(
                &format!("SELECT ember_id FROM {table} WHERE {scope}user_id = :user_id"),
                &*self.bind(named_params! { ":user_id": user_id }),
                |row| row.get(0),
            )
            .optional()?)
    }

    /// Connects account `user_id`, called `username`, to `ember_id`. The
    /// latest sign-in wins: this account leaves any other Ember ID and this
    /// Ember ID leaves any other account, each ending the links its sign-in
    /// approved. The new row is fresh consent, since the withdrawals of the
    /// old one went with it. A configured site's account whose lookups
    /// name usernames is also stored under `username_key`, the form they
    /// match; other accounts have none.
    pub fn connect(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        ember_id: &str,
        user_id: &str,
        username: &str,
        username_key: Option<&str>,
    ) -> Result<()> {
        self.release(tx, ctx, ember_id, user_id)?;
        match self {
            Self::Discord => tx.execute(
                "INSERT INTO discord_accounts (user_id, ember_id, username, connected_at) VALUES (?1, ?2, ?3, ?4)",
                params![user_id, ember_id, username, ctx.now],
            )?,
            Self::Configured(site) => {
                // A site may let an account rename, and a name it gave up
                // can be taken by another. An older account of this site
                // stored under this name answers for it no more; it answers
                // again for the name it next connects with.
                tx.execute(
                    "UPDATE site_accounts SET username_key = NULL
                     WHERE site = ?1 AND username_key = ?2 AND user_id != ?3",
                    params![site, username_key, user_id],
                )?;
                tx.execute(
                    "INSERT INTO site_accounts (site, user_id, ember_id, username, username_key, connected_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                    params![site, user_id, ember_id, username, username_key, ctx.now],
                )?
            }
        };
        Ok(())
    }

    fn release(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        ember_id: &str,
        user_id: &str,
    ) -> Result<()> {
        let (table, scope) = self.scope(ACCOUNTS);
        let replaced: Vec<(String, String)> = tx
            .prepare(&format!(
                "DELETE FROM {table} WHERE {scope}(user_id = :user_id OR ember_id = :ember_id)
                 RETURNING user_id, ember_id"
            ))?
            .query_map(
                &*self.bind(named_params! { ":user_id": user_id, ":ember_id": ember_id }),
                |row| Ok((row.get(0)?, row.get(1)?)),
            )?
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
    pub fn disconnect(&self, tx: &Transaction<'_>, ctx: &Ctx, ember_id: &str) -> Result<()> {
        sign_ins::end(tx, self, ember_id)?;
        let (table, scope) = self.scope(ACCOUNTS);
        let removed: Option<String> = tx
            .query_row(
                &format!("DELETE FROM {table} WHERE {scope}ember_id = :ember_id RETURNING user_id"),
                &*self.bind(named_params! { ":ember_id": ember_id }),
                |row| row.get(0),
            )
            .optional()?;
        match removed {
            Some(user_id) => self.end_links(
                tx,
                ctx,
                &user_id,
                &format!("{}_disconnected", self.approved_via()),
            ),
            None => Ok(()),
        }
    }

    /// The account connected for a lookup key, by `by`: a user ID exactly
    /// (Discord's only if it is one, so the emails BluMint also sends find
    /// nothing), or a `username_key`, which at most one account of a site
    /// answers for (`connect`, and the schema's unique index). Its user ID,
    /// Ember ID, username and when it connected.
    fn find(
        &self,
        tx: &Transaction<'_>,
        key: &str,
        by: LookupBy,
    ) -> Result<Option<(String, String, String, u64)>> {
        if *self == Self::Discord && !discord::is_user_id(key) {
            return Ok(None);
        }
        let column = match by {
            LookupBy::UserId => "user_id",
            LookupBy::Username => "username_key",
        };
        let (table, scope) = self.scope(ACCOUNTS);
        Ok(tx
            .query_row(
                &format!(
                    "SELECT user_id, ember_id, username, connected_at FROM {table}
                     WHERE {scope}{column} = :key"
                ),
                &*self.bind(named_params! { ":key": key }),
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?)
    }

    /// Links account `user_id`'s subject on the connection to `ember_id`,
    /// approved by the player's sign-in, and gives the participant `ember_id`
    /// is linked as there now, if it is. A link the player removed since that
    /// sign-in is not made again. A link from a code the player claimed, or
    /// from another site's sign-in, is kept: on the Ember ID, which then
    /// needs no other, or on the subject (a provider can name its accounts
    /// `<site>:<user ID>` too), which leaves this Ember ID unlinked.
    fn link(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        connection_id: &str,
        user_id: &str,
        username: &str,
        ember_id: &EmberId,
    ) -> Result<Option<String>> {
        let subject = self.subject(user_id);
        // Linked unless the only link is another account's of this same
        // site, which `connect` ended already.
        let linked: bool = tx.query_row(
            "SELECT EXISTS (SELECT 1 FROM links WHERE ember_id = ?1 AND connection_id = ?2 AND revoked_at IS NULL
               AND NOT (approved_via = ?4 AND substr(claim_id, 1, length(?5)) = ?5 AND claim_id != ?3))",
            params![
                ember_id.as_str(),
                connection_id,
                subject,
                self.approved_via(),
                format!("{}:", self.id())
            ],
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
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        connection_id: &str,
        user_id: &str,
        username: &str,
        ember_id: &EmberId,
    ) -> Result<bool> {
        let (withdrawals, scope) = self.scope(WITHDRAWALS);
        let withdrawn: bool = tx.query_row(
            &format!(
                "SELECT EXISTS (SELECT 1 FROM {withdrawals}
                   WHERE {scope}user_id = :user_id AND connection_id = :connection_id)"
            ),
            &*self.bind(named_params! { ":user_id": user_id, ":connection_id": connection_id }),
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
            params![link_id, account_id, connection_id, ember_id.as_str(), self.approved_via(), subject, ctx.now],
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
                    "approved_via": self.approved_via(),
                }),
            },
        )?;
        Ok(true)
    }

    /// Ends every link this account's sign-in approved, on every connection,
    /// when the account moves to another Ember ID or is disconnected.
    fn end_links(
        &self,
        tx: &Transaction<'_>,
        ctx: &Ctx,
        user_id: &str,
        reason: &str,
    ) -> Result<()> {
        let links: Vec<(String, String, String, String, String)> = tx
            .prepare(
                "SELECT l.id, l.ember_id, l.connection_id, a.participant_id, c.tenant_id
                 FROM links l
                 JOIN external_accounts a ON a.id = l.account_id
                 JOIN provider_connections c ON c.id = l.connection_id
                 WHERE l.approved_via = ?1 AND l.claim_id = ?2 AND l.revoked_at IS NULL",
            )?
            .query_map(params![self.approved_via(), self.subject(user_id)], |row| {
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

/// Whether any account is connected on the configured site `id`.
pub fn site_has_accounts(tx: &Transaction<'_>, id: &str) -> Result<bool> {
    Ok(tx.query_row(
        "SELECT EXISTS (SELECT 1 FROM site_accounts WHERE site = ?1)",
        [id],
        |row| row.get(0),
    )?)
}

/// The accounts `asks` name whose Ember IDs are linked on
/// `connection_id` now, so its platform can create matches for them, in
/// order and once per site and Ember ID. An account `link` cannot link
/// is left out.
pub fn find_and_link(
    tx: &Transaction<'_>,
    ctx: &Ctx,
    connection_id: &str,
    asks: Vec<Ask>,
) -> Result<Vec<Found>> {
    let mut found: Vec<Found> = Vec::new();
    for ask in asks {
        let Some((user_id, ember_id, username, connected_at)) =
            ask.site.find(tx, &ask.key, ask.by)?
        else {
            continue;
        };
        let ember_id = EmberId::parse(&ember_id).map_err(|_| ApiFailure::unavailable())?;
        if found
            .iter()
            .any(|player| player.ask.site == ask.site && player.ember_id == ember_id)
        {
            continue;
        }
        let Some(participant_id) =
            ask.site
                .link(tx, ctx, connection_id, &user_id, &username, &ember_id)?
        else {
            continue;
        };
        found.push(Found {
            ask,
            user_id,
            connected_at,
            ember_id,
            participant_id,
        });
    }
    Ok(found)
}

/// The player (or the platform account's holder) removed `link_id`. If a
/// sign-in approved it, the consent of every account the Ember ID has
/// connected (Discord's and each site's) is withdrawn on that connection, so
/// no other account links the player there again either. Each stays
/// withdrawn until that account's row is replaced by a new sign-in; the
/// player's sign-ins in flight end, so only one started from now on gives
/// consent back.
pub fn withdraw(tx: &Transaction<'_>, link_id: &str) -> Result<()> {
    let link: Option<(String, String, String)> = tx
        .query_row(
            "SELECT approved_via, connection_id, ember_id FROM links WHERE id = ?1",
            [link_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    let Some((approved_via, connection_id, ember_id)) = link else {
        return Ok(());
    };
    if !matches!(approved_via.as_str(), "discord" | "site") {
        return Ok(());
    }
    tx.execute(
        "INSERT OR IGNORE INTO discord_withdrawals (user_id, connection_id)
         SELECT user_id, ?1 FROM discord_accounts WHERE ember_id = ?2",
        params![connection_id, ember_id],
    )?;
    tx.execute(
        "INSERT OR IGNORE INTO site_withdrawals (site, user_id, connection_id)
         SELECT site, user_id, ?1 FROM site_accounts WHERE ember_id = ?2",
        params![connection_id, ember_id],
    )?;
    sign_ins::end_everywhere(tx, &ember_id)
}
