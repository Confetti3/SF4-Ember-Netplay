#!/usr/bin/env python3
"""Verify SF4e VPS room capacity / relay port-range configuration.

Does not create rooms or require private credentials. Optionally queries
a broker /v1/health endpoint when --health-url is provided.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from typing import Any


DEFAULT_MAX_ROOMS = 50
DEFAULT_RELAY_PORT_BASE = 23456
DEFAULT_GGPO_UDP_PORT_BASE = 24456
MAX_PORT = 65535


def parse_positive_int(raw: str | None, name: str, fallback: int) -> int:
    if raw is None or str(raw).strip() == "":
        return fallback
    text = str(raw).strip()
    if text.startswith("-"):
        if not text[1:].isdigit():
            raise ValueError(f"{name} must be an integer (got {raw!r})")
    elif not text.isdigit():
        raise ValueError(f"{name} must be an integer (got {raw!r})")
    return int(text, 10)


def load_config(
    max_rooms: str | None,
    relay_base: str | None,
    ggpo_base: str | None,
) -> dict[str, int]:
    rooms = parse_positive_int(max_rooms, "MAX_ROOMS", DEFAULT_MAX_ROOMS)
    session_base = parse_positive_int(relay_base, "RELAY_PORT_BASE", DEFAULT_RELAY_PORT_BASE)
    ggpo = parse_positive_int(ggpo_base, "GGPO_UDP_PORT_BASE", DEFAULT_GGPO_UDP_PORT_BASE)

    if rooms < 1:
        raise ValueError(f"MAX_ROOMS must be >= 1 (got {rooms})")
    if session_base < 1:
        raise ValueError(f"RELAY_PORT_BASE must be >= 1 (got {session_base})")
    if ggpo < 1:
        raise ValueError(f"GGPO_UDP_PORT_BASE must be >= 1 (got {ggpo})")

    session_end = session_base + rooms - 1
    ggpo_end = ggpo + rooms - 1
    if session_end > MAX_PORT:
        raise ValueError(
            f"Session relay range exceeds {MAX_PORT}: {session_base}-{session_end}"
        )
    if ggpo_end > MAX_PORT:
        raise ValueError(
            f"GGPO UDP relay range exceeds {MAX_PORT}: {ggpo}-{ggpo_end}"
        )
    if session_base <= ggpo_end and ggpo <= session_end:
        raise ValueError(
            f"Session relay range {session_base}-{session_end} overlaps "
            f"GGPO UDP range {ggpo}-{ggpo_end}"
        )

    return {
        "maxRooms": rooms,
        "relayPortBase": session_base,
        "relayPortEnd": session_end,
        "ggpoUdpPortBase": ggpo,
        "ggpoUdpPortEnd": ggpo_end,
        "maxActiveMatches": rooms,
        "maxActivePlayers": rooms * 2,
        "maxClients": rooms * 4,
    }


def fetch_health(url: str, timeout: float = 10.0) -> dict[str, Any]:
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", errors="replace")
    data = json.loads(body)
    if not isinstance(data, dict):
        raise ValueError("health response is not a JSON object")
    return data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-rooms", default=os.environ.get("MAX_ROOMS"))
    parser.add_argument("--relay-port-base", default=os.environ.get("RELAY_PORT_BASE"))
    parser.add_argument("--ggpo-udp-port-base", default=os.environ.get("GGPO_UDP_PORT_BASE"))
    parser.add_argument(
        "--health-url",
        default=os.environ.get("SF4E_BROKER_HEALTH_URL", ""),
        help="Optional broker /v1/health URL to compare live config",
    )
    args = parser.parse_args(argv)

    try:
        cfg = load_config(args.max_rooms, args.relay_port_base, args.ggpo_udp_port_base)
    except ValueError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 1

    print("SF4e VPS capacity configuration")
    print(f"MAX_ROOMS:              {cfg['maxRooms']}")
    print(
        f"Session relay range:    {cfg['relayPortBase']}-{cfg['relayPortEnd']} TCP/UDP"
    )
    print(f"GGPO relay range:       {cfg['ggpoUdpPortBase']}-{cfg['ggpoUdpPortEnd']} UDP")
    print(f"Maximum active matches: {cfg['maxActiveMatches']}")
    print(f"Maximum active players: {cfg['maxActivePlayers']}")
    print(f"Maximum clients:        {cfg['maxClients']}")
    print(
        "Note: This is a configured ceiling, not a proven 50-room load result."
    )

    if not args.health_url:
        return 0

    print("Broker health:")
    try:
        health = fetch_health(args.health_url)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, ValueError, json.JSONDecodeError) as err:
        print(f"ERROR: failed to query {args.health_url}: {err}", file=sys.stderr)
        return 1

    rooms = health.get("rooms", "?")
    max_rooms = health.get("maxRooms", "?")
    print(f"Rooms: {rooms} / {max_rooms}")

    mismatches: list[str] = []
    checks = (
        ("maxRooms", cfg["maxRooms"]),
        ("relayPortBase", cfg["relayPortBase"]),
        ("relayPortEnd", cfg["relayPortEnd"]),
        ("ggpoUdpPortBase", cfg["ggpoUdpPortBase"]),
        ("ggpoUdpPortEnd", cfg["ggpoUdpPortEnd"]),
    )
    for key, expected in checks:
        actual = health.get(key)
        if actual is None:
            # Older brokers may omit end ports; derive when possible.
            if key == "relayPortEnd" and health.get("relayPortBase") is not None and health.get("maxRooms") is not None:
                actual = int(health["relayPortBase"]) + int(health["maxRooms"]) - 1
            elif key == "ggpoUdpPortEnd" and health.get("ggpoUdpPortBase") is not None and health.get("maxRooms") is not None:
                actual = int(health["ggpoUdpPortBase"]) + int(health["maxRooms"]) - 1
            else:
                mismatches.append(f"{key} missing from health response")
                continue
        if int(actual) != int(expected):
            mismatches.append(f"{key}: health={actual} expected={expected}")

    if mismatches:
        print("Configured ranges match: no")
        for item in mismatches:
            print(f"  - {item}")
        return 1

    print("Configured ranges match: yes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
