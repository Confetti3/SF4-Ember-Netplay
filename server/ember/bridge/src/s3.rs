//! Just enough S3 for the replay archive: path-style `PUT`s, `GET`s and
//! `DELETE`s of one object, signed with AWS Signature Version 4, which any
//! S3 server (rustfs, MinIO, AWS) takes.
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};
use reqwest::Method;
use url::Url;

use crate::{
    config::Archive,
    util::{Outbound, hex, read_bounded},
};

/// `endpoint` as an S3 origin: http or https, a host and an optional port,
/// with no credentials, path, query or fragment. `Config::validate` checks
/// it with this, and `Bucket::new` keeps what it reads.
pub fn origin(endpoint: &str) -> Result<Url, &'static str> {
    Url::parse(endpoint)
        .ok()
        .filter(|url| {
            matches!(url.scheme(), "http" | "https")
                && url.has_host()
                && url.username().is_empty()
                && url.password().is_none()
                && url.path() == "/"
                && url.query().is_none()
                && url.fragment().is_none()
        })
        .ok_or("archive.endpoint must be an http(s) origin without a path, query or credentials")
}

/// The archive's bucket, as the delivery queues write to it. Each object's
/// URL is built once, from the endpoint read once, and both the signature
/// and the request are made from that URL, so they name the same host.
pub struct Bucket {
    origin: Url,
    bucket: String,
    region: String,
    access_key: String,
    client: Outbound,
}

impl Bucket {
    pub fn new(archive: &Archive) -> Result<Self, &'static str> {
        Ok(Self {
            origin: origin(&archive.endpoint)?,
            bucket: archive.bucket.clone(),
            region: archive.region.clone(),
            access_key: archive.access_key.clone(),
            client: Outbound::default(),
        })
    }

    /// Where the object `key` is. `key` holds only characters S3 needs no
    /// escaping for (`[a-z0-9/._-]`), as do bucket names.
    fn url(&self, key: &str) -> Url {
        let mut url = self.origin.clone();
        url.set_path(&format!("/{}/{key}", self.bucket));
        url
    }

    /// Puts `body` at `key`, signed with `secret` at `now`; the error says
    /// why the bucket did not take it.
    pub async fn put(
        &self,
        secret: &str,
        key: &str,
        body: Vec<u8>,
        now: u64,
    ) -> Result<(), String> {
        self.send(Method::PUT, secret, key, body, now).await.map(drop)
    }

    /// The object at `key`, at most `limit` bytes, or why it could not be
    /// read.
    pub async fn get(
        &self,
        secret: &str,
        key: &str,
        limit: usize,
        now: u64,
    ) -> Result<Vec<u8>, String> {
        let response = self.send(Method::GET, secret, key, Vec::new(), now).await?;
        read_bounded(response, limit)
            .await
            .ok_or_else(|| format!("{key}: unreadable"))
    }

    /// Deletes the object at `key`. S3 answers a delete of an object that is
    /// already gone with success too.
    pub async fn delete(&self, secret: &str, key: &str, now: u64) -> Result<(), String> {
        self.send(Method::DELETE, secret, key, Vec::new(), now)
            .await
            .map(drop)
    }

    /// Sends a signed `method` for `key` with `body`; the error says why the
    /// bucket did not answer with success.
    async fn send(
        &self,
        method: Method,
        secret: &str,
        key: &str,
        body: Vec<u8>,
        now: u64,
    ) -> Result<reqwest::Response, String> {
        let client = self.client.client().map_err(|_| "client".to_owned())?;
        let url = self.url(key);
        let mut request = client.request(method.clone(), url.clone());
        if method == Method::PUT {
            request = request.header("content-type", "application/octet-stream");
        }
        for (name, value) in self.signed(&method, &url, secret, &body, now) {
            request = request.header(name, value);
        }
        match request.body(body).send().await {
            Ok(response) if response.status().is_success() => Ok(response),
            Ok(response) => Err(format!("{key}: status {}", response.status().as_u16())),
            Err(_) => Err(format!("{key}: unreachable")),
        }
    }

    /// The headers that sign a `method` of `body` to `url` at `now`:
    /// x-amz-date, x-amz-content-sha256 and authorization. The signed host
    /// is the one the request sends (`authority`).
    fn signed(
        &self,
        method: &Method,
        url: &Url,
        secret: &str,
        body: &[u8],
        now: u64,
    ) -> Vec<(&'static str, String)> {
        let date = amz_date(now);
        let day = &date[..8];
        let payload = hex(&Sha256::digest(body));
        let request = format!(
            "{method}\n{}\n\nhost:{}\nx-amz-content-sha256:{payload}\nx-amz-date:{date}\n\nhost;x-amz-content-sha256;x-amz-date\n{payload}",
            url.path(),
            authority(url)
        );
        let scope = format!("{day}/{}/s3/aws4_request", self.region);
        let to_sign = format!(
            "AWS4-HMAC-SHA256\n{date}\n{scope}\n{}",
            hex(&Sha256::digest(request.as_bytes()))
        );
        let mut signing = hmac(format!("AWS4{secret}").as_bytes(), day);
        for part in [self.region.as_str(), "s3", "aws4_request"] {
            signing = hmac(&signing, part);
        }
        let signature = hex(&hmac(&signing, &to_sign));
        vec![
            ("x-amz-date", date),
            ("x-amz-content-sha256", payload),
            (
                "authorization",
                format!(
                    "AWS4-HMAC-SHA256 Credential={}/{scope}, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature={signature}",
                    self.access_key
                ),
            ),
        ]
    }
}

