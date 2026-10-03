// @ember/bridge-sdk: a client for the Ember tournament bridge, webhook
// verification, and the identity and signature rules shared with Ember.
export {
  BridgeClient,
  BridgeError,
  type BridgeClientOptions,
  type FoundPlayer,
  type Lobby,
  type LobbyPlayer,
  type LobbyStanding,
  type LobbySpec,
  type MatchSpec,
  type Participant,
  type PlayerRecord,
  type RecentMatch,
  type ResolvedPlayer,
  type Rotation,
  type RoomSpec,
  type RulesProfile,
  type Tournament,
  type TournamentEntrant,
  type TournamentFormat,
  type TournamentSet,
  type TournamentSlot,
  type TournamentSpec,
} from "./client.ts";
export { canonicalize, parseStrict, StrictJsonError, type Json } from "./canonical.ts";
export {
  EVENT_TYPES,
  EventError,
  eventType,
  isRoomEvent,
  parseEvent,
  type BridgeEvent,
  type ConnectionRoom,
  type EventName,
  type RoomCloseReason,
  type RoomEvent,
  type RoomState,
  type RoomSummary,
} from "./events.ts";
export { base64url, decodeBase64url, emberIdFromPublicKey, EncodingError, fingerprint, isEmberId, verifyEd25519 } from "./identity.ts";
export { parseResult, ResultError, RESULT_TYPE, verifyResult, type MatchResult, type ResultOutcome, type ResultParticipant } from "./results.ts";
export {
  CHALLENGE_DOMAIN,
  checkChallenge,
  checkReport,
  commandDigest,
  ProofError,
  publicKeyFromSeed,
  REPORT_DOMAIN,
  signChallenge,
  signReport,
  signingBytes,
  verifyProof,
  verifyReport,
  type Challenge,
  type Expected,
  type Proof,
  type SignedReport,
} from "./signed.ts";
export {
  TestPlayer,
  type Binding,
  type ClaimAnswer,
  type GameResult,
  type LinkClaim,
  type Permit,
  type PrepareAnswer,
  type ReportReceipt,
  type TestPlayerOptions,
} from "./testing.ts";
export { signWebhook, TOLERANCE_SECONDS, verifyWebhook, WebhookError, type WebhookHeaders } from "./webhook.ts";
