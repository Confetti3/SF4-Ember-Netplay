//! The pages a sign-in shows in the player's browser: what it connected, or
//! the question it asks before connecting or moving an account, naming the
//! sites that find the player by it.
use axum::response::Response;
use ember_protocol::EmberId;

use super::provider::{Routes, label};
use crate::{
    config::{Config, Disputes},
    http::page,
    linking::Site,
    util::html,
};

fn fingerprint(ember_id: &str) -> String {
    EmberId::parse(ember_id)
        .map(|id| id.fingerprint())
        .unwrap_or_default()
}

/// What one site's sign-in pages say: its name, where their answers go, and
/// the platforms on this bridge that find players by its accounts, named as
/// each tenant names itself, for the pages to tell the player who finds them.
pub(super) struct Pages {
    label: &'static str,
    callback: &'static str,
    /// Each platform's name, already escaped for the page, and whether it starts
    /// a match that ended with a player's links again from the games already
    /// won (`Disputes::Restart`) rather than hand it to an organizer.
    named: Vec<(String, bool)>,
}

impl Pages {
    pub(super) fn of(config: &Config, site: Site) -> Self {
        let mut named: Vec<(String, bool)> = Vec::new();
        for tenant in &config.tenants {
            for connection in &tenant.connections {
                let platform = (
                    html(&tenant.name),
                    connection.disputes() == Disputes::Restart,
                );
                if connection.enabled && connection.finds_by(site) && !named.contains(&platform) {
                    named.push(platform);
                }
            }
        }
        Self {
            label: label(site),
            callback: Routes::of(site).callback,
            named,
        }
    }

    /// The sites whose `restarts` is as asked, as "A", "A and B" or "A, B and
    /// C", or None when there are none.
    fn listed(&self, restarts: Option<bool>) -> Option<String> {
        let mut names: Vec<&str> = Vec::new();
        for (name, restart) in &self.named {
            if restarts.is_none_or(|wanted| wanted == *restart) && !names.contains(&name.as_str()) {
                names.push(name);
            }
        }
        let (last, rest) = names.split_last()?;
        Some(match rest {
            [] => (*last).to_owned(),
            _ => format!("{} and {last}", rest.join(", ")),
        })
    }

    /// Who finds the player by their account.
    fn finders(&self) -> String {
        self.listed(None)
            .unwrap_or_else(|| "Tournament sites that use this Ember service".into())
    }

    /// Where the player goes back to.
    fn back(&self) -> String {
        match &self.named[..] {
            [(name, _)] => name.clone(),
            _ => "the tournament site you came from".into(),
        }
    }

    /// What happens to a tournament match the player's links ended, and to
    /// its game in progress. A restarting site's match ends, the game with
    /// it; a reviewing site's may instead wait for an organizer, who may
    /// count the game (`matches::on_unlink`).
    fn after_ending(&self) -> String {
        let restarting = self.listed(Some(true)).map(|names| {
            format!(
                " At {names}, the match ends and a game in progress does not count. \
                 {names} will start the match again from the games already won."
            )
        });
        let reviewing = self.listed(Some(false)).map(|names| {
            format!(
                " At {names}, the match may wait for an organizer, who decides how it goes on, \
                 a game in progress included."
            )
        });
        restarting.into_iter().chain(reviewing).collect()
    }

    pub(super) fn connected(&self, username: &str, ember_id: &str) -> Response {
        let label = self.label;
        finished(
            &format!("{label} connected"),
            &format!(
                "Your {label} account <strong>{}</strong> is connected to your Ember ID <strong>{}</strong>. \
                 {} can now find you by this {label} account. \
                 You can close this tab and go back to {}. Ember shows the connection too.",
                html(username),
                html(&fingerprint(ember_id)),
                self.finders(),
                self.back()
            ),
        )
    }

    /// What connecting an account to `to` does to the account `to` has now,
    /// if any: it is disconnected, and its links end as a moved account's
    /// do, with `after` saying what then happens to their matches.
    fn replacing(&self, to: &str, replaced: Option<&str>, after: &str) -> String {
        replaced.map_or_else(String::new, |replaced| {
            format!(
                "<p>Ember ID <strong>{to}</strong> is connected to {} account <strong>{}</strong> now. \
                 Connecting this one disconnects it, and its tournament links end too, \
                 which stops any tournament match you are playing through them.{after}</p>",
                self.label,
                html(replaced)
            )
        })
    }

