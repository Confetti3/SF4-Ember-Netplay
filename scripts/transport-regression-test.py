#!/usr/bin/env python3
"""Automated transport regression checks (broker + relay infra). See docs/TRANSPORT_REGRESSION.md."""

import hashlib
import json
import os
import socket
import ssl
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

BROKER = os.environ.get("SF4E_BROKER_URL", "https://74-208-200-95.nip.io")
HOST = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
ROOT = Path(__file__).resolve().parents[1]
PACKAGE_DIR = Path(os.environ.get("SF4E_PACKAGE_DIR", ROOT / "msvc-out" / "relwithdebinfo"))


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


def sidecar_hash() -> str:
    path = PACKAGE_DIR / "Sidecar.dll"
    if not path.is_file():
        return "0" * 64
    return hashlib.sha256(path.read_bytes()).hexdigest()


class BrokerClient:
    def __init__(self, base: str):
        self.base = base.rstrip("/")
        self.ctx = ssl.create_default_context()

    def request(self, method: str, path: str, body=None, headers=None):
        req = urllib.request.Request(self.base + path, method=method, headers=headers or {})
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, data=data, context=self.ctx, timeout=20) as r:
            return json.loads(r.read().decode("utf-8"))

    def get(self, path: str):
        return self.request("GET", path)

    def post(self, path: str, body=None):
        return self.request("POST", path, body=body)


def nat_probe() -> dict:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(b"SF4E_PROBE", (HOST, 8790))
        data, _ = s.recvfrom(4096)
        return json.loads(data.decode("utf-8"))
    finally:
        s.close()


def ggpo_register(ggpo_port: int, room_token: str, local_ggpo_port: int = 23457) -> str:
    packet = b"SF4G" + room_token.encode("ascii")
    packet += bytes([(local_ggpo_port >> 8) & 0xFF, local_ggpo_port & 0xFF])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(packet, (HOST, ggpo_port))
        data, _ = s.recvfrom(64)
        return data.decode("ascii", errors="replace")
    except socket.timeout:
        return "timeout"
    finally:
        s.close()


def ggpo_relay_forward_test(ggpo_port: int, room_token: str) -> bool:
    """Two local clients register with distinct GGPO ports; verify bidirectional relay."""
    token = room_token.encode("ascii")
    reg_a = b"SF4G" + token + bytes([(23457 >> 8) & 0xFF, 23457 & 0xFF])
    reg_b = b"SF4G" + token + bytes([(23458 >> 8) & 0xFF, 23458 & 0xFF])
    payload_a = b"GGPO_TEST_A"
    payload_b = b"GGPO_TEST_B"

    sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sa.settimeout(3)
    sb.settimeout(3)
    try:
        sa.bind(("0.0.0.0", 23457))
        sb.bind(("0.0.0.0", 23458))
        dest = (HOST, ggpo_port)
        sa.sendto(reg_a, dest)
        ra, _ = sa.recvfrom(64)
        sb.sendto(reg_b, dest)
        rb, _ = sb.recvfrom(64)
        if not (ra.startswith(b"SF4R") or ra.startswith(b"SF4W")):
            return False
        if not rb.startswith(b"SF4R"):
            return False
        sa.sendto(payload_a, dest)
        sb.sendto(payload_b, dest)
        fb, _ = sb.recvfrom(256)
        fa, _ = sa.recvfrom(256)
        return fb == payload_a and fa == payload_b
    except OSError:
        return False
    finally:
        sa.close()
        sb.close()


def ggpo_relay_rematch_registration_test(ggpo_port: int, room_token: str) -> bool:
    """After both slots are filled, a new probe socket must still get SF4R/SF4W (rematch)."""
    token = room_token.encode("ascii")
    reg_a = b"SF4G" + token + bytes([(23457 >> 8) & 0xFF, 23457 & 0xFF])
    reg_b = b"SF4G" + token + bytes([(23458 >> 8) & 0xFF, 23458 & 0xFF])
    reg_rematch = b"SF4G" + token + bytes([(23457 >> 8) & 0xFF, 23457 & 0xFF])

    sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rematch = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sa.settimeout(3)
    sb.settimeout(3)
    rematch.settimeout(3)
    try:
        dest = (HOST, ggpo_port)
        sa.bind(("0.0.0.0", 23457))
        sb.bind(("0.0.0.0", 23458))
        sa.sendto(reg_a, dest)
        ra, _ = sa.recvfrom(64)
        sb.sendto(reg_b, dest)
        rb, _ = sb.recvfrom(64)
        if not (ra.startswith(b"SF4R") or ra.startswith(b"SF4W")):
            return False
        if not (rb.startswith(b"SF4R") or rb.startswith(b"SF4W")):
            return False
        rematch.sendto(reg_rematch, dest)
        rr, _ = rematch.recvfrom(64)
        return rr.startswith(b"SF4R") or rr.startswith(b"SF4W")
    except (OSError, socket.timeout):
        return False
    finally:
        sa.close()
        sb.close()
        rematch.close()


