use std::fmt;

/// A protocol rule that an input broke. Messages name the rule, never the
/// rejected value, so they are safe to log and to return to a caller.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// The body is larger than the limit for its route.
    TooLarge,
    /// Not strict JSON: duplicate key, fraction or exponent, unsafe integer,
    /// excessive nesting, invalid UTF-8 or trailing data.
    InvalidJson(String),
    /// Well-formed JSON that does not match the object's schema.
    InvalidField(&'static str),
    /// A base64url, base64 or decimal value that is not in canonical form.
    InvalidEncoding(&'static str),
    /// A public key that is not a usable Ed25519 point.
    InvalidKey,
    /// A signature that does not verify for the stated key and domain.
    InvalidSignature,
    /// A challenge or webhook outside its time window.
    Expired,
    /// A signed object whose fields disagree with each other or with the
    /// operation the verifier expected.
    Mismatch(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooLarge => f.write_str("body is too large"),
            Self::InvalidJson(reason) => write!(f, "invalid JSON: {reason}"),
            Self::InvalidField(field) => write!(f, "invalid field: {field}"),
            Self::InvalidEncoding(field) => write!(f, "noncanonical encoding: {field}"),
            Self::InvalidKey => f.write_str("invalid public key"),
            Self::InvalidSignature => f.write_str("invalid signature"),
            Self::Expired => f.write_str("outside the allowed time window"),
            Self::Mismatch(what) => write!(f, "mismatch: {what}"),
        }
    }
}

impl std::error::Error for Error {}
