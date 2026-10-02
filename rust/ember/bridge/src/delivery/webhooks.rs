//! Signed webhooks from the outbox.
//!
//! Each attempt re-resolves the destination, refuses loopback, private,
//! link-local, multicast, metadata and other special addresses, then pins
//! the connection to the address it checked, so DNS rebinding cannot steer
//! it elsewhere. Redirects are not followed and responses are read through a
//! byte cap. The stored event bytes are sent unchanged; only the delivery
//! timestamp and signature differ between attempts.
use std::{
    net::{IpAddr, Ipv4Addr, SocketAddr},
    time::Duration,
};

use ember_protocol::webhook::{self, Secret};
use reqwest::Url;
use rusqlite::{Transaction, params};

use super::{BATCH, Leased, Outcome, Queue, Record, Settled};
use crate::{AppState, error::Result};

const MAX_RESPONSE: usize = 64 * 1024;

pub struct Due {
    outbox_id: i64,
    event_id: String,
    body: Vec<u8>,
    subscription_id: String,
    url: String,
    secret: Vec<u8>,
    previous: Option<(Vec<u8>, u64)>,
}

/// The outbox: pending deliveries to enabled subscriptions whose owner's
/// credential is not revoked.
pub struct Webhooks;

impl Queue for Webhooks {
    type Item = Due;

