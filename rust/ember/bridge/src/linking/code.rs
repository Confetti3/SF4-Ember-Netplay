//! Link codes: generation, normalization and the keyed hash that is stored
//! in place of the code.
use ember_protocol::api::ErrorCode;

use crate::{
    Keys,
    error::{ApiFailure, Result},
    util::random,
};

pub const CODE_LIFETIME_SECS: u64 = 5 * 60;
const CODE_ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const CODE_LENGTH: usize = 10;
const MAX_SUBJECT: usize = 256;

/// The same answer for unknown, expired, used and foreign codes, so codes
/// cannot be probed through error text (spec 25.2).
pub(super) fn code_rejected() -> ApiFailure {
    ApiFailure::new(
        ErrorCode::LinkExpired,
        "That code is not valid. Ask for a new code and try again.",
    )
}

pub(super) fn generate_code() -> String {
    let bytes: [u8; 8] = random();
    let bits = u64::from_be_bytes(bytes);
    (0..CODE_LENGTH)
        .map(|index| CODE_ALPHABET[((bits >> (index * 5)) & 31) as usize] as char)
        .collect()
}

/// Accepts the ten symbols, in either case, with at most the single display
/// separator after the fifth. Nothing else is normalized.
pub(super) fn normalize_code(input: &str) -> Option<String> {
    let compact = match input.len() {
        CODE_LENGTH => input.to_owned(),
        11 if input.as_bytes()[5] == b'-' => format!("{}{}", &input[..5], &input[6..]),
        _ => return None,
    };
    let upper = compact.to_ascii_uppercase();
    upper
        .bytes()
        .all(|byte| CODE_ALPHABET.contains(&byte))
        .then_some(upper)
}

pub(super) fn display_code(code: &str) -> String {
    format!("{}-{}", &code[..5], &code[5..])
}

pub(super) fn code_hmac(keys: &Keys, normalized: &str) -> [u8; 32] {
    keys.keyed_hash("link-code", normalized.as_bytes())
}

pub fn check_subject(subject: &str) -> Result<()> {
    if subject.is_empty() || subject.len() > MAX_SUBJECT || subject.chars().any(char::is_control) {
        return Err(ApiFailure::invalid("The external subject is invalid."));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_use_crockford_symbols() {
        for _ in 0..200 {
            let code = generate_code();
            assert_eq!(code.len(), 10);
            assert_eq!(normalize_code(&code).as_deref(), Some(code.as_str()));
            let shown = display_code(&code);
            assert_eq!(normalize_code(&shown).as_deref(), Some(code.as_str()));
            assert_eq!(
                normalize_code(&shown.to_lowercase()).as_deref(),
                Some(code.as_str())
            );
        }
        for bad in [
            "ABCDE FGHJK",
            "ABCDEFGHJ",
            "ABCDE--FGHJK",
            "ABCDEFGHJI",
            "ABCDO-FGHJK",
            "ABC-DEFGHJK",
            " ABCDEFGHJK",
        ] {
            assert_eq!(normalize_code(bad), None, "{bad}");
        }
    }
}
