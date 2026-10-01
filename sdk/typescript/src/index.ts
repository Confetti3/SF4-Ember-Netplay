// @ember/bridge-sdk: a client for the Ember tournament bridge, webhook
// verification, and the identity and signature rules shared with Ember.
export {
  BridgeClient,
  BridgeError,
  type BridgeClientOptions,
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
} from "./client.ts";
export { canonicalize, parseStrict, StrictJsonError, type Json } from "./canonical.ts";
export { EVENT_TYPES, EventError, eventType, parseEvent, type BridgeEvent, type EventName } from "./events.ts";
export { base64url, decodeBase64url, emberIdFromPublicKey, EncodingError, fingerprint, isEmberId, verifyEd25519 } from "./identity.ts";
export {
  CHALLENGE_DOMAIN,
  checkChallenge,
  checkReport,
  commandDigest,
  ProofError,
  publicKeyFromSeed,
  REPORT_DOMAIN,
  signChallenge,
  signingBytes,
  verifyProof,
  verifyReport,
  type Challenge,
  type Expected,
  type Proof,
  type SignedReport,
} from "./signed.ts";
export { signWebhook, TOLERANCE_SECONDS, verifyWebhook, WebhookError, type WebhookHeaders } from "./webhook.ts";
