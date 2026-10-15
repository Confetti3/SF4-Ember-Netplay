//! A stand-in platform that receives signed webhook-style POSTs: event
//! webhooks and a connection's results.
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::Duration,
};

use axum::{Router, body::Bytes, http::HeaderMap, routing::post};
use ember_protocol::webhook::{self, Headers};

pub type Delivery = (Headers, Vec<u8>);

/// What the stand-in was sent, and the statuses to answer with (in order)
/// before it answers 200.
#[derive(Clone, Default)]
pub struct Receiver {
    pub deliveries: Arc<Mutex<Vec<Delivery>>>,
    pub answers: Arc<Mutex<VecDeque<u16>>>,
}

impl Receiver {
    pub fn answer_with(&self, statuses: &[u16]) {
        self.answers
            .lock()
            .unwrap()
            .extend(statuses.iter().copied());
    }

    pub fn received(&self) -> Vec<Delivery> {
        self.deliveries.lock().unwrap().clone()
    }
}

/// A receiver and the URL it listens on.
pub async fn receiver() -> (String, Receiver) {
    let state = Receiver::default();
    let shared = state.clone();
    let app = Router::new().route(
        "/hook",
        post(move |headers: HeaderMap, body: Bytes| {
            let shared = shared.clone();
            async move {
                let get = |name: &str| {
                    headers
                        .get(name)
                        .map(|v| v.to_str().unwrap().to_owned())
                        .unwrap_or_default()
                };
                shared.deliveries.lock().unwrap().push((
                    Headers {
                        id: get(webhook::HEADER_ID),
                        timestamp: get(webhook::HEADER_TIMESTAMP),
                        signature: get(webhook::HEADER_SIGNATURE),
                    },
                    body.to_vec(),
                ));
                let status = shared.answers.lock().unwrap().pop_front().unwrap_or(200);
                axum::http::StatusCode::from_u16(status).unwrap()
            }
        }),
    );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (format!("http://{address}/hook"), state)
}

/// The deliveries once there are at least `count`, for at most ten seconds.
pub async fn wait_for(receiver: &Receiver, count: usize) -> Vec<Delivery> {
    for _ in 0..200 {
        let deliveries = receiver.received();
        if deliveries.len() >= count {
            return deliveries;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    panic!(
        "expected {count} deliveries, got {}",
        receiver.received().len()
    );
}