    fn lease(&self, tx: &Transaction<'_>, now: u64, until: u64) -> Result<Vec<Leased<Due>>> {
        let rows = tx
            .prepare(
                "SELECT o.id, o.attempts, o.first_attempt_at, e.id, e.body, s.id, s.url, s.secret_sealed,
                        s.previous_secret_sealed, s.previous_expires_at
                 FROM delivery_outbox o
                 JOIN events e ON e.seq = o.event_seq
                 JOIN webhook_subscriptions s ON s.id = o.subscription_id
                 JOIN service_credentials c ON c.id = s.owner_credential
                 WHERE o.state = 'pending' AND o.next_attempt_at <= ?1 AND s.enabled = 1
                   AND c.revoked_at IS NULL
                 ORDER BY o.next_attempt_at, o.id LIMIT ?2",
            )?
            .query_map(params![now, BATCH], |row| {
                let previous: Option<Vec<u8>> = row.get(8)?;
                let until: Option<u64> = row.get(9)?;
                Ok(Leased {
                    attempt: row.get::<_, u32>(1)? + 1,
                    first_attempt_at: row.get(2)?,
                    item: Due {
                        outbox_id: row.get(0)?,
                        event_id: row.get(3)?,
                        body: row.get(4)?,
                        subscription_id: row.get(5)?,
                        url: row.get(6)?,
                        secret: row.get(7)?,
                        previous: previous.zip(until),
                    },
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        for row in &rows {
            tx.execute(
                "UPDATE delivery_outbox SET next_attempt_at = ?1, attempts = ?2 WHERE id = ?3",
                params![until, row.attempt, row.item.outbox_id],
            )?;
        }
        Ok(rows)
    }

    async fn attempt(&self, state: &AppState, item: &Due) -> Outcome {
        attempt(state, item).await
    }

    fn record(&self, tx: &Transaction<'_>, leased: &Leased<Due>, record: &Record) -> Result<()> {
        let (status, next, error) = match &record.settled {
            Settled::Delivered => ("delivered", record.at, None),
            Settled::Retry { at, error } => ("pending", *at, Some(error)),
            Settled::Refused(error) | Settled::Expired(error) => ("dead", record.at, Some(error)),
        };
        let changed = tx.execute(
            "UPDATE delivery_outbox SET state = ?1, next_attempt_at = ?2,
                first_attempt_at = COALESCE(first_attempt_at, ?3), last_error = ?4
             WHERE id = ?5 AND state = 'pending' AND attempts = ?6",
            params![
                status,
                next,
                record.first_attempt_at,
                error,
                leased.item.outbox_id,
                leased.attempt
            ],
        )?;
        // The receiver said the endpoint is gone (410).
        if changed == 1 && matches!(record.settled, Settled::Refused(_)) {
            tx.execute(
                "UPDATE webhook_subscriptions SET enabled = 0, disabled_at = ?1 WHERE id = ?2",
                params![record.at, leased.item.subscription_id],
            )?;
        }
        Ok(())
    }
}

/// Whether a leased delivery may still go out: pending, its subscription
/// enabled and its owner's credential not revoked.
async fn still_due(state: &AppState, outbox_id: i64) -> bool {
    state
        .db
        .read(move |tx| {
            Ok(tx.query_row(
                "SELECT EXISTS (SELECT 1 FROM delivery_outbox o
                   JOIN webhook_subscriptions s ON s.id = o.subscription_id
                   JOIN service_credentials c ON c.id = s.owner_credential
                   WHERE o.id = ?1 AND o.state = 'pending' AND s.enabled = 1 AND c.revoked_at IS NULL)",
                [outbox_id],
                |row| row.get::<_, bool>(0),
            )?)
        })
        .await
        .unwrap_or(false)
}

async fn attempt(state: &AppState, item: &Due) -> Outcome {
    let retry = |error: &str| Outcome::Retry {
        error: error.to_owned(),
        retry_after: None,
    };
    let now = state.now();
    let Some(current) = state.keys.open(&item.secret).and_then(|text| {
        std::str::from_utf8(&text)
            .ok()
            .and_then(|text| Secret::parse(text).ok())
    }) else {
        return retry("secret unavailable");
    };
    let previous = item
        .previous
        .as_ref()
        .filter(|(_, until)| *until > now)
        .and_then(|(sealed, _)| state.keys.open(sealed))
        .and_then(|text| {
            std::str::from_utf8(&text)
                .ok()
                .and_then(|text| Secret::parse(text).ok())
        });
    let mut secrets = vec![&current];
    if let Some(previous) = &previous {
        secrets.push(previous);
    }
    let Ok(headers) = webhook::sign(&secrets, &item.event_id, now, &item.body) else {
        return retry("cannot sign");
    };
    let allow_private = state.config.allow_private_webhooks;
    let Ok(url) = Url::parse(&item.url) else {
        return retry("invalid url");
    };
    let address = match destination(&url, allow_private).await {
        Ok(address) => address,
        Err(error) => return retry(error),
    };
    let mut builder = crate::util::outbound_client();
    if let Some(host) = url.host_str()
        && literal_ip(&url).is_none()
    {
        builder = builder.resolve(host, address);
    }
    if !allow_private {
        builder = builder.https_only(true);
    }
    let Ok(client) = builder.build() else {
        return retry("client");
    };
    // A delete or a revoked owner since the lease, including while the
    // destination resolved, cancels the delivery. Checked as late as possible.
    if !still_due(state, item.outbox_id).await {
        return Outcome::Cancelled;
    }
    let response = client
        .post(url)
        .header("content-type", "application/json")
        .header(webhook::HEADER_ID, &headers.id)
        .header(webhook::HEADER_TIMESTAMP, &headers.timestamp)
        .header(webhook::HEADER_SIGNATURE, &headers.signature)
        .body(item.body.clone())
        .send()
        .await;
    let mut response = match response {
        Ok(response) => response,
        Err(error) => {
            return retry(if error.is_timeout() {
                "timeout"
            } else {
                "connection failed"
            });
        }
    };
    let status = response.status();
    let retry_after = response
        .headers()
        .get("retry-after")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.parse::<u64>().ok());
    // Drain at most MAX_RESPONSE bytes; a hostile receiver cannot make the
    // worker buffer more than that.
    let mut read = 0;
    while let Ok(Some(chunk)) = response.chunk().await {
        read += chunk.len();
        if read > MAX_RESPONSE {
            break;
        }
    }
    if status.is_success() {
        Outcome::Delivered
    } else if status.as_u16() == 410 {
        Outcome::Refused("gone".to_owned())
    } else {
        Outcome::Retry {
            error: format!("status {}", status.as_u16()),
            retry_after,
        }
    }
}

/// Resolves and checks every address for the URL's host. Any refused
/// address refuses the destination.
async fn destination(
    url: &Url,
    allow_private: bool,
) -> std::result::Result<SocketAddr, &'static str> {
    let port = url.port_or_known_default().ok_or("no port")?;
    if let Some(ip) = literal_ip(url) {
        return if allowed_address(ip, allow_private) {
            Ok(SocketAddr::new(ip, port))
        } else {
            Err("destination refused")
        };
    }
    let host = url.host_str().ok_or("no host")?;
    let addresses: Vec<SocketAddr> = tokio::time::timeout(
        Duration::from_secs(5),
        tokio::net::lookup_host((host, port)),
    )
    .await
    .map_err(|_| "dns timeout")?
    .map_err(|_| "dns failed")?
    .collect();
    if addresses.is_empty()
        || addresses
            .iter()
            .any(|address| !allowed_address(address.ip(), allow_private))
    {
        return Err("destination refused");
    }
    Ok(addresses[0])
}

pub fn literal_ip(url: &Url) -> Option<IpAddr> {
    match url.host()? {
        url::Host::Ipv4(ip) => Some(IpAddr::V4(ip)),
        url::Host::Ipv6(ip) => Some(IpAddr::V6(ip)),
        url::Host::Domain(_) => None,
    }
}

/// True for a public unicast address. `allow_private` admits loopback and
/// private ranges for local testing; metadata and link-local stay refused.
pub fn allowed_address(ip: IpAddr, allow_private: bool) -> bool {
    match ip {
        IpAddr::V4(ip) => allowed_v4(ip, allow_private),
        IpAddr::V6(ip) => {
            if let Some(mapped) = ip.to_ipv4_mapped() {
                return allowed_v4(mapped, allow_private);
            }
            let segments = ip.segments();
            // 6to4 and NAT64 embed an IPv4 destination; judge that instead.
            if segments[0] == 0x2002 {
                let embedded = Ipv4Addr::new(
                    (segments[1] >> 8) as u8,
                    segments[1] as u8,
                    (segments[2] >> 8) as u8,
                    segments[2] as u8,
                );
                return allowed_v4(embedded, allow_private);
            }
            if segments[..6] == [0x64, 0xff9b, 0, 0, 0, 0] {
                let octets = ip.octets();
                return allowed_v4(
                    Ipv4Addr::new(octets[12], octets[13], octets[14], octets[15]),
                    allow_private,
                );
            }
            if ip.is_loopback() {
                return allow_private;
            }
            let unique_local = segments[0] & 0xfe00 == 0xfc00;
            if unique_local {
                return allow_private;
            }
            !(ip.is_unspecified()
                || ip.is_multicast()
                || segments[0] & 0xffc0 == 0xfe80 // link-local
                || segments[0] & 0xffc0 == 0xfec0 // site-local
                || (segments[0] == 0x2001 && segments[1] == 0x0db8) // documentation
                || (segments[0] == 0x2001 && segments[1] == 0) // Teredo
                || segments[0] == 0x0100 && segments[1..4] == [0, 0, 0]) // discard
        }
    }
}

fn allowed_v4(ip: Ipv4Addr, allow_private: bool) -> bool {
    let [a, b, c, _] = ip.octets();
    if ip.is_loopback() || ip.is_private() || (a == 100 && (64..128).contains(&b)) {
        return allow_private;
    }
    !(ip.is_unspecified()
        || ip.is_link_local() // includes 169.254.169.254 metadata
        || ip.is_broadcast()
        || ip.is_multicast()
        || ip.is_documentation()
        || a == 0
        || a >= 240
        || (a == 192 && b == 0 && c == 0)
        || (a == 198 && (b == 18 || b == 19)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refuses_special_addresses() {
        let refused = [
            "127.0.0.1",
            "10.1.2.3",
            "172.16.0.1",
            "192.168.1.1",
            "169.254.169.254",
            "100.64.0.1",
            "0.0.0.0",
            "224.0.0.1",
            "255.255.255.255",
            "192.0.2.1",
            "198.18.0.1",
            "240.0.0.1",
            "::1",
            "::",
            "fe80::1",
            "fc00::1",
            "fd12:3456::1",
            "ff02::1",
            "::ffff:127.0.0.1",
            "::ffff:169.254.169.254",
            "2002:7f00:0001::1",
            "64:ff9b::a9fe:a9fe",
            "2001:db8::1",
            "2001::1",
        ];
        for text in refused {
            assert!(!allowed_address(text.parse().unwrap(), false), "{text}");
        }
        for text in [
            "93.184.216.34",
            "2606:4700::1111",
            "::ffff:93.184.216.34",
            "1.1.1.1",
        ] {
            assert!(allowed_address(text.parse().unwrap(), false), "{text}");
        }
        // Local testing admits loopback and private ranges, never metadata.
        assert!(allowed_address("127.0.0.1".parse().unwrap(), true));
        assert!(allowed_address("::1".parse().unwrap(), true));
        assert!(!allowed_address("169.254.169.254".parse().unwrap(), true));
        assert!(!allowed_address(
            "::ffff:169.254.169.254".parse().unwrap(),
            true
        ));
    }
}
