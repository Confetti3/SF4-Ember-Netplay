#!/usr/bin/env python3
"""Internal rollback benchmark — see docs/ROLLBACK_BENCHMARK.md."""
import argparse
import csv
import hashlib
import json
import os
import platform
import re
import socket
import ssl
import statistics
import subprocess
import sys
import time
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BROKER = os.environ.get("SF4E_BROKER_URL", "https://74-208-200-95.nip.io")
HOST = os.environ.get("SF4E_VPS_HOST", "74.208.200.95")
PACKAGE_DIR = Path(os.environ.get("SF4E_PACKAGE_DIR", ROOT / "msvc-out" / "relwithdebinfo"))
RESULTS_DIR = ROOT / "benchmark-results"
CRITICAL_PROBES = ("Broker health", "GGPO port", "Transport regression")
FIELDS = [
    "scenario_id",
    "match_id",
    "input_delay",
    "transport_active",
    "rtt_ms",
    "lfb_max",
    "rfb_max",
    "lfb_avg",
    "rfb_avg",
    "subjective_rollbacks",
    "desync",
    "notes",
]


@dataclass
class Probe:
    name: str
    ok: bool
    value: str = ""
    detail: str = ""
    critical: bool = False


def sidecar_hash():
    p = PACKAGE_DIR / "Sidecar.dll"
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else "0" * 64


class Broker:
    def __init__(self, base):
        self.base = base.rstrip("/")
        self.ctx = ssl.create_default_context()

    def get(self, path):
        r = urllib.request.Request(self.base + path, method="GET")
        with urllib.request.urlopen(r, context=self.ctx, timeout=20) as res:
            return json.loads(res.read().decode())

    def post(self, path, body=None):
        data = json.dumps(body).encode() if body else None
        r = urllib.request.Request(self.base + path, data=data, method="POST")
        if data:
            r.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(r, context=self.ctx, timeout=20) as res:
            return json.loads(res.read().decode())


def ping_ms(host, n=4):
    flag = "-n" if platform.system().lower().startswith("win") else "-c"
    p = subprocess.run(
        ["ping", flag, str(n), host],
        capture_output=True,
        text=True,
        timeout=30,
    )
    text = p.stdout + p.stderr
    times = [
        float(x)
        for x in re.findall(r"(?:time[=<]\s*)(\d+(?:\.\d+)?)\s*ms", text, re.I)
    ]
    if not times:
        avg_m = re.search(
            r"Average\s*=\s*(\d+(?:\.\d+)?)\s*ms", text, re.I
        )
        if avg_m:
            return float(avg_m.group(1)), "avg from summary"
    if times:
        return statistics.mean(times), f"min={min(times):.0f} max={max(times):.0f}"
    return None, "parse fail (ICMP optional; NAT probe used for RTT estimate)"


def nat_probe(host):
    rtts, last = [], {}
    for _ in range(5):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        try:
            t0 = time.perf_counter()
            s.sendto(b"SF4E_PROBE", (host, 8790))
            d, _ = s.recvfrom(4096)
            rtts.append((time.perf_counter() - t0) * 1000)
            last = json.loads(d.decode())
        except Exception as e:
            last = {"error": str(e)}
        finally:
            s.close()
    return last, (statistics.mean(rtts) if rtts else None)


def eff_rtt(ping, nat):
    b = max(ping or 0, nat or 0)
    return b * 2 if b > 0 else 80.0


def presets(eff):
    return {
        "responsive": max(2, min(8, round(eff / 75))),
        "standard": max(2, min(8, round(eff / 50))),
        "smooth": max(3, min(10, round(eff / 40))),
    }


def pick_preset(p, transport):
    if transport == "auto" and p["smooth"] > p["standard"]:
        return p["smooth"], "smooth"
    return p["standard"], "standard"


