"""Linux half of test-full-quality.ps1; runs local checks, never deployment.

Rust workspaces come from the shared inventory; the receipt also lists the
platform-specific checks below."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("toolchain", help="Rust toolchain selector, such as +1.98.0, from test-full-quality.ps1")
args = parser.parse_args()
source = args.source.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ)
environment["PATH"] = str(Path.home() / ".cargo/bin") + os.pathsep + environment["PATH"]
environment["CARGO_BUILD_JOBS"] = "4"
environment["JOBS"] = "4"
checks = []


def add(name, command, directory=source, target=None):
    checks.append(dict(name=name, platform="linux", status="unexecuted", command=command,
                       directory=str(directory), target=target, log=str(output / (name + ".log"))))


for component in json.loads((source / "scripts/rust-workspaces.json").read_text(encoding="utf-8")):
    name, relative = component["name"], component["path"]
    target = str(Path.home() / ".cache/sf4-full-quality" / name)
    directory = source / relative
    add(name + "-tests", ["cargo", args.toolchain, "test", "--locked"], directory, target)
    add(name + "-clippy", ["cargo", args.toolchain, "clippy", "--locked", "--all-targets", "--no-deps", "--", "-D", "warnings"], directory, target)
    add(name + "-format", ["cargo", args.toolchain, "fmt", "--all", "--check"], directory, target)
add("rooms-signals", ["cargo", args.toolchain, "test", "--locked", "--test", "sigterm"],
    source / "server/ember-rooms", str(Path.home() / ".cache/sf4-full-quality/rooms"))
add("posix-helper", ["bash", str(source / "server/roomhost/test-linux.sh"), str(source)])
add("room-host-build", ["bash", str(source / "server/roomhost/build-linux.sh"), str(source), str(output / "sf4e-room-host")])
add("rooms-deployment-mocks", ["bash", "test-setup.sh"], source / "server/ember-rooms/deploy")
add("reports-deployment-mocks", ["python3", "tests/deploy_setup.py"], source / "server/ember-reports")
files = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=source, text=True).splitlines()
for name in sorted(set(files)):
    if name.endswith(".sh"):
        add("shell-syntax-" + name.replace("/", "-"), ["bash", "-n", str(source / name)])


def save():
    (output / "linux-quality.json").write_text(json.dumps(checks, indent=2) + "\n")


save()
for check in checks:
    print("RUN", check["name"], flush=True)
    started = time.monotonic()
    env = dict(environment)
    if check["target"]:
        env["CARGO_TARGET_DIR"] = check["target"]
    try:
        with open(check["log"], "w") as log:
            result = subprocess.run(check["command"], cwd=check["directory"], env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=3600)
        check["exitCode"] = result.returncode
        check["status"] = "passed" if result.returncode == 0 else "failed"
    except (OSError, subprocess.TimeoutExpired) as error:
        check["status"] = "failed"
        check["error"] = str(error)
    check["seconds"] = time.monotonic() - started
    save()
    print(check["status"].upper(), check["name"], flush=True)
raise SystemExit(any(check["status"] != "passed" for check in checks))
