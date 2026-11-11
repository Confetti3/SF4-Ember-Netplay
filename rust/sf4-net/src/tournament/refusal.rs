//! A bridge's refusal of a request, read once from its answer. Decisions
//! match on the variant; the text the game reads is made only by `text`.
use std::borrow::Cow;

use ember_protocol::api::{ApiError, ErrorCode};

/// What a bridge refused with.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// The word a route's table gives a `details.reason` (possibly
    /// `<reason>_<cause>`), which a caller words apart from the bridge's code.
    Reason(Box<str>),
    /// The bridge's error code.
    Error(ErrorCode),
    /// An answer that names neither: only its HTTP status.
    Status(u16),
}

impl Refusal {
    /// The refusal in a bridge's answer: its `details.reason` when that is one
    /// of `reasons`, else its error code, or the status when the body names
    /// none. A listed reason with a `details.cause` is `<reason>_<cause>` when
    /// `reasons` lists that too; a reason that is not listed never takes a
    /// cause, so `room` and `full` are not `room_full`.
    pub(super) fn read(status: u16, body: &[u8], reasons: &[&str]) -> Self {
        let Ok(ApiError { error }) = serde_json::from_slice::<ApiError>(body) else {
            return Self::Status(status);
        };
        match reason_of(&error.details, reasons) {
            Some(reason) => Self::Reason(reason),
            None => Self::Error(error.code),
        }
    }

    /// The text the game reads: the reason, the code, or `http_<status>`.
    pub fn text(&self) -> Cow<'_, str> {
        match self {
            Self::Reason(reason) => Cow::Borrowed(reason),
            Self::Error(code) => Cow::Borrowed(code.as_str()),
            Self::Status(status) => Cow::Owned(format!("http_{status}")),
        }
    }

    /// Whether the bridge refused with this error code.
    pub fn is(&self, code: ErrorCode) -> bool {
        matches!(self, Self::Error(refused) if *refused == code)
    }

    /// Whether this is the reason `word`, one a caller worded apart.
    pub fn is_reason(&self, word: &str) -> bool {
        matches!(self, Self::Reason(reason) if &**reason == word)
    }

    /// Whether this is one of the `reasons` of a route's table.
    pub(super) fn is_listed(&self, reasons: &[&str]) -> bool {
        matches!(self, Self::Reason(reason) if reasons.contains(&&**reason))
    }

    /// Whether the bridge answered with a server failure status and named no
    /// code: it is failing, not refusing this request.
    pub fn is_server_failure(&self) -> bool {
        matches!(self, Self::Status(500..=599))
    }
}

/// The reason in an error's `details` that a caller words, if any.
fn reason_of(
    details: &serde_json::Map<String, serde_json::Value>,
    reasons: &[&str],
) -> Option<Box<str>> {
    let reason = details.get("reason")?.as_str()?;
    let cause = details.get("cause").and_then(|cause| cause.as_str());
    if let (true, Some(cause)) = (reasons.contains(&reason), cause) {
        let caused = format!("{reason}_{cause}");
        if reasons.contains(&caused.as_str()) {
            return Some(caused.into());
        }
    }
    // Tournament Mode by invitation is the same answer wherever an
    // organizer is turned away, so the game always words it.
    (reasons.contains(&reason) || reason == "invite_required").then(|| reason.into())
}