    /// Asks before moving the account from Ember ID `from` to `to`. The form
    /// carries the question's confirmation secret, which only this page has.
    pub(super) fn moving(
        &self,
        confirmation: &str,
        username: &str,
        from: &str,
        to: &str,
        replaced: Option<&str>,
    ) -> Response {
        let (label, callback) = (self.label, self.callback);
        let (username, from, to) = (
            html(username),
            html(&fingerprint(from)),
            html(&fingerprint(to)),
        );
        let (finders, after_ending) = (self.finders(), self.after_ending());
        let replacing = self.replacing(&to, replaced, "");
        page(
            &format!("Move this {label} account?"),
            &format!(
                "<h1>Move this {label} account?</h1>\
                 <p>Your {label} account <strong>{username}</strong> is connected to another Ember ID, <strong>{from}</strong>. \
                 Moving it connects it to Ember ID <strong>{to}</strong> instead.</p>\
                 <div class=\"box warn\"><p>{finders} will then find <strong>{to}</strong> by this {label} account, \
                 and your tournament links on <strong>{from}</strong> end. \
                 That stops any tournament match you are playing on <strong>{from}</strong>.{after_ending}</p>{replacing}\
                 <p>Move it only if both Ember IDs are yours, for example on a new PC.</p></div>\
                 <form method=\"post\" action=\"{callback}\"><input type=\"hidden\" name=\"confirmation\" value=\"{}\">\
                 <button name=\"choice\" value=\"move\">Move it to {to}</button>\
                 <button name=\"choice\" value=\"keep\">Keep it on {from}</button></form>",
                html(confirmation)
            ),
            false,
        )
    }

    /// Asks before connecting an account no Ember ID has to `to`, naming it
    /// so the player can compare it with the one their Ember shows. The form
    /// carries the question's confirmation secret, which only this page has.
    pub(super) fn connecting(
        &self,
        confirmation: &str,
        username: &str,
        to: &str,
        replaced: Option<&str>,
    ) -> Response {
        let (label, callback) = (self.label, self.callback);
        let (username, to) = (html(username), html(&fingerprint(to)));
        let finders = self.finders();
        let replacing = self.replacing(&to, replaced, &self.after_ending());
        page(
            &format!("Connect this {label} account?"),
            &format!(
                "<h1>Connect this {label} account?</h1>\
                 <p>This connects your {label} account <strong>{username}</strong> to Ember ID <strong>{to}</strong>. \
                 {finders} can then find you by this {label} account.</p>\
                 {replacing}<div class=\"box warn\"><p>Check that <strong>{to}</strong> is the Ember ID your Ember shows. \
                 If you did not choose Connect {label} in Ember just now, someone else may have sent you this link, \
                 so choose This wasn't me.</p></div>\
                 <form method=\"post\" action=\"{callback}\"><input type=\"hidden\" name=\"confirmation\" value=\"{}\">\
                 <button name=\"choice\" value=\"connect\">Connect it to {to}</button>\
                 <button name=\"choice\" value=\"refuse\">This wasn't me</button></form>",
                html(confirmation)
            ),
            false,
        )
    }

    pub(super) fn off(&self) -> Response {
        let label = self.label;
        finished(
            &format!("{label} sign-in is off"),
            &format!(
                "This Ember service does not offer {label} sign-in, so you cannot connect {label} here."
            ),
        )
    }

    pub(super) fn not_saved(&self) -> Response {
        finished(
            "Not connected",
            &format!(
                "Ember could not save your {} account. Try again later.",
                self.label
            ),
        )
    }

    pub(super) fn not_waiting(&self) -> Response {
        let label = self.label;
        finished(
            "Sign-in expired",
            &format!(
                "Ember is not waiting for this {label} sign-in, or it took longer than ten minutes. \
                 Choose Connect {label} in Ember again."
            ),
        )
    }

    /// The site refused the code, or Ember never got to ask it.
    pub(super) fn not_connected(&self) -> Response {
        let label = self.label;
        finished(
            "Not connected",
            &format!(
                "{label} did not connect your account. Choose Connect {label} in Ember to try again."
            ),
        )
    }

    pub(super) fn no_answer(&self) -> Response {
        let label = self.label;
        finished(
            &format!("{label} did not answer"),
            &format!(
                "{label} did not confirm your account. Choose Connect {label} in Ember to try again."
            ),
        )
    }

    /// The month's requests to the site are spent (`budget`).
    pub(super) fn busy(&self) -> Response {
        let label = self.label;
        finished(
            "Not connected",
            &format!(
                "Ember cannot check with {label} right now. Choose Connect {label} in Ember to try again later."
            ),
        )
    }

    /// The account was connected or moved elsewhere after the page asked.
    pub(super) fn changed(&self) -> Response {
        let label = self.label;
        finished(
            "Not connected",
            &format!(
                "Your {label} account was connected to an Ember ID after this page asked, so nothing changed. \
                 To connect it here, choose Connect {label} in Ember again."
            ),
        )
    }

    /// The player said the sign-in was not theirs.
    pub(super) fn refused(&self, username: &str) -> Response {
        let label = self.label;
        finished(
            "Not connected",
            &format!(
                "Your {label} account <strong>{}</strong> was not connected to any Ember ID. \
                 If you did not choose Connect {label} in Ember, someone may have sent you that link, \
                 so do not sign in from it again. You can close this tab.",
                html(username)
            ),
        )
    }

    /// The player kept the account where it was.
    pub(super) fn kept(&self, username: &str) -> Response {
        finished(
            "Nothing moved",
            &format!(
                "Your {} account <strong>{}</strong> stays connected to your other Ember ID. \
                 You can close this tab.",
                self.label,
                html(username)
            ),
        )
    }
}

fn finished(title: &str, body: &str) -> Response {
    page(
        title,
        &format!("<h1>{}</h1><p>{body}</p>", html(title)),
        false,
    )
}
