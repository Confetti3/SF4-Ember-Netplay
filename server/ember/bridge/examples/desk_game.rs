//! Plays the game's side of the tournament desk from a terminal, for the
//! desk's end-to-end test (scripts/e2e-desk.mjs): an Ember ID from a seed
//! file signs what Ember would sign, against any bridge.
//!
//! ```sh
//! desk_game <bridge origin> <seed file> id
//! desk_game <bridge origin> <seed file> organize <site>   # prints the site's consent page
//! desk_game <bridge origin> <seed file> handoff           # prints a signed-in /desk link
//! desk_game <bridge origin> <seed file> device <code>     # what asked, and the numbers to pick from
//! desk_game <bridge origin> <seed file> approve <code> <number>
//! ```
//!
//! The seed file holds 32 bytes; a missing one is made, readable only by its
//! owner. Answers print as JSON. A loopback http bridge is taken for local
//! tests; any other must be https.
use std::{io::Write, path::Path, time::SystemTime};

use ember_protocol::{
    SigningIdentity,
    challenge::{Action, Challenge, Expected, Method, ProvenRequest, command_digest},
    encoding::OriginPolicy,
    json,
};
use reqwest::Client;
use serde_json::{Value as Json, json};
use zeroize::Zeroizing;

struct Game {
    client: Client,
    origin: String,
    bridge_id: String,
    identity: SigningIdentity,
    session: Option<String>,
}

fn seed(path: &Path) -> Zeroizing<[u8; 32]> {
    let mut seed = Zeroizing::new([0u8; 32]);
    match std::fs::read(path) {
        Ok(bytes) => seed.copy_from_slice(&bytes[..32]),
        Err(_) => {
            getrandom::fill(seed.as_mut()).unwrap();
            let mut options = std::fs::OpenOptions::new();
            options.write(true).create_new(true);
            #[cfg(unix)]
            std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
            options
                .open(path)
                .unwrap()
                .write_all(seed.as_ref())
                .unwrap();
        }
    }
    seed
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

impl Game {
    async fn new(origin: &str, seed_file: &Path) -> Self {
        ember_bridge::install_crypto_provider();
        let client = Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .build()
            .unwrap();
        let known = client
            .get(format!("{origin}/.well-known/ember-bridge.json"))
            .send()
            .await
            .unwrap()
            .bytes()
            .await
            .unwrap();
        let known: Json = serde_json::from_slice(&known).unwrap();
        Self {
            client,
            origin: origin.trim_end_matches('/').to_owned(),
            bridge_id: known["bridge_id"].as_str().unwrap().to_owned(),
            identity: SigningIdentity::from_seed(&seed(seed_file)).unwrap(),
            session: None,
        }
    }

    /// Signs `command` for `action` at `path` and sends it, as the helper does.
    async fn send(&self, action: Action, path: &str, command: Json) -> (u16, Json) {
        let command = json::to_value(&command).unwrap();
        let mut request = self
            .client
            .post(format!("{}/v1/auth/challenges", self.origin))
            .header("content-type", "application/json")
            .body(
                json!({
                    "public_key": self.identity.public_key(),
                    "action": action,
                    "method": Method::Post,
                    "path": path,
                    "request_digest": command_digest(&command),
                })
                .to_string(),
            );
        if let Some(session) = &self.session {
            request = request.bearer_auth(session);
        }
        let bytes = request.send().await.unwrap().bytes().await.unwrap();
        let challenge: Challenge = json::parse_as(&bytes, 8192)
            .unwrap_or_else(|_| panic!("no challenge: {}", String::from_utf8_lossy(&bytes)));
        let expected = Expected {
            bridge_id: &self.bridge_id,
            audience: &self.origin,
            ember_id: self.identity.ember_id(),
            action,
            method: Method::Post,
            path,
            command: &command,
        };
        let proof = challenge
            .sign(
                &self.identity,
                &expected,
                now(),
                OriginPolicy::AllowLoopbackHttp,
            )
            .unwrap();
        let mut request = self
            .client
            .post(format!("{}{path}", self.origin))
            .header("content-type", "application/json")
            .body(ProvenRequest { command, proof }.to_json().unwrap());
        if let Some(session) = &self.session {
            request = request.bearer_auth(session);
        }
        read(request.send().await.unwrap()).await
    }

    async fn sign_in(&mut self) {
        let (status, made) = self
            .send(
                Action::SessionCreate,
                "/v1/sessions",
                json!({ "requested_scopes": ["self:read", "tournament:participate"] }),
            )
            .await;
        assert_eq!(status, 201, "session: {made}");
        self.session = Some(made["session_token"].as_str().unwrap().to_owned());
    }

    async fn get(&self, path: &str) -> (u16, Json) {
        let request = self
            .client
            .get(format!("{}{path}", self.origin))
            .bearer_auth(self.session.as_deref().unwrap());
        read(request.send().await.unwrap()).await
    }
}

async fn read(response: reqwest::Response) -> (u16, Json) {
    let status = response.status().as_u16();
    let bytes = response.bytes().await.unwrap();
    let body = serde_json::from_slice(&bytes)
        .unwrap_or_else(|_| Json::String(String::from_utf8_lossy(&bytes).into()));
    (status, body)
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [origin, seed_file, command, rest @ ..] = args.as_slice() else {
        eprintln!(
            "usage: desk_game <bridge origin> <seed file> id|organize <site>|handoff|device <code>|approve <code> <number>"
        );
        std::process::exit(2);
    };
    let mut game = Game::new(origin, Path::new(seed_file)).await;
    let (status, answer) = match (command.as_str(), rest) {
        ("id", []) => (
            200,
            json!({ "ember_id": game.identity.ember_id().as_str() }),
        ),
        ("organize", [site]) => {
            game.sign_in().await;
            game.send(
                Action::SiteConnect,
                &format!("/v1/sites/{site}/organize"),
                json!({}),
            )
            .await
        }
        ("handoff", []) => {
            game.sign_in().await;
            let (status, made) = game
                .send(Action::SiteConnect, "/v1/web-sessions/handoff", json!({}))
                .await;
            let link = made["token"]
                .as_str()
                .map(|token| format!("{}/desk/h/{token}", game.origin));
            (status, json!({ "link": link, "answer": made }))
        }
        ("device", [code]) => {
            game.sign_in().await;
            game.get(&format!("/v1/web-sessions/device?code={code}"))
                .await
        }
        ("approve", [code, number]) => {
            game.sign_in().await;
            let number: u64 = number.parse().unwrap();
            game.send(
                Action::SiteConnect,
                "/v1/web-sessions/approve",
                json!({ "code": code, "number": number }),
            )
            .await
        }
        _ => {
            eprintln!("unknown command");
            std::process::exit(2);
        }
    };
    println!("{}", json!({ "status": status, "body": answer }));
    if !(200..300).contains(&status) {
        std::process::exit(1);
    }
}
