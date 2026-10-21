//! Just enough S3 for the replay archive: a path-style `PUT` signed with
//! AWS Signature Version 4, which any S3 server (rustfs, MinIO, AWS) takes.
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

use crate::{config::Archive, util::hex};

fn hmac(key: &[u8], data: &str) -> Vec<u8> {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("any key length");
    mac.update(data.as_bytes());
    mac.finalize().into_bytes().to_vec()
}

/// `YYYYMMDDTHHMMSSZ` for unix seconds `now`, UTC.
fn amz_date(now: u64) -> String {
    ember_protocol::event::rfc3339(now).replace(['-', ':'], "")
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

    /// The same PUT signed by botocore (AWS's own Python signer) with AWS's
    /// example credentials.
    #[test]
    fn a_put_is_signed_as_aws_signs_it() {
        let archive = Archive {
            endpoint: "http://127.0.0.1:9400".into(),
            bucket: "replays".into(),
            region: "us-east-1".into(),
            access_key: "AKIAIOSFODNN7EXAMPLE".into(),
            public_base: "https://s3.example/replays".into(),
        };
        let headers = signed_put(
            &archive,
            "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
            "games/0badf00d.emberreplay",
            b"Welcome to Amazon S3.",
            1_792_576_171,
        );
        let authorization = &headers.iter().find(|(name, _)| *name == "authorization").unwrap().1;
        assert_eq!(
            authorization,
            "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20261021/us-east-1/s3/aws4_request, \
             SignedHeaders=host;x-amz-content-sha256;x-amz-date, \
             Signature=c11dd0e59eaeca03ac783eba11a79d98b6049217a5c43986cfe3d12bf4321b5a"
        );
    }
}
