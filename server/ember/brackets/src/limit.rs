//! A site's own request rate, kept on this side: start.gg allows 80
//! requests in any 60 seconds per token. Every tournament using one token
//! shares its limiter, and a request waits rather than being refused.
use std::{collections::VecDeque, sync::Arc, time::Duration};

use tokio::{sync::Mutex, time::Instant};

/// The requests sent in the last `span`, and how many it may hold.
#[derive(Debug)]
pub(crate) struct Window {
    limit: usize,
    span: Duration,
    sent: VecDeque<Instant>,
}

impl Window {
    pub fn new(limit: usize, span: Duration) -> Self {
        Self {
            limit,
            span,
            sent: VecDeque::with_capacity(limit),
        }
    }

    /// How long a request at `now` waits so that no `span` holds more than
    /// `limit` of them; nothing when it may go now.
    pub fn wait(&mut self, now: Instant) -> Option<Duration> {
        while self
            .sent
            .front()
            .is_some_and(|&sent| now.duration_since(sent) >= self.span)
        {
            self.sent.pop_front();
        }
        if self.sent.len() < self.limit {
            return None;
        }
        self.sent
            .front()
            .map(|&oldest| self.span - now.duration_since(oldest))
    }

    pub fn record(&mut self, now: Instant) {
        self.sent.push_back(now);
    }
}

/// A `Window` shared by every request made with one token.
#[derive(Clone)]
pub(crate) struct Limiter(Arc<Mutex<Window>>);

impl Limiter {
    pub fn new(limit: usize, span: Duration) -> Self {
        Self(Arc::new(Mutex::new(Window::new(limit, span))))
    }

    /// Waits until a request may be sent, and counts it as sent.
    pub async fn admit(&self) {
        let mut window = self.0.lock().await;
        while let Some(wait) = window.wait(Instant::now()) {
            tokio::time::sleep(wait).await;
        }
        window.record(Instant::now());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn no_span_holds_more_than_the_limit() {
        let start = Instant::now();
        let at = |secs: u64| start + Duration::from_secs(secs);
        let mut window = Window::new(3, Duration::from_secs(60));
        for secs in [0, 10, 20] {
            assert_eq!(window.wait(at(secs)), None, "{secs}");
            window.record(at(secs));
        }
        // The fourth waits until the first is a minute old.
        assert_eq!(window.wait(at(30)), Some(Duration::from_secs(30)));
        assert_eq!(window.wait(at(59)), Some(Duration::from_secs(1)));
        assert_eq!(window.wait(at(60)), None);
        window.record(at(60));
        assert_eq!(window.wait(at(61)), Some(Duration::from_secs(9)));
        assert_eq!(window.wait(at(200)), None);
    }
}
