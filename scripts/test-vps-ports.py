#!/usr/bin/env python3
"""Probe SF4e VPS ports from the current machine (control + relay + NAT probe)."""

import hashlib
import json
import os
import socket
import ssl
import sys
import urllib.error
import urllib.request

BROKER = os.environ.get("SF4E_BROKER_URL", "https://74-208-200-95.nip.io")
HOST = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
PACKAGE_DIR = os.environ.get(
    "SF4E_PACKAGE_DIR",
    os.path.join(os.path.dirname(__file__), "..", "msvc-out", "relwithdebinfo"),
)


def sidecar_hash() -> str:
    path = os.path.join(PACKAGE_DIR, "Sidecar.dll")
    if not os.path.isfile(path):
        return "0" * 64
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def http(method: str, path: str, body=None, headers=None):
    ctx = ssl.create_default_context()
    req = urllib.request.Request(BROKER + path, method=method, headers=headers or {})
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, data=data, context=ctx, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def test_tcp(port: int, timeout: float = 3.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((HOST, port))
        return "open"
    except socket.timeout:
        return "timeout"
    except ConnectionRefusedError:
        return "refused"
    except OSError as e:
        return f"error:{e.errno}"
    finally:
        s.close()


def test_udp(port: int, payload: bytes = b"\x00", timeout: float = 2.0) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(payload, (HOST, port))
        try:
            data, _ = s.recvfrom(4096)
            return f"reply:{data[:120]!r}"
        except socket.timeout:
            return "sent_no_reply"
    except OSError as e:
        return f"error:{e.errno}"
    finally:
        s.close()


def nat_probe(timeout: float = 3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b"SF4E_PROBE", (HOST, 8790))
        data, _ = s.recvfrom(4096)
        return json.loads(data.decode("utf-8"))
    except Exception as e:
        return {"error": str(e)}
    finally:
        s.close()


def main() -> int:
    print(f"Broker: {BROKER}")
    print(f"VPS IP: {HOST}")
    print()

    print("=== CONTROL PLANE ===")
    try:
        health = http("GET", "/v1/health")
    except Exception as e:
        print(f"FAIL broker health: {e}")
        return 1

    print(json.dumps(health, indent=2))
    ok_https = True
    print(f"HTTPS :443 -> {test_tcp(443)}")
    print(f"HTTP  :80  -> {test_tcp(80)}")
    print()

    print("=== ADMIN PORTS (expect blocked) ===")
    for p in (8787, 8788, 8789):
        result = test_tcp(p)
        blocked = result in ("timeout", "refused")
        print(f"tcp {p:5} -> {result} {'OK' if blocked else 'WARN (unexpectedly open)'}")
    print()

    print("=== RELAY PORTS (create temp room) ===")
    try:
        created = http(
            "POST",
            "/v1/rooms",
            {"displayName": "PortTest", "sidecarHash": sidecar_hash()},
        )
    except Exception as e:
        print(f"FAIL room create: {e}")
        return 1

    code = created.get("code")
    port = int(created.get("port", 0))
    ggpo_port = int(created.get("ggpoPort", 0))
    secret = created.get("hostSecret", "")
    print(f"Room {code} sessionPort={port} ggpoPort={ggpo_port}")

    if secret:
        try:
            hb = http(
                "POST",
                f"/v1/rooms/{code}/heartbeat",
                {"hostSecret": secret},
            )
            print(f"Heartbeat: relayListening={hb.get('relayListening')} heartbeatOk={hb.get('heartbeatOk')}")
        except urllib.error.HTTPError as e:
            print(f"Heartbeat HTTP {e.code}: {e.read().decode('utf-8', errors='replace')[:200]}")
        except Exception as e:
            print(f"Heartbeat error: {e}")

        try:
            rh = http("GET", f"/v1/rooms/{code}/health")
            print(f"Room health: relayOk={rh.get('relayOk')} ggpoRelayOk={rh.get('ggpoRelayOk')}")
        except Exception as e:
            print(f"Room health error: {e}")

    session_tcp = test_tcp(port)
    session_udp = test_udp(port)
    print(f"session tcp {port} -> {session_tcp}")
    print(f"session udp {port} -> {session_udp}")

    ggpo_base = int(health.get("ggpoUdpPortBase", 24456))
    for gp in (ggpo_base, ggpo_base + 1, ggpo_base + 19):
        print(f"ggpo  udp {gp} -> {test_udp(gp)}")

    print()
    print("=== NAT PROBE (8790/udp, expect JSON reply) ===")
    probe = nat_probe()
    print(probe)
    nat_ok = "observedIp" in probe
    print(f"NAT probe: {'PASS' if nat_ok else 'FAIL'}")

    if secret:
        try:
            req = urllib.request.Request(
                BROKER + f"/v1/rooms/{code}",
                method="DELETE",
                data=json.dumps({"hostSecret": secret}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            ctx = ssl.create_default_context()
            with urllib.request.urlopen(req, context=ctx, timeout=10) as r:
                print(f"Cleanup DELETE: HTTP {r.status}")
        except Exception as e:
            print(f"Cleanup error: {e}")

    print()
    print("=== SUMMARY ===")
    relay_udp_ok = session_udp in ("sent_no_reply",)  # no reply is normal for GNS
    relay_tcp_ok = session_tcp in ("open", "refused")  # refused may mean UDP-only listener
    admin_ok = all(test_tcp(p) in ("timeout", "refused") for p in (8787, 8788, 8789))

    rows = [
        ("HTTPS broker (:443)", ok_https and health.get("ok")),
        ("Admin ports blocked", admin_ok),
        ("Session relay UDP reachable", relay_udp_ok),
        ("Session relay TCP", session_tcp),
        ("NAT probe :8790/udp", nat_ok),
        ("GGPO UDP range (24456+)", "see sent_no_reply above; no process until auto transport"),
    ]
    for name, status in rows:
        print(f"  {name}: {status}")

    if not nat_ok:
        print()
        print("ACTION: Open UDP 8790 and the GGPO range (default 24456-24505 for MAX_ROOMS=50) in the provider cloud firewall (ufw alone is not enough).")

    return 0 if health.get("ok") and relay_udp_ok else 1


if __name__ == "__main__":
    sys.exit(main())
