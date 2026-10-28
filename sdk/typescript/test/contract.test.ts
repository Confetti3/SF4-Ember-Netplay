// The SDK keeps its own lists of the protocol's event names, room objects and enums. This holds them to the
// protocol: server/ember/protocol/examples/contract.rs prints what the Rust side has, and each list here
// must match it. The contract comes from, in this order:
//   EMBER_PROTOCOL_CONTRACT          a file holding its output
//   EMBER_PROTOCOL_CONTRACT_BINARY   the built example
//   the example beside EMBER_BRIDGE_TEST_BINARY (the same cargo target folder's examples folder), built here
//   with cargo (CARGO, or cargo on PATH) when it is not there yet
// Like the other required prerequisites, a missing contract fails the test rather than skipping it.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { test } from "node:test";

import { GAMES_TO_WIN_MAX, ROTATIONS, TOURNAMENT_FORMATS } from "../src/client.ts";
import {
  CONNECTION_ROOM_KEYS,
  EVENT_FIELDS,
  EVENT_TYPES,
  ROOM_CLOSE_REASONS,
  ROOM_STATES,
  ROOM_SUMMARY_KEYS,
} from "../src/events.ts";
import { binary } from "./bridge-process.ts";

interface Contract {
  event_types: string[];
  event_fields: string[];
  room_states: string[];
  room_close_reasons: string[];
  rotations: string[];
  tournament_formats: string[];
  max_games_to_win: number;
  shapes: { [name: string]: string[] };
}

function loadContract(): Contract {
  const file = process.env.EMBER_PROTOCOL_CONTRACT ?? "";
  if (file) return JSON.parse(readFileSync(file, "utf8")) as Contract;
  const name = process.platform === "win32" ? "contract.exe" : "contract";
  const beside = binary ? join(dirname(binary), "examples", name) : "";
  const example = process.env.EMBER_PROTOCOL_CONTRACT_BINARY || beside;
  if (example === beside && beside && !existsSync(beside)) {
    // The bridge was built into <target>/debug/ember-bridge; build the example into the same target folder.
    try {
      execFileSync(
        process.env.CARGO || "cargo",
        ["build", "--locked", "--manifest-path", resolve(import.meta.dirname, "../../../server/ember/Cargo.toml"), "-p", "ember-protocol", "--example", "contract"],
        { env: { ...process.env, CARGO_TARGET_DIR: dirname(dirname(binary)) }, stdio: "pipe" },
      );
    } catch {
      // The error below says what to do.
    }
  }
  if (!example || !existsSync(example)) {
    throw new Error(
      "The protocol contract is missing. Build it with `cargo build --locked -p ember-protocol --example contract` in server/ember " +
        "(the same CARGO_TARGET_DIR as the bridge), or set EMBER_PROTOCOL_CONTRACT_BINARY or EMBER_PROTOCOL_CONTRACT.",
    );
  }
  return JSON.parse(execFileSync(example, { encoding: "utf8" })) as Contract;
}

const contract = loadContract();
const sorted = (names: readonly string[]) => [...names].sort();

test("the event families are the protocol's, in its order", () => {
  assert.deepEqual([...EVENT_TYPES], contract.event_types);
});

test("an event has exactly the protocol's envelope keys", () => {
  assert.deepEqual(sorted(EVENT_FIELDS), contract.event_fields);
});

test("room objects have exactly the protocol's keys", () => {
  assert.deepEqual(sorted(ROOM_SUMMARY_KEYS), contract.shapes.RoomSummary);
  assert.deepEqual(sorted(CONNECTION_ROOM_KEYS), contract.shapes.ConnectionRoom);
});

test("room states and close reasons are the protocol's", () => {
  assert.deepEqual([...ROOM_STATES], contract.room_states);
  assert.deepEqual([...ROOM_CLOSE_REASONS], contract.room_close_reasons);
});

test("lobby rotations, tournament formats and the set length limit are the protocol's", () => {
  assert.deepEqual([...ROTATIONS], contract.rotations);
  assert.deepEqual([...TOURNAMENT_FORMATS], contract.tournament_formats);
  assert.equal(GAMES_TO_WIN_MAX, contract.max_games_to_win);
});
