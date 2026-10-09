/// Why a report was refused. `as_str` is the `reason` sent to the client, so
/// these strings are part of the launcher contract. Diagnostic detail, such as
/// a storage error, stays with the caller and never reaches the response.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    // Admission
    MissingPeer,
    RateLimit,
    QueueFull,
    QueueTimeout,
    QueueClosed,
    // Upload
    BodyTooLarge,
    InvalidBody,
    InvalidMultipart,
    UploadTimeout,
    // Parts
    MissingPartName,
    MissingLogFilename,
    UnknownLogFilename,
    DuplicateLog,
    DuplicateOrInvalidPart,
    UnknownPart,
    PartTooLarge,
    InvalidMetaJson,
    LogNotUtf8,
    InvalidMinidumpSignature,
    MissingMeta,
    // Metadata
    UnsupportedSchema,
    InvalidMetadata,
    InvalidBuildId,
    CommentTooLong,
    InvalidCrashFacts,
    // Processing and storage
    ProcessingFailed,
    OutboxUnavailable,
}

impl Refusal {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::MissingPeer => "missing_peer",
            Self::RateLimit => "rate_limit",
            Self::QueueFull => "queue_full",
            Self::QueueTimeout => "queue_timeout",
            Self::QueueClosed => "queue_closed",
            Self::BodyTooLarge => "body_too_large",
            Self::InvalidBody => "invalid_body",
            Self::InvalidMultipart => "invalid_multipart",
            Self::UploadTimeout => "upload_timeout",
            Self::MissingPartName => "missing_part_name",
            Self::MissingLogFilename => "missing_log_filename",
            Self::UnknownLogFilename => "unknown_log_filename",
            Self::DuplicateLog => "duplicate_log",
            Self::DuplicateOrInvalidPart => "duplicate_or_invalid_part",
            Self::UnknownPart => "unknown_part",
            Self::PartTooLarge => "part_too_large",
            Self::InvalidMetaJson => "invalid_meta_json",
            Self::LogNotUtf8 => "log_not_utf8",
            Self::InvalidMinidumpSignature => "invalid_minidump_signature",
            Self::MissingMeta => "missing_meta",
            Self::UnsupportedSchema => "unsupported_schema",
            Self::InvalidMetadata => "invalid_metadata",
            Self::InvalidBuildId => "invalid_build_id",
            Self::CommentTooLong => "comment_too_long",
            Self::InvalidCrashFacts => "invalid_crash_facts",
            Self::ProcessingFailed => "processing_failed",
            Self::OutboxUnavailable => "outbox_unavailable",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Refusal::*;

    #[test]
    fn wire_reasons_are_unchanged() {
        for (refusal, reason) in [
            (MissingPeer, "missing_peer"),
            (RateLimit, "rate_limit"),
            (QueueFull, "queue_full"),
            (QueueTimeout, "queue_timeout"),
            (QueueClosed, "queue_closed"),
            (BodyTooLarge, "body_too_large"),
            (InvalidBody, "invalid_body"),
            (InvalidMultipart, "invalid_multipart"),
            (UploadTimeout, "upload_timeout"),
            (MissingPartName, "missing_part_name"),
            (MissingLogFilename, "missing_log_filename"),
            (UnknownLogFilename, "unknown_log_filename"),
            (DuplicateLog, "duplicate_log"),
            (DuplicateOrInvalidPart, "duplicate_or_invalid_part"),
            (UnknownPart, "unknown_part"),
            (PartTooLarge, "part_too_large"),
            (InvalidMetaJson, "invalid_meta_json"),
            (LogNotUtf8, "log_not_utf8"),
            (InvalidMinidumpSignature, "invalid_minidump_signature"),
            (MissingMeta, "missing_meta"),
            (UnsupportedSchema, "unsupported_schema"),
            (InvalidMetadata, "invalid_metadata"),
            (InvalidBuildId, "invalid_build_id"),
            (CommentTooLong, "comment_too_long"),
            (InvalidCrashFacts, "invalid_crash_facts"),
            (ProcessingFailed, "processing_failed"),
            (OutboxUnavailable, "outbox_unavailable"),
        ] {
            assert_eq!(refusal.as_str(), reason);
        }
    }
}
