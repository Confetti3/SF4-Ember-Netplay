#!/usr/bin/env python3
"""Apply the complete shipping GGPO patch stack; optionally compile on Windows.

Requires git, and cmake for --build. Uses a temporary checkout and never edits
an existing working tree. Network access is required to retrieve pinned GGPO.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(*args: str, cwd: Path) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="compile patched GGPO (Windows SDK required)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    port = root / "vcpkg-overlays" / "ports" / "ggpo"
    text = (port / "portfile.cmake").read_text(encoding="utf-8")
    ref_match = re.search(r"\bREF\s+([0-9a-f]+)\b", text)
    stack_match = re.search(r"\bPATCHES\s+(.*?)\n\)", text, re.DOTALL)
    if ref_match is None or stack_match is None:
        raise RuntimeError("Cannot identify the pinned ref and ordered GGPO patches")
    patches = re.findall(r'"([^"/]+\.patch)"', stack_match.group(1))
    if not patches:
        raise RuntimeError("No GGPO port patches found")
    with tempfile.TemporaryDirectory(prefix="ember-ggpo-check-") as directory:
        work = Path(directory)
        source = work / "source"
        # No shallow clone: the pinned historical commit must be available.
        run("git", "clone", "--quiet", "--no-checkout", "https://github.com/adanducci/ggpo.git", str(source), cwd=work)
        run("git", "checkout", "--quiet", "--detach", ref_match.group(1), cwd=source)
        for name in patches:
            patch = str(port / name)
            run("git", "apply", "--check", "--ignore-whitespace", patch, cwd=source)
            run("git", "apply", "--ignore-whitespace", "--whitespace=nowarn", patch, cwd=source)
            print(f"PASS applied {name}", flush=True)
        shutil.copy2(port / "input-repair.h", source / "src/lib/ggpo/network/input-repair.h")
        run("git", "diff", "--check", cwd=source)
        if args.build:
            build = work / "build"
            run("cmake", "-S", str(source), "-B", str(build),
                "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DGGPO_BUILD_VECTORWAR=OFF",
                "-DGGPO_BUILD_SDK=ON", "-DBUILD_SHARED_LIBS=OFF", cwd=work)
            run("cmake", "--build", str(build), "--config", "Release", cwd=work)
            print("PASS compiled patched GGPO", flush=True)


if __name__ == "__main__":
    main()