def run_probe():
    probes = []
    ping, pd = ping_ms(HOST)
    probes.append(
        Probe(
            "ICMP VPS",
            ping is not None,
            f"{ping:.1f}ms" if ping else "",
            pd,
            critical=False,
        )
    )
    nat, nr = nat_probe(HOST)
    probes.append(
        Probe(
            "NAT :8790",
            "observedIp" in nat,
            f"rtt~{nr:.1f}" if nr else "",
            str(nat),
            critical=False,
        )
    )
    health = {}
    try:
        health = Broker(BROKER).get("/v1/health")
        probes.append(
            Probe(
                "Broker health",
                bool(health.get("ok")),
                f"transport={health.get('brokerGgpoTransport')}",
                "",
                critical=True,
            )
        )
    except Exception as e:
        probes.append(Probe("Broker health", False, detail=str(e), critical=True))
    try:
        c = Broker(BROKER).post(
            "/v1/rooms",
            {"displayName": "RollbackBench", "sidecarHash": sidecar_hash()},
        )
        gp = int(c.get("ggpoPort", 0))
        probes.append(
            Probe(
                "GGPO port",
                gp >= 24456,
                f"{c.get('code')} {gp}",
                "",
                critical=True,
            )
        )
    except Exception as e:
        probes.append(Probe("GGPO port", False, detail=str(e), critical=True))
    tr = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "transport-regression-test.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=120,
    )
    trline = next(
        (
            ln
            for ln in reversed((tr.stdout or "").splitlines())
            if "passed" in ln.lower()
        ),
        f"exit={tr.returncode}",
    )
    ok_tr = "passed" in trline.lower() and "fail" not in trline.lower()
    probes.append(
        Probe("Transport regression", ok_tr, trline, "", critical=True)
    )
    eff = eff_rtt(ping, nr)
    pr = presets(eff)
    d, name = pick_preset(pr, str(health.get("brokerGgpoTransport", "auto")))
    return probes, ping, nat, health, pr, d, name, eff, nr


def probe_status_label(p: Probe) -> str:
    if p.ok:
        return "PASS"
    if p.critical:
        return "FAIL"
    return "WARN"


def cmd_probe(args):
    probes, ping, nat, health, pr, d, name, eff, nr = run_probe()
    print("SF4e rollback benchmark")
    print("  Broker:", BROKER)
    print("  VPS:", HOST)
    for p in probes:
        print(f"[{probe_status_label(p)}] {p.name} {p.value}".rstrip())
        if p.detail and not p.ok:
            print(f"         {p.detail}")
    print(f"\nEst. udp_relay RTT ~{eff:.0f} ms", end="")
    if ping is None and nr:
        print(f" (from NAT probe ~{nr:.0f} ms one-way)")
    else:
        print()
    for k, v in pr.items():
        mark = " *" if k == name else ""
        print(f"  {k:12} {v} frames{mark}")
    print(f"\nSuggested host input delay: {d} ({name})")
    print("Log fights: template -> analyze (LFB/RFB from bottom overlay)")
    rep = {
        "when": datetime.now(timezone.utc).isoformat(),
        "ping": ping,
        "nat": nat,
        "nat_rtt_ms": nr,
        "health": health,
        "presets": pr,
        "recommended_delay": d,
        "recommended_preset": name,
        "estimated_rtt_ms": eff,
        "probes": [asdict(p) for p in probes],
    }
    out = (
        Path(args.json)
        if args.json
        else RESULTS_DIR
        / f"probe-{datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')}.json"
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rep, indent=2), encoding="utf-8")
    print("JSON:", out.resolve())
    if any(not p.ok for p in probes if p.name in CRITICAL_PROBES):
        return 1
    return 0