/// The `Host` a request to `url` carries: its host, IPv6 in brackets, and
/// its port unless that is the scheme's default, which `Url` drops.
fn authority(url: &Url) -> String {
    let host = url.host_str().unwrap_or_default();
    match url.port() {
        Some(port) => format!("{host}:{port}"),
        None => host.to_owned(),
    }
}

fn hmac(key: &[u8], data: &str) -> Vec<u8> {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("any key length");
    mac.update(data.as_bytes());
    mac.finalize().into_bytes().to_vec()
}

/// `YYYYMMDDTHHMMSSZ` for unix seconds `now`, UTC.
fn amz_date(now: u64) -> String {
    ember_protocol::event::rfc3339(now).replace(['-', ':'], "")
}

#[cfg(test)]
mod tests {
    use tokio::{
        io::{AsyncReadExt, AsyncWriteExt},
        net::TcpListener,
    };

    use super::*;

    fn bucket(endpoint: &str) -> Bucket {
        Bucket::new(&Archive {
            endpoint: endpoint.into(),
            bucket: "replays".into(),
            region: "us-east-1".into(),
            access_key: "AKIAIOSFODNN7EXAMPLE".into(),
            public_base: "https://s3.example/replays".into(),
        })
        .unwrap()
    }

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
        let bucket = bucket("http://127.0.0.1:9400");
        let url = bucket.url("games/0badf00d.emberreplay");
        assert_eq!(
            url.as_str(),
            "http://127.0.0.1:9400/replays/games/0badf00d.emberreplay"
        );
        let headers = bucket.signed(
            &Method::PUT,
            &url,
            "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
            b"Welcome to Amazon S3.",
            1_792_576_171,
        );
        let authorization = &headers
            .iter()
            .find(|(name, _)| *name == "authorization")
            .unwrap()
            .1;
        assert_eq!(
            authorization,
            "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20261021/us-east-1/s3/aws4_request, \
             SignedHeaders=host;x-amz-content-sha256;x-amz-date, \
             Signature=c11dd0e59eaeca03ac783eba11a79d98b6049217a5c43986cfe3d12bf4321b5a"
        );
    }

    #[test]
    fn only_an_origin_is_an_endpoint() {
        for endpoint in [
            "http://127.0.0.1:9400",
            "https://store.example",
            "https://store.example/",
            "http://[::1]:9400",
        ] {
            assert!(origin(endpoint).is_ok(), "{endpoint}");
        }
        for endpoint in [
            "ftp://store.example",
            "https://key:secret@store.example",
            "https://key@store.example",
            "https://store.example/replays",
            "https://store.example/?x=1",
            "https://store.example?",
            "https://store.example/#top",
            "store.example:9400",
            "",
        ] {
            assert!(origin(endpoint).is_err(), "{endpoint}");
        }
    }

    /// A default port is neither sent nor signed; any other port and an
    /// IPv6 host are both, as written.
    #[test]
    fn the_signed_host_is_the_url_host() {
        for (endpoint, url, host) in [
            (
                "https://store.example:443",
                "https://store.example/replays/k",
                "store.example",
            ),
            (
                "http://store.example:80",
                "http://store.example/replays/k",
                "store.example",
            ),
            (
                "https://store.example:9443",
                "https://store.example:9443/replays/k",
                "store.example:9443",
            ),
            (
                "http://[::1]:9400",
                "http://[::1]:9400/replays/k",
                "[::1]:9400",
            ),
            (
                "https://[2001:db8::1]",
                "https://[2001:db8::1]/replays/k",
                "[2001:db8::1]",
            ),
        ] {
            let built = bucket(endpoint).url("k");
            assert_eq!(built.as_str(), url, "{endpoint}");
            assert_eq!(authority(&built), host, "{endpoint}");
        }
    }

    /// The Host header of the first request `listener` takes, which it
    /// reads whole and answers 200.
    async fn host_sent(listener: TcpListener) -> String {
        let (mut socket, _) = listener.accept().await.unwrap();
        let mut head = Vec::new();
        while !head.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            socket.read_exact(&mut byte).await.unwrap();
            head.push(byte[0]);
        }
        let head = String::from_utf8(head).unwrap();
        let header = |wanted: &str| {
            head.lines().find_map(|line| {
                let (name, value) = line.split_once(':')?;
                name.eq_ignore_ascii_case(wanted)
                    .then(|| value.trim().to_owned())
            })
        };
        let length: usize = header("content-length").unwrap().parse().unwrap();
        socket.read_exact(&mut vec![0; length]).await.unwrap();
        socket
            .write_all(b"HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n")
            .await
            .unwrap();
        header("host").unwrap()
    }

    /// What reqwest sends as the host is what the signature signed.
    #[tokio::test]
    async fn the_request_sends_the_signed_host() {
        crate::install_crypto_provider();
        for bind in ["127.0.0.1:0", "[::1]:0"] {
            let Ok(listener) = TcpListener::bind(bind).await else {
                continue;
            };
            let address = listener.local_addr().unwrap();
            let bucket = bucket(&format!("http://{address}"));
            let sent = tokio::spawn(host_sent(listener));
            let key = "games/0badf00d.emberreplay";
            bucket
                .put("secret", key, b"game".to_vec(), 1_792_576_171)
                .await
                .unwrap();
            assert_eq!(sent.await.unwrap(), authority(&bucket.url(key)), "{bind}");
        }
    }
}
