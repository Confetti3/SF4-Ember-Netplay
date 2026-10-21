//! Just enough S3 for the replay archive: a path-style `PUT` signed with
//! AWS Signature Version 4, which any S3 server (rustfs, MinIO, AWS) takes.
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

use crate::config::Archive;

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn hmac(key: &[u8], data: &str) -> Vec<u8> {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("any key length");
    mac.update(data.as_bytes());
    mac.finalize().into_bytes().to_vec()
}

/// `YYYYMMDDTHHMMSSZ` for unix seconds `now`, UTC.
fn amz_date(now: u64) -> String {
    let (days, secs) = ((now / 86_400) as i64, now % 86_400);
    // Days to a civil date (Howard Hinnant's days_from_civil, inverted).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}{month:02}{day:02}T{:02}{:02}{:02}Z",
        secs / 3600,
        secs / 60 % 60,
        secs % 60
    )
}

/// The headers of a signed `PUT` of `body` to `key` at `now`: host,
/// x-amz-date, x-amz-content-sha256 and authorization. `key` holds only
/// characters S3 needs no escaping for (`[a-z0-9/._-]`).
pub fn signed_put(
    archive: &Archive,
    secret: &str,
    key: &str,
    body: &[u8],
    now: u64,
) -> Vec<(&'static str, String)> {
    let host = archive
        .endpoint
        .split_once("://")
        .map_or(archive.endpoint.as_str(), |(_, rest)| rest)
        .trim_end_matches('/')
        .to_owned();
    let date = amz_date(now);
    let day = &date[..8];
    let payload = hex(&Sha256::digest(body));
    let path = format!("/{}/{key}", archive.bucket);
    let request = format!(
        "PUT\n{path}\n\nhost:{host}\nx-amz-content-sha256:{payload}\nx-amz-date:{date}\n\nhost;x-amz-content-sha256;x-amz-date\n{payload}"
    );
    let scope = format!("{day}/{}/s3/aws4_request", archive.region);
    let to_sign = format!(
        "AWS4-HMAC-SHA256\n{date}\n{scope}\n{}",
        hex(&Sha256::digest(request.as_bytes()))
    );
    let mut signing = hmac(format!("AWS4{secret}").as_bytes(), day);
    for part in [archive.region.as_str(), "s3", "aws4_request"] {
        signing = hmac(&signing, part);
    }
    let signature = hex(&hmac(&signing, &to_sign));
    vec![
        ("host", host),
        ("x-amz-date", date),
        ("x-amz-content-sha256", payload),
        (
            "authorization",
            format!(
                "AWS4-HMAC-SHA256 Credential={}/{scope}, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature={signature}",
                archive.access_key
            ),
        ),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dates_are_utc() {
        assert_eq!(amz_date(0), "19700101T000000Z");
        assert_eq!(amz_date(951_782_400), "20000229T000000Z");
        assert_eq!(amz_date(1_792_576_171), "20261021T094931Z");
    }

    /// AWS's own example (GET Object, "Example: GET Object" in the SigV4
    /// docs) has a GET; the same key derivation and string-to-sign layout
    /// checked here on its signing key.
    #[test]
    fn signing_key_matches_aws_example() {
        let mut key = hmac(b"AWS4wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20120215");
        for part in ["us-east-1", "iam", "aws4_request"] {
            key = hmac(&key, part);
        }
        assert_eq!(
            hex(&key),
            "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d"
        );
    }
}
