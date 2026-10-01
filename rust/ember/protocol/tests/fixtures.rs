//! Ports every check of `docs/design/identity-bridge/tools/verify_fixtures.py`
//! so the Rust wire rules reproduce the specification's public test vectors.
//! All seeds and secrets here are public fixtures.
use std::{fs, path::PathBuf};

use ember_protocol::{
    EmberId, Error, PublicKey, SigningIdentity,
    challenge::{Action, Challenge, Expected, Method, Proof, ProvenRequest, command_digest},
    encoding::{Counter, OriginPolicy, b64u, decode_b64u},
    event::Event,
    json::{self, MAX_BODY, Value},
    matches::{CreateMatch, MatchCompleted},
    report::SignedReport,
    sign::Domain,
    webhook::{self, Headers, Secret},
};
use serde::Deserialize;
use zeroize::Zeroizing;

const POLICY: OriginPolicy = OriginPolicy::HttpsOnly;
const AUDIENCE: &str = "https://bridge.ember.example";

fn examples() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../docs/design/identity-bridge/examples")
}

fn bytes(name: &str) -> Vec<u8> {
    fs::read(examples().join(name)).unwrap_or_else(|error| panic!("{name}: {error}"))
}

fn load(name: &str) -> Value {
    json::parse(&bytes(name), MAX_BODY).unwrap()
}

fn typed<T: serde::de::DeserializeOwned>(name: &str) -> T {
    json::parse_as(&bytes(name), MAX_BODY).unwrap()
}

#[derive(Deserialize)]
struct Vectors {
    identities: Vec<IdentityVector>,
    challenge: ChallengeVector,
}

#[derive(Deserialize)]
struct IdentityVector {
    seed_hex: String,
    public_key_hex: String,
    public_key_base64url: String,
    ember_id: String,
}

#[derive(Deserialize)]
struct ChallengeVector {
    command: serde_json::Value,
    canonical_utf8: String,
    signing_bytes_hex: String,
    signature_base64url: String,
}

#[derive(Deserialize)]
struct WebhookFixture {
    secret_base64: String,
    body_file: String,
    verification_time_unix: u64,
    headers: WebhookHeaders,
}

#[derive(Deserialize)]
struct WebhookHeaders {
    #[serde(rename = "webhook-id")]
    id: String,
    #[serde(rename = "webhook-timestamp")]
    timestamp: String,
    #[serde(rename = "webhook-signature")]
    signature: String,
}

fn vectors() -> Vectors {
    typed("test-vectors.json")
}

fn hex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap())
        .collect()
}

fn to_hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{b:02x}")).collect()
}

fn identity(index: usize) -> SigningIdentity {
    let seed: [u8; 32] = hex(&vectors().identities[index].seed_hex)
        .try_into()
        .unwrap();
    SigningIdentity::from_seed(&Zeroizing::new(seed)).unwrap()
}

fn command() -> Value {
    json::to_value(&vectors().challenge.command).unwrap()
}

fn expected<'a>(id: &'a EmberId, command: &'a Value) -> Expected<'a> {
    Expected {
        bridge_id: "brg_11111111-1111-4111-8111-111111111111",
        audience: AUDIENCE,
        ember_id: id,
        action: Action::SessionCreate,
        method: Method::Post,
        path: "/v1/sessions",
        command,
    }
}

// schema: identity.json, auth-challenge.json, auth-proof.json, create-match.json,
// game-report-p1.json, game-report-p2.json, match-completed-event.json
#[test]
fn schema_examples_decode_strictly() {
    let identity = load("identity.json");
    let key = PublicKey::from_b64u(identity.get("public_key").unwrap().as_str().unwrap()).unwrap();
    assert_eq!(
        key.ember_id().as_str(),
        identity.get("ember_id").unwrap().as_str().unwrap()
    );
    assert_eq!(identity.get("algorithm").unwrap().as_str(), Some("Ed25519"));

    typed::<Challenge>("auth-challenge.json")
        .check_shape(POLICY)
        .unwrap();
    typed::<Proof>("auth-proof.json");
    typed::<CreateMatch>("create-match.json").check().unwrap();
    for name in ["game-report-p1.json", "game-report-p2.json"] {
        typed::<SignedReport>(name).report.check().unwrap();
    }
    typed::<Event>("match-completed-event.json")
        .check(POLICY)
        .unwrap();

    let request = ProvenRequest::parse(&bytes("auth-session-request.json")).unwrap();
    assert_eq!(request.command, command());
}

// identity derivation vector 1 and 2
#[test]
fn identity_derivation_vectors() {
    for (index, vector) in vectors().identities.iter().enumerate() {
        let identity = identity(index);
        let key = identity.public_key();
        assert_eq!(to_hex(&key.to_bytes()), vector.public_key_hex);
        assert_eq!(key.to_b64u(), vector.public_key_base64url);
        assert_eq!(identity.ember_id().as_str(), vector.ember_id);
        assert_eq!(vector.ember_id.len(), 57);
        assert_eq!(
            EmberId::parse(&vector.ember_id).unwrap(),
            *identity.ember_id()
        );
        assert_eq!(
            PublicKey::from_b64u(&vector.public_key_base64url).unwrap(),
            key
        );
    }
}