def cmd_template(out):
    out.parent.mkdir(parents=True, exist_ok=True)
    new = not out.exists()
    with out.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
            w.writerow(
                {
                    "scenario_id": "D",
                    "match_id": "1",
                    "input_delay": "2",
                    "transport_active": "udp_relay",
                    "rtt_ms": "85",
                    "lfb_max": "1",
                    "rfb_max": "2",
                    "lfb_avg": "0",
                    "rfb_avg": "1",
                    "subjective_rollbacks": "few",
                    "desync": "no",
                    "notes": "example — replace with real match peaks",
                }
            )
    print("Template:", out.resolve())
    return 0


def load_probe_json(path: Path | None):
    if not path or not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def cmd_analyze(path, scenario_id, probe_json):
    rows = list(csv.DictReader(path.open(encoding="utf-8")))
    sid = (scenario_id or "D").strip().upper()
    filtered = [
        r
        for r in rows
        if (r.get("scenario_id") or "D").strip().upper() == sid
    ]
    if not filtered:
        print(f"No rows for scenario_id={sid}")
        return 1
    print(f"Analyzing scenario {sid} ({len(filtered)} rows)")
    probe = load_probe_json(probe_json)
    if probe:
        print(
            f"Probe suggestion: delay={probe.get('recommended_delay')} "
            f"({probe.get('recommended_preset')}), "
            f"est. RTT ~{probe.get('estimated_rtt_ms', '?')} ms"
        )
    by = {}
    for r in filtered:
        try:
            d = int(float((r.get("input_delay") or "").strip()))
        except ValueError:
            continue
        by.setdefault(d, []).append(r)
    if not by:
        print("No valid input_delay values in CSV")
        return 1
    best, bscore = None, -1
    for d, g in sorted(by.items()):
        lfb = [
            int(float(r["lfb_max"]))
            for r in g
            if (r.get("lfb_max") or "").strip()
        ]
        rfb = [
            int(float(r["rfb_max"]))
            for r in g
            if (r.get("rfb_max") or "").strip()
        ]
        leg = sum(
            1
            for r in g
            if "legacy" in (r.get("transport_active") or "").lower()
        )
        desync = sum(1 for r in g if (r.get("desync") or "").lower() == "yes")
        sc = (
            100
            - (max(lfb) if lfb else 0) * 8
            - (max(rfb) if rfb else 0) * 8
            - leg * 15
            - desync * 25
        )
        print(
            f"delay={d} n={len(g)} score={sc} "
            f"LFB_max={max(lfb) if lfb else 0} "
            f"RFB_max={max(rfb) if rfb else 0} legacy={leg} desync={desync}"
        )
        if sc > bscore:
            bscore, best = sc, d
    if best is not None:
        print(f"\nRecommended host input delay: {best}")
        if probe and probe.get("recommended_delay") == best:
            print("(matches probe suggestion)")
        elif probe:
            print(
                f"(probe suggested {probe.get('recommended_delay')}; "
                "match logs override for ground truth)"
            )
    return 0 if best is not None else 1


def main():
    p = argparse.ArgumentParser(description="SF4e rollback benchmark harness")
    s = p.add_subparsers(dest="c", required=True)
    pr = s.add_parser("probe", help="infra probe + delay presets")
    pr.add_argument("--json", help="write probe JSON to this path")
    pr.set_defaults(func=cmd_probe)
    pt = s.add_parser("template", help="create/append match log CSV")
    pt.add_argument(
        "-o",
        type=Path,
        default=RESULTS_DIR / "matches.csv",
        help="CSV path",
    )
    pt.set_defaults(func=lambda a: cmd_template(a.o))
    pa = s.add_parser("analyze", help="pick best input delay from match logs")
    pa.add_argument("csv_file", type=Path)
    pa.add_argument(
        "--scenario",
        default="D",
        help="filter rows by scenario_id (default: D)",
    )
    pa.add_argument(
        "--probe-json",
        type=Path,
        default=None,
        help="compare result to probe JSON snapshot",
    )
    pa.set_defaults(
        func=lambda a: cmd_analyze(a.csv_file, a.scenario, a.probe_json)
    )
    a = p.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
