//! Test tool: mints public room tickets without a bridge, so the C++ room host
//! fixture (`src/tests/public_room_host_test.cxx`) can admit clients. Keys come
//! from a seed given on the command line; never use it for a real bridge key.
//!
//! ```text
//! room_ticket key <seed-hex>
//!     prints the ticket verification key (unpadded base64url, the form
//!     `host_public`'s `ticket_key` takes) and a key id, one per line
//! room_ticket ember-id <seed-hex>
//!     prints the Ember ID of the identity with that seed
//! room_ticket sign <seed-hex> <kid> <bridge_id> <room_id_hex32> <ember_id>
//!                  <endpoint_id_hex64> [<issued_at>]
//!     prints a SignedRoomTicket as one line of JSON, issued now by default
//! ```
use std::{
    process::ExitCode,
    time::{SystemTime, UNIX_EPOCH},
};

use ember_protocol::{
    EmberId, SigningIdentity,
    play::VERSION,
    rooms::{RoomTicket, TICKET_SECS},
};
use zeroize::Zeroizing;

/// The key id the tool names its key by. It depends on nothing but this text.
const KID: &str = "room-ticket-test-1";

fn identity(seed_hex: &str) -> Result<SigningIdentity, String> {
    let digits = seed_hex.as_bytes();
    if digits.len() != 64 {
        return Err("the seed is 64 hex characters".into());
    }
    let mut seed = Zeroizing::new([0u8; 32]);
    for (index, pair) in digits.chunks(2).enumerate() {
        let text = std::str::from_utf8(pair).map_err(|_| "the seed is hex")?;
        seed[index] = u8::from_str_radix(text, 16).map_err(|_| "the seed is hex")?;
    }
    SigningIdentity::from_seed(&seed).map_err(|error| error.to_string())
}

fn sign(arguments: &[String]) -> Result<String, String> {
    let [
        seed,
        kid,
        bridge_id,
        room_id,
        ember_id,
        endpoint_id,
        rest @ ..,
    ] = arguments
    else {
        return Err("sign takes a seed, kid, bridge id, room id, Ember ID and endpoint id".into());
    };
    let issued_at = match rest {
        [] => SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|error| error.to_string())?
            .as_secs(),
        [text] => text.parse().map_err(|_| "issued_at is unix seconds")?,
        _ => return Err("too many arguments".into()),
    };
    let ticket = RoomTicket {
        version: VERSION,
        bridge_id: bridge_id.clone(),
        room_id: room_id.clone(),
        ember_id: EmberId::parse(ember_id).map_err(|error| error.to_string())?,
        endpoint_id: endpoint_id.clone(),
        issued_at,
        expires_at: issued_at + TICKET_SECS,
    };
    let signed = ticket
        .sign(&identity(seed)?, kid)
        .map_err(|error| error.to_string())?;
    serde_json::to_string(&signed).map_err(|error| error.to_string())
}

fn run(arguments: &[String]) -> Result<String, String> {
    match arguments {
        [command, seed] if command == "key" => {
            Ok(format!("{}\n{KID}", identity(seed)?.public_key().to_b64u()))
        }
        [command, seed] if command == "ember-id" => Ok(identity(seed)?.ember_id().to_string()),
        [command, rest @ ..] if command == "sign" => sign(rest),
        _ => Err(
            "usage: room_ticket key|ember-id <seed-hex> | sign <seed-hex> <kid> <bridge_id> \
                  <room_id> <ember_id> <endpoint_id> [<issued_at>]"
                .into(),
        ),
    }
}

fn main() -> ExitCode {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    match run(&arguments) {
        Ok(text) => {
            println!("{text}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("room_ticket: {error}");
            ExitCode::FAILURE
        }
    }
}