// challenge canonical bytes, challenge signing bytes
#[test]
fn challenge_canonical_and_signing_bytes() {
    let challenge: Challenge = typed("auth-challenge.json");
    let value = challenge.to_value().unwrap();
    let vector = vectors().challenge;
    assert_eq!(
        String::from_utf8(value.canonical()).unwrap(),
        vector.canonical_utf8
    );
    assert_eq!(
        to_hex(&Domain::Challenge.signing_bytes(&value)),
        vector.signing_bytes_hex
    );
    // The file as received canonicalizes the same way as the typed object.
    assert_eq!(load("auth-challenge.json").canonical(), value.canonical());
}

// challenge signature and request binding
#[test]
fn challenge_signature_and_binding() {
    let challenge: Challenge = typed("auth-challenge.json");
    let proof: Proof = typed("auth-proof.json");
    let identity = identity(0);
    let command = command();
    let now = challenge.issued_at + 1;
    assert_eq!(challenge.request_digest, command_digest(&command));
    let expected = expected(identity.ember_id(), &command);
    challenge
        .verify(&expected, &proof, &identity.public_key(), now, POLICY)
        .unwrap();
    // Ed25519 is deterministic, so our signer reproduces the fixture proof.
    let signed = challenge.sign(&identity, &expected, now, POLICY).unwrap();
    assert_eq!(signed, proof);
    assert_eq!(signed.signature, vectors().challenge.signature_base64url);
}

