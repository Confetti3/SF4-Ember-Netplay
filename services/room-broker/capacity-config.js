/**
 * Shared VPS room capacity / relay port-range configuration.
 * Used by the broker and by operator verification tooling.
 */

const DEFAULT_MAX_ROOMS = 50;
const DEFAULT_RELAY_PORT_BASE = 23456;
const DEFAULT_GGPO_UDP_PORT_BASE = 24456;
const DEFAULT_ROOM_CAPACITY_WARNING_PERCENT = 80;
const MAX_PORT = 65535;

/**
 * @param {string|undefined} raw
 * @param {string} name
 * @param {number} fallback
 * @returns {number}
 */
function parsePositiveInt(raw, name, fallback) {
  if (raw === undefined || raw === null || String(raw).trim() === "") {
    return fallback;
  }
  const text = String(raw).trim();
  if (!/^-?\d+$/.test(text)) {
    throw new Error(`${name} must be an integer (got ${JSON.stringify(raw)})`);
  }
  const value = Number.parseInt(text, 10);
  if (!Number.isFinite(value)) {
    throw new Error(`${name} must be a finite integer (got ${JSON.stringify(raw)})`);
  }
  return value;
}

/**
 * Inclusive ranges [aStart, aEnd] and [bStart, bEnd] overlap?
 * @param {number} aStart
 * @param {number} aEnd
 * @param {number} bStart
 * @param {number} bEnd
 */
function rangesOverlap(aStart, aEnd, bStart, bEnd) {
  return aStart <= bEnd && bStart <= aEnd;
}

/**
 * @param {NodeJS.ProcessEnv|Record<string, string|undefined>} [env]
 */
function loadCapacityConfig(env = process.env) {
  const maxRooms = parsePositiveInt(env.MAX_ROOMS, "MAX_ROOMS", DEFAULT_MAX_ROOMS);
  const relayPortBase = parsePositiveInt(
    env.RELAY_PORT_BASE,
    "RELAY_PORT_BASE",
    DEFAULT_RELAY_PORT_BASE
  );
  const ggpoUdpPortBase = parsePositiveInt(
    env.GGPO_UDP_PORT_BASE,
    "GGPO_UDP_PORT_BASE",
    DEFAULT_GGPO_UDP_PORT_BASE
  );
  const roomCapacityWarningPercent = parsePositiveInt(
    env.ROOM_CAPACITY_WARNING_PERCENT,
    "ROOM_CAPACITY_WARNING_PERCENT",
    DEFAULT_ROOM_CAPACITY_WARNING_PERCENT
  );

  if (maxRooms < 1) {
    throw new Error(`MAX_ROOMS must be >= 1 (got ${maxRooms})`);
  }
  if (relayPortBase < 1) {
    throw new Error(`RELAY_PORT_BASE must be >= 1 (got ${relayPortBase})`);
  }
  if (ggpoUdpPortBase < 1) {
    throw new Error(`GGPO_UDP_PORT_BASE must be >= 1 (got ${ggpoUdpPortBase})`);
  }
  if (roomCapacityWarningPercent < 1 || roomCapacityWarningPercent > 100) {
    throw new Error(
      `ROOM_CAPACITY_WARNING_PERCENT must be between 1 and 100 (got ${roomCapacityWarningPercent})`
    );
  }

  const relayPortEnd = relayPortBase + maxRooms - 1;
  const ggpoUdpPortEnd = ggpoUdpPortBase + maxRooms - 1;

  if (relayPortEnd > MAX_PORT) {
    throw new Error(
      `Session relay range exceeds port ${MAX_PORT}: ${relayPortBase}-${relayPortEnd} (MAX_ROOMS=${maxRooms})`
    );
  }
  if (ggpoUdpPortEnd > MAX_PORT) {
    throw new Error(
      `GGPO UDP relay range exceeds port ${MAX_PORT}: ${ggpoUdpPortBase}-${ggpoUdpPortEnd} (MAX_ROOMS=${maxRooms})`
    );
  }
  if (rangesOverlap(relayPortBase, relayPortEnd, ggpoUdpPortBase, ggpoUdpPortEnd)) {
    throw new Error(
      `Session relay range ${relayPortBase}-${relayPortEnd} overlaps GGPO UDP range ${ggpoUdpPortBase}-${ggpoUdpPortEnd}`
    );
  }

  return {
    maxRooms,
    relayPortBase,
    relayPortEnd,
    ggpoUdpPortBase,
    ggpoUdpPortEnd,
    roomCapacityWarningPercent,
    maxActiveMatches: maxRooms,
    maxActivePlayers: maxRooms * 2,
    maxClients: maxRooms * 4,
  };
}

/**
 * Allocate the next free session/GGPO port pair.
 * Marks sessionPort in reservedPorts until the caller releases it.
 *
 * @param {{
 *   maxRooms: number,
 *   relayPortBase: number,
 *   ggpoUdpPortBase: number,
 * }} config
 * @param {Iterable<{port: number}>} rooms
 * @param {Set<number>} reservedPorts
 * @returns {{ sessionPort: number, ggpoPort: number }}
 */
function allocatePortPair(config, rooms, reservedPorts) {
  const usedSession = new Set([...rooms].map((r) => r.port));
  for (let i = 0; i < config.maxRooms; i++) {
    const port = config.relayPortBase + i;
    if (!usedSession.has(port) && !reservedPorts.has(port)) {
      reservedPorts.add(port);
      return {
        sessionPort: port,
        ggpoPort: config.ggpoUdpPortBase + i,
      };
    }
  }
  return { sessionPort: 0, ggpoPort: 0 };
}

/**
 * @param {Set<number>} reservedPorts
 * @param {number} port
 */
function releasePortReservation(reservedPorts, port) {
  if (port) {
    reservedPorts.delete(port);
  }
}

/**
 * @param {number} sessionPort
 * @param {{ maxRooms: number, relayPortBase: number, ggpoUdpPortBase: number }} config
 */
function ggpoPortForSessionPort(sessionPort, config) {
  const index = sessionPort - config.relayPortBase;
  if (index < 0 || index >= config.maxRooms) {
    return 0;
  }
  return config.ggpoUdpPortBase + index;
}

module.exports = {
  DEFAULT_MAX_ROOMS,
  DEFAULT_RELAY_PORT_BASE,
  DEFAULT_GGPO_UDP_PORT_BASE,
  DEFAULT_ROOM_CAPACITY_WARNING_PERCENT,
  MAX_PORT,
  parsePositiveInt,
  rangesOverlap,
  loadCapacityConfig,
  allocatePortPair,
  releasePortReservation,
  ggpoPortForSessionPort,
};
