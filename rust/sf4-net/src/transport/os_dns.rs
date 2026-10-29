//! Host names through the operating system's resolver.
//!
//! Iroh's own resolver reads the system's nameservers and asks two of them at
//! once, giving up after three seconds. A router that hands out a dead server
//! (often an ISP IPv6 one beside a working IPv4 one) then fails a lookup when
//! both picks are dead, and the first network report finds no relay. Iroh
//! retries only on its next periodic report, about 20 seconds later, so
//! hosting a room fails as "relay unreachable" while the PC itself browses
//! fine: Windows' resolver skips servers that do not answer. Address lookups
//! therefore go to the OS. TXT lookups, which the OS resolver cannot do, stay
//! with iroh's resolver.
//!
//! Each lookup is one blocking OS call that answers both families, of which
//! the caller keeps one. Iroh staggers its lookups, so a relay host costs up to
//! three calls per family per network report; the OS cache answers them.
use std::{
    future::Future,
    net::{IpAddr, Ipv4Addr, Ipv6Addr},
    pin::Pin,
};

use iroh::dns::{BoxIter, DNS_TIMEOUT, DnsError, DnsResolver, Resolver, TxtRecordData};

type Lookup<T> = Pin<Box<dyn Future<Output = Result<BoxIter<T>, DnsError>> + Send>>;

/// Address lookups through the OS, TXT lookups through iroh's resolver.
pub(super) fn resolver() -> DnsResolver {
    DnsResolver::custom(OsDns(DnsResolver::new()))
}

/// The inner resolver answers TXT lookups only.
#[derive(Debug)]
struct OsDns(DnsResolver);

/// The addresses the OS finds for `host` that `pick` keeps.
fn os_lookup<T: Send + 'static>(host: String, pick: fn(IpAddr) -> Option<T>) -> Lookup<T> {
    Box::pin(async move {
        let found = tokio::net::lookup_host((host.as_str(), 0))
            .await
            .map_err(|error| DnsError::from(n0_error::AnyError::from(error)))?;
        let ips: Vec<T> = found.filter_map(|addr| pick(addr.ip())).collect();
        Ok(Box::new(ips.into_iter()) as BoxIter<T>)
    })
}

impl Resolver for OsDns {
    fn lookup_ipv4(&self, host: String) -> Lookup<Ipv4Addr> {
        os_lookup(host, |ip| match ip {
            IpAddr::V4(ip) => Some(ip),
            IpAddr::V6(_) => None,
        })
    }

    fn lookup_ipv6(&self, host: String) -> Lookup<Ipv6Addr> {
        os_lookup(host, |ip| match ip {
            IpAddr::V6(ip) => Some(ip),
            IpAddr::V4(_) => None,
        })
    }

    fn lookup_txt(&self, host: String) -> Lookup<TxtRecordData> {
        let resolver = self.0.clone();
        Box::pin(async move {
            let records = resolver.lookup_txt(host, DNS_TIMEOUT).await?;
            Ok(Box::new(records.collect::<Vec<_>>().into_iter()) as BoxIter<_>)
        })
    }

    fn clear_cache(&self) {
        self.0.clear_cache();
    }

    fn reset(&self) -> Box<dyn Resolver> {
        Box::new(OsDns(DnsResolver::new()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn addresses_skip_iroh_resolver() {
        // The inner resolver's only nameserver never answers, so a name that
        // resolves came from the OS (the hosts file, no network).
        let dead = DnsResolver::with_nameserver((Ipv4Addr::LOCALHOST, 9).into());
        let dns = OsDns(dead);
        let v4: Vec<_> = dns.lookup_ipv4("localhost".into()).await.unwrap().collect();
        assert!(v4.contains(&Ipv4Addr::LOCALHOST), "{v4:?}");
        // A host without IPv6 may give no AAAA answer, but never an error.
        let v6: Vec<_> = dns.lookup_ipv6("localhost".into()).await.unwrap().collect();
        assert!(v6.iter().all(Ipv6Addr::is_loopback), "{v6:?}");
    }
}