// reject tampered challenge nonce, wrong audience, wrong command digest,
// expired challenge
#[test]
fn challenge_rejections() {
    let challenge: Challenge = typed("auth-challenge.json");
    let proof: Proof = typed("auth-proof.json");
    let identity = identity(0);
    let key = identity.public_key();
    let command = command();
    let now = challenge.issued_at + 1;
    let expected = expected(identity.ember_id(), &command);

    let mut nonce = challenge.clone();
    nonce.nonce = b64u(&[0; 32]);
    assert_eq!(
        nonce.verify(&expected, &proof, &key, now, POLICY),
        Err(Error::InvalidSignature)
    );

    let mut audience = challenge.clone();
    audience.audience = "https://attacker.example".into();
    assert_eq!(
        audience.verify(&expected, &proof, &key, now, POLICY),
        Err(Error::Mismatch("audience"))
    );
    // Even when the verifier is talked into the wrong audience, the
    // signature still binds the original one.
    let attacker = Expected {
        audience: "https://attacker.example",
        ..expected
    };
    assert_eq!(
        audience.verify(&attacker, &proof, &key, now, POLICY),
        Err(Error::InvalidSignature)
    );

    let admin = json::parse(br#"{"requested_scopes":["admin"]}"#, MAX_BODY).unwrap();
    let wrong_command = Expected {
        command: &admin,
        ..expected
    };
    assert_eq!(
        challenge.verify(&wrong_command, &proof, &key, now, POLICY),
        Err(Error::Mismatch("request_digest"))
    );

    assert_eq!(
        challenge.verify(&expected, &proof, &key, challenge.expires_at, POLICY),
        Err(Error::Expired)
    );
    assert_eq!(
        challenge.verify(&expected, &proof, &key, challenge.issued_at - 1, POLICY),
        Err(Error::Expired)
    );

    // Modified action, method, path or ID also fail (AUTH-02).
    let link = Expected {
        action: Action::LinkClaim,
        ..expected
    };
    assert!(challenge.verify(&link, &proof, &key, now, POLICY).is_err());
    let delete = Expected {
        method: Method::Delete,
        ..expected
    };
    assert!(
        challenge
            .verify(&delete, &proof, &key, now, POLICY)
            .is_err()
    );
    let path = Expected {
        path: "/v1/links",
        ..expected
    };
    assert!(challenge.verify(&path, &proof, &key, now, POLICY).is_err());
    let other = identity_one_key();
    assert_eq!(
        challenge.verify(&expected, &proof, &other, now, POLICY),
        Err(Error::Mismatch("ember_id"))
    );
    // The helper refuses to sign a challenge that does not match its operation.
    assert!(challenge.sign(&identity, &link, now, POLICY).is_err());
    assert!(
        challenge
            .sign(&identity, &expected, challenge.expires_at, POLICY)
            .is_err()
    );
}

fn identity_one_key() -> PublicKey {
    identity(1).public_key()
}

// player-one and player-two report signature and consistency, and agreement
#[test]
fn reports_verify_and_agree() {
    let a: SignedReport = typed("game-report-p1.json");
    let b: SignedReport = typed("game-report-p2.json");
    let ra = a.verify().unwrap();
    let rb = b.verify().unwrap();
    assert_ne!(ra.reporter_id, rb.reporter_id);
    assert!(ra.agrees_with(rb));
    assert_ne!(ra.capture_frame, rb.capture_frame);
    // Deterministic signing reproduces both fixture signatures.
    assert_eq!(ra.sign(&identity(0)).unwrap(), a);
    assert_eq!(rb.sign(&identity(1)).unwrap(), b);
    // A player cannot sign the other player's report.
    assert!(ra.sign(&identity(1)).is_err());
}

// reject altered winner, substituted signing identity, unconfirmed native
// result claim, noncanonical counter, counter overflow, cross-domain signature
#[test]
fn report_rejections() {
    let a: SignedReport = typed("game-report-p1.json");
    let b: SignedReport = typed("game-report-p2.json");

    let mut winner = a.clone();
    winner.report.result = ember_protocol::report::Outcome::P2Win;
    assert_eq!(winner.verify(), Err(Error::InvalidSignature));
    assert!(!winner.report.agrees_with(&b.report));

    let mut key = a.clone();
    key.public_key = b.public_key;
    assert_eq!(key.verify(), Err(Error::Mismatch("reporter_id")));

    let mut frame = a.clone();
    frame.report.confirmed_input_frame = Some(Counter(1));
    assert_eq!(
        frame.verify(),
        Err(Error::Mismatch("result inputs are not confirmed"))
    );

    let report_text = String::from_utf8(bytes("game-report-p1.json")).unwrap();
    let noncanonical =
        report_text.replace(r#""match_generation": "3""#, r#""match_generation": "03""#);
    assert_ne!(noncanonical, report_text);
    assert!(json::parse_as::<SignedReport>(noncanonical.as_bytes(), MAX_BODY).is_err());
    let overflow = report_text.replace(
        r#""match_generation": "3""#,
        r#""match_generation": "18446744073709551616""#,
    );
    assert!(json::parse_as::<SignedReport>(overflow.as_bytes(), MAX_BODY).is_err());

    let report_value = json::to_value(&a.report).unwrap();
    let key = PublicKey::from_b64u(&vectors().identities[0].public_key_base64url).unwrap();
    assert!(
        key.verify(Domain::GameReport, &report_value, &a.signature)
            .is_ok()
    );
    assert_eq!(
        key.verify(Domain::Challenge, &report_value, &a.signature),
        Err(Error::InvalidSignature)
    );
}

// reject duplicate JSON properties, restricted canonicalizer rejects floats
#[test]
fn strict_json_rejections() {
    assert!(json::parse(br#"{"a":1,"a":2}"#, MAX_BODY).is_err());
    assert!(json::parse(br#"{"n":1.5}"#, MAX_BODY).is_err());
    assert!(json::to_value(&serde_json::json!({ "n": 1.5 })).is_err());
    let duplicated = String::from_utf8(bytes("auth-proof.json"))
        .unwrap()
        .replacen('{', r#"{"signature":"x","#, 1);
    assert!(json::parse_as::<Proof>(duplicated.as_bytes(), MAX_BODY).is_err());
}

// FT2 completed-event score consistency
#[test]
fn completed_event_score() {
    let event: Event = typed("match-completed-event.json");
    let data: MatchCompleted =
        serde_json::from_value(serde_json::Value::Object(event.data.clone())).unwrap();
    data.check().unwrap();
    let mut two_winners = data.clone();
    two_winners.scores[1].wins = 2;
    assert!(two_winners.check().is_err());
    let mut short = data.clone();
    short.accepted_attempt_ids.pop();
    assert!(short.check().is_err());
}

// Standard Webhooks raw-body HMAC fixture, modified body, stale delivery,
// wrong secret
#[test]
fn webhook_fixture() {
    let fixture: WebhookFixture = typed("webhook-fixture.json");
    let body = bytes(&fixture.body_file);
    let secret = Secret::parse(&fixture.secret_base64).unwrap();
    let headers = Headers {
        id: fixture.headers.id,
        timestamp: fixture.headers.timestamp,
        signature: fixture.headers.signature,
    };
    let now = fixture.verification_time_unix;
    webhook::verify(&[&secret], &headers, &body, now).unwrap();
    // Our signer reproduces the fixture header.
    let signed = webhook::sign(
        &[&secret],
        &headers.id,
        headers.timestamp.parse().unwrap(),
        &body,
    )
    .unwrap();
    assert_eq!(signed, headers);

    let mut modified = body.clone();
    modified.push(b' ');
    assert_eq!(
        webhook::verify(&[&secret], &headers, &modified, now),
        Err(Error::InvalidSignature)
    );
    assert_eq!(
        webhook::verify(&[&secret], &headers, &body, now + 301),
        Err(Error::Expired)
    );
    let wrong = Secret::from_bytes([0; 32]);
    assert_eq!(
        webhook::verify(&[&wrong], &headers, &body, now),
        Err(Error::InvalidSignature)
    );
    // A legitimate retry keeps the event ID and gets a fresh timestamp.
    let retry = webhook::sign(&[&secret], &headers.id, now + 600, &body).unwrap();
    assert!(webhook::verify(&[&secret], &retry, &body, now + 600).is_ok());
    assert_eq!(retry.id, headers.id);
}

#[test]
fn proof_signature_requires_exact_length() {
    let proof: Proof = typed("auth-proof.json");
    assert!(decode_b64u::<64>(&proof.signature, "signature").is_ok());
    assert!(decode_b64u::<64>(&format!("{}A", proof.signature), "signature").is_err());
}