def run_checks() -> list[Check]:
    checks: list[Check] = []
    broker = BrokerClient(BROKER)
    room = None
    secret = ""

    try:
        health = broker.get("/v1/health")
        transport = health.get("brokerGgpoTransport")
        checks.append(
            Check(
                "Broker health",
                health.get("ok") is True,
                f"transport={transport}",
            )
        )
        checks.append(
            Check(
                "VPS transport mode is auto",
                transport == "auto",
                f"got {transport!r}; set BROKER_GGPO_TRANSPORT=auto on VPS",
            )
        )

        probe = nat_probe()
        checks.append(
            Check(
                "NAT probe :8790/udp",
                "observedIp" in probe,
                str(probe),
            )
        )

        created = broker.post(
            "/v1/rooms",
            {"displayName": "TransportTest", "sidecarHash": sidecar_hash()},
        )
        room = created.get("code")
        secret = created.get("hostSecret", "")
        session_port = int(created.get("port", 0))
        ggpo_port = int(created.get("ggpoPort", 0))
        room_token = created.get("roomToken", "")
        checks.append(Check("Room create", bool(room and session_port), f"{room} session={session_port}"))
        checks.append(
            Check(
                "GGPO port allocated",
                ggpo_port >= 24456,
                f"ggpoPort={ggpo_port}",
            )
        )

        rh = broker.get(f"/v1/rooms/{room}/health")
        checks.append(
            Check(
                "Session relay listening",
                rh.get("relayOk") is True,
                f"relayOk={rh.get('relayOk')}",
            )
        )
        checks.append(
            Check(
                "GGPO relay listening",
                rh.get("ggpoRelayOk") is True,
                f"ggpoRelayOk={rh.get('ggpoRelayOk')}",
            )
        )

        plan_host = broker.get(f"/v1/rooms/{room}/connect-plan?role=host")
        transport_plan = plan_host.get("transport")
        checks.append(
            Check(
                "Connect plan (host, pre-register)",
                transport_plan in ("udp_relay", "legacy_session_tunnel", "p2p"),
                f"transport={transport_plan} ggpoPort={plan_host.get('ggpoPort')}",
            )
        )
        checks.append(
            Check(
                "Connect plan prefers UDP relay",
                transport_plan == "udp_relay" and plan_host.get("ggpoRemotePort") == ggpo_port,
                f"remote={plan_host.get('ggpoRemoteHost')}:{plan_host.get('ggpoRemotePort')}",
            )
        )
        plan_token = plan_host.get("roomToken", "")
        checks.append(
            Check(
                "Connect plan includes roomToken",
                len(plan_token) == 32 and all(c in "0123456789abcdef" for c in plan_token.lower()),
                f"token={plan_token[:8]}..." if plan_token else "missing (deploy broker server.js)",
            )
        )

        reg_host = broker.post(
            f"/v1/rooms/{room}/register-endpoint",
            {
                "role": "host",
                "hostSecret": secret,
                "ggpoPort": 23457,
                "natType": "unknown",
            },
        )
        checks.append(
            Check(
                "Register host endpoint",
                reg_host.get("ok") is True,
                f"role=host",
            )
        )

        reg_guest = broker.post(
            f"/v1/rooms/{room}/register-endpoint",
            {"role": "guest", "ggpoPort": 23458, "natType": "unknown"},
        )
        checks.append(
            Check(
                "Register guest endpoint",
                reg_guest.get("ok") is True,
                f"role=guest",
            )
        )

        plan_after = broker.get(f"/v1/rooms/{room}/connect-plan?role=host")
        post_transport = plan_after.get("transport")
        checks.append(
            Check(
                "Connect plan after both endpoints",
                post_transport in ("p2p", "udp_relay"),
                f"transport={post_transport}",
            )
        )

        if room_token and ggpo_port:
            ggpo_resp = ggpo_register(ggpo_port, room_token, 23457)
            checks.append(
                Check(
                    "GGPO UDP relay registration (v2)",
                    ggpo_resp.startswith("SF4R") or ggpo_resp.startswith("SF4W"),
                    f"response={ggpo_resp!r}",
                )
            )
            forward_ok = ggpo_relay_forward_test(ggpo_port, room_token)
            checks.append(
                Check(
                    "GGPO UDP relay bidirectional forward",
                    forward_ok,
                    "ports 23457/23458 via declared registration",
                )
            )
            rematch_ok = ggpo_relay_rematch_registration_test(ggpo_port, room_token)
            checks.append(
                Check(
                    "GGPO UDP relay rematch re-registration",
                    rematch_ok,
                    "ephemeral probe after both slots full",
                )
            )

        hb = broker.post(
            f"/v1/rooms/{room}/heartbeat",
            {"hostSecret": secret},
        )
        checks.append(
            Check(
                "Heartbeat with host secret",
                hb.get("heartbeatOk") is True,
                f"relayListening={hb.get('relayListening')}",
            )
        )

        evt = broker.post(
            f"/v1/rooms/{room}/events",
            {
                "type": "transport_fallback",
                "hostSecret": secret,
                "transportActive": "legacy_session_tunnel",
                "detail": {"from": "udp_relay", "reason": "automated_test"},
            },
        )
        checks.append(
            Check(
                "transport_fallback event",
                evt.get("ok") is True,
                "posted to match coordinator",
            )
        )

    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        checks.append(Check("HTTP error", False, f"{e.code} {body[:300]}"))
    except Exception as e:
        checks.append(Check("Unexpected error", False, str(e)))
    finally:
        if room and secret:
            try:
                req = urllib.request.Request(
                    BROKER + f"/v1/rooms/{room}",
                    method="DELETE",
                    data=json.dumps({"hostSecret": secret}).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, context=ssl.create_default_context(), timeout=15):
                    pass
            except Exception:
                pass

    return checks


def main() -> int:
    print(f"Transport regression (automated infra checks)")
    print(f"Broker: {BROKER}")
    print(f"VPS:    {HOST}")
    print()

    checks = run_checks()
    passed = 0
    for c in checks:
        status = "PASS" if c.ok else "FAIL"
        suffix = f" — {c.detail}" if c.detail else ""
        print(f"[{status}] {c.name}{suffix}")
        if c.ok:
            passed += 1

    print()
    print(f"Result: {passed}/{len(checks)} passed")
    if passed == len(checks):
        print("Infra checks OK. Run manual 2-player match tests (rows 1-5 in TRANSPORT_REGRESSION.md).")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
