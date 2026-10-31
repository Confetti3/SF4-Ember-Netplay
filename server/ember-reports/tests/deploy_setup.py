"""Exercise setup's post-reload check and install-symbols.sh's store handling
without installing or contacting a server.

Run with: python3 tests/deploy_setup.py (requires Bash).
"""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


class PublicRouteCheck(unittest.TestCase):
    def run_check(self, responses, clock_reads=None):
        setup = (Path(__file__).parents[1] / "deploy/setup.sh").read_text()
        # Exercise the real final setup block, with a restore trap and fake
        # commands. Fake sleep advances Bash's clock, keeping timeouts fast.
        block = setup.split("\nsystemctl reload nginx\n", 1)[1]
        if clock_reads is not None:
            # Script each clock read to reproduce expiration between the old
            # loop's condition and its separate remaining-budget calculation.
            block = block.replace("SECONDS", "$(clock)")
        prefix = r'''
set -euo pipefail
trap 'status=$?; if (( status != 0 )); then echo restored >&2; fi; exit "$status"' EXIT
unset SECONDS
SECONDS=0
calls=0
systemctl() { echo "$*" >&2; }
sleep() { SECONDS=$((SECONDS + 1)); }
clock() {
    local reads=$(cat "$CLOCK_FILE")
    echo $((reads + 1)) > "$CLOCK_FILE"
    local values=($CLOCK_READS)
    printf '%s' "${values[reads]:-${values[-1]}}"
}
curl() {
    printf '%s\n' "$@" >> "$ARGS_LOG"
    calls=$(cat "$CALLS_FILE")
    echo $((calls + 1)) > "$CALLS_FILE"
    local codes=($RESPONSES)
    local code=${codes[calls]:-${codes[-1]}}
    if [[ "$code" == error ]]; then printf 000; return 7; fi
    printf '%s' "$code"
}
ETC=/etc/ember-reports
'''
        with tempfile.TemporaryDirectory() as directory:
            args_log = Path(directory) / "args"
            calls_file = Path(directory) / "calls"
            calls_file.write_text("0")
            clock_file = Path(directory) / "clock"
            clock_file.write_text("0")
            result = subprocess.run(
                [shutil.which("bash"), "-c", prefix + block],
                env={**os.environ, "ARGS_LOG": args_log.as_posix(),
                     "CALLS_FILE": calls_file.as_posix(), "RESPONSES": " ".join(responses),
                     "CLOCK_FILE": clock_file.as_posix(),
                     "CLOCK_READS": " ".join(map(str, clock_reads or []))},
                capture_output=True, text=True, timeout=5,
            )
            args = args_log.read_text() if args_log.exists() else ""
            arguments = args.splitlines()
            budgets = [int(arguments[i + 1]) for i, arg in enumerate(arguments)
                       if arg == "--max-time"]
            self.assertEqual(len(budgets), int(calls_file.read_text()))
            self.assertTrue(all(budget > 0 for budget in budgets), budgets)
            return result, int(calls_file.read_text()), args

    def test_old_worker_and_transport_error_are_retried(self):
        result, calls, args = self.run_check(["404", "error", "405"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, 3)
        self.assertIn("--resolve\nembernetplay.link:443:127.0.0.1\n", args)
        self.assertIn("--noproxy\n*\n", args)
        self.assertIn("https://embernetplay.link/report/v1\n", args)
        self.assertNotIn("--insecure", args)
        self.assertNotIn("-X\nPOST", args)
        self.assertNotIn("restored", result.stderr)

    def test_persistent_wrong_route_fails_and_triggers_restore(self):
        for code in ["404", "200", "301", "error"]:
            with self.subTest(code=code):
                result, calls, _ = self.run_check([code])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, 10)
                self.assertIn("restored", result.stderr)
                self.assertNotIn("installed.", result.stdout)

    def test_deadline_boundary_keeps_curl_budget_positive_and_triggers_restore(self):
        # Read 0 establishes deadline 10; read 9 leaves one second. The old
        # loop then read 10 again and passed --max-time 0 to curl.
        result, calls, args = self.run_check(["404"], clock_reads=[0, 9, 10])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(calls, 1)
        self.assertIn("--max-time\n1\n", args)
        self.assertIn("Report HTTPS route did not answer 405 within 10 seconds.", result.stderr)
        self.assertIn("restored", result.stderr)
        self.assertNotIn("installed.", result.stdout)

    def test_expired_deadline_skips_curl_and_triggers_restore(self):
        result, calls, args = self.run_check(["405"], clock_reads=[0, 10])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(calls, 0)
        self.assertEqual(args, "")
        self.assertIn("Report HTTPS route did not answer 405 within 10 seconds.", result.stderr)
        self.assertIn("restored", result.stderr)
        self.assertNotIn("installed.", result.stdout)


class SymbolInstall(unittest.TestCase):
    """install-symbols.sh on a scratch store and manifest folder."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.store = root / "symbols"
        self.manifests = root / "manifests"
        self.store.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def run_phase(self, channel, label, phase):
        script = Path(__file__).parents[1] / "deploy/install-symbols.sh"
        return subprocess.run(
            [shutil.which("bash"), "-s", "--", channel, label, phase],
            input=script.read_text(), capture_output=True, text=True, timeout=10,
            env={**os.environ, "EMBER_SYMBOL_STORE": self.store.as_posix(),
                 "EMBER_SYMBOL_MANIFESTS": self.manifests.as_posix()},
        )

    def upload(self, channel, label, modules):
        prepared = self.run_phase(channel, label, "prepare")
        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        incoming = Path(prepared.stdout.strip())
        self.assertEqual(incoming.parent, self.store)
        for module in modules:
            folder = incoming / module
            folder.mkdir(parents=True)
            name = module.split("/")[0].removesuffix(".pdb")
            (folder / f"{name}.sym").write_text(f"MODULE windows x86 {module.split('/')[1]} {name}.pdb\n")
        return incoming

    def install(self, channel, label, modules):
        incoming = self.upload(channel, label, modules)
        committed = self.run_phase(channel, label, "commit")
        cleaned = self.run_phase(channel, label, "cleanup")
        self.assertEqual(cleaned.returncode, 0, cleaned.stderr)
        self.assertFalse(incoming.exists())
        return committed

    def age_manifests(self):
        stale = time.time() - 50 * 24 * 3600
        for manifest in self.manifests.glob("*.txt"):
            os.utime(manifest, (stale, stale))

    def test_upload_moves_into_store_and_records_the_release(self):
        result = self.install("stable", "1.4.1", ["Sidecar.pdb/AA1", "GGPO.pdb/BB1"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.store / "Sidecar.pdb/AA1/Sidecar.sym").is_file())
        self.assertEqual((self.manifests / "stable-1.4.1.txt").read_text().split(),
                         ["GGPO.pdb/BB1", "Sidecar.pdb/AA1"])
        # The same symbols again: kept as they are, listed again.
        again = self.install("nightly", "1.5.0-nightly20261030", ["GGPO.pdb/BB1"])
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual((self.manifests / "nightly-1.5.0-nightly20261030.txt").read_text().split(),
                         ["GGPO.pdb/BB1"])
        self.assertEqual(sorted(p.name for p in self.store.iterdir()), ["GGPO.pdb", "Sidecar.pdb"])

    def test_old_nightly_symbols_go_unless_another_release_lists_them(self):
        self.assertEqual(self.install("stable", "1.4.1", ["GGPO.pdb/BB1"]).returncode, 0)
        old = self.install("nightly", "1.5.0-nightly20260901", ["Sidecar.pdb/CC1", "GGPO.pdb/BB1"])
        self.assertEqual(old.returncode, 0, old.stderr)
        self.age_manifests()
        result = self.install("nightly", "1.5.0-nightly20261030", ["Sidecar.pdb/DD1", "GGPO.pdb/BB1"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.store / "Sidecar.pdb/CC1").exists())
        self.assertTrue((self.store / "Sidecar.pdb/DD1/Sidecar.sym").is_file())
        # Listed by a Stable release, however old, and by the new Nightly.
        self.assertTrue((self.store / "GGPO.pdb/BB1/GGPO.sym").is_file())
        self.assertEqual(sorted(p.name for p in self.manifests.glob("*.txt")),
                         ["nightly-1.5.0-nightly20261030.txt", "stable-1.4.1.txt"])

    def test_bad_input_installs_nothing(self):
        for channel, label in [("beta", "1.4.1"), ("stable", "1.4.1;rm"), ("stable", "../1.4.1")]:
            result = self.run_phase(channel, label, "prepare")
            self.assertEqual(result.returncode, 2, result.stderr)
        empty = self.install("stable", "1.4.1", [])
        self.assertNotEqual(empty.returncode, 0)
        odd = self.install("stable", "1.4.2", ["Sidecar.pdb/not-hex"])
        self.assertNotEqual(odd.returncode, 0)
        # Every entry is checked before any moves: a valid first entry stays
        # out of the store when the second is invalid.
        second = self.install("stable", "1.4.3", ["GGPO.pdb/BB1", "Sidecar.pdb/not-hex"])
        self.assertEqual(second.returncode, 4, second.stderr)
        self.assertEqual(list(self.store.iterdir()), [])
        self.assertFalse(self.manifests.exists())

    def test_prune_never_follows_a_manifest_entry_outside_the_store(self):
        # A manifest edited by hand: an entry naming an upload folder or the
        # store's parent is never deleted.
        outside = self.store.parent / "outside/AA1"
        hidden = self.store / ".incoming-stable-1.4.1/AA1"
        for folder in (outside, hidden):
            folder.mkdir(parents=True)
        self.manifests.mkdir()
        (self.manifests / "nightly-1.5.0-nightly20260901.txt").write_text(
            ".incoming-stable-1.4.1/AA1\n../outside/AA1\n")
        self.age_manifests()
        result = self.install("nightly", "1.5.0-nightly20261030", ["Sidecar.pdb/DD1"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(outside.is_dir() and hidden.is_dir())
        self.assertFalse((self.manifests / "nightly-1.5.0-nightly20260901.txt").exists())

    def commit_in_background(self, channel, label):
        script = Path(__file__).parents[1] / "deploy/install-symbols.sh"
        process = subprocess.Popen(
            [shutil.which("bash"), "-s", "--", channel, label, "commit"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, "EMBER_SYMBOL_STORE": self.store.as_posix(),
                 "EMBER_SYMBOL_MANIFESTS": self.manifests.as_posix()},
        )
        process.stdin.write(script.read_text())
        process.stdin.close()
        return process

    def test_a_shared_dependency_survives_a_stable_commit_and_a_nightly_prune(self):
        # GGPO.pdb/BB1 is listed by an expired Nightly manifest, and a Stable
        # release lists it too while a Nightly commit prunes. Commit holds the
        # store's lock from its checks for folders already there to its prune,
        # so the two run one after the other, and either order keeps BB1.
        for stable_first in (True, False):
            with self.subTest(stable_first=stable_first):
                shutil.rmtree(self.store)
                shutil.rmtree(self.manifests, ignore_errors=True)
                self.store.mkdir()
                old = self.install("nightly", "1.5.0-nightly20260901", ["GGPO.pdb/BB1", "Sidecar.pdb/CC1"])
                self.assertEqual(old.returncode, 0, old.stderr)
                self.age_manifests()
                self.upload("stable", "1.4.1", ["GGPO.pdb/BB1", "Sidecar.pdb/AA1"])
                self.upload("nightly", "1.5.0-nightly20261030", ["Sidecar.pdb/DD1"])
                order = [("stable", "1.4.1"), ("nightly", "1.5.0-nightly20261030")]
                if not stable_first:
                    order.reverse()
                lock = os.open(self.store, os.O_RDONLY)
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX)
                    waiting = self.commit_in_background(*order[0])
                    time.sleep(0.5)
                    # Nothing moved, nothing recorded, nothing pruned while
                    # another holds the store.
                    self.assertIsNone(waiting.poll())
                    self.assertFalse((self.store / "Sidecar.pdb/AA1").exists())
                    self.assertFalse((self.store / "Sidecar.pdb/DD1").exists())
                    self.assertTrue((self.store / "Sidecar.pdb/CC1").exists())
                    self.assertEqual(sorted(p.name for p in self.manifests.glob("*.txt")),
                                     ["nightly-1.5.0-nightly20260901.txt"])
                finally:
                    fcntl.flock(lock, fcntl.LOCK_UN)
                    os.close(lock)
                _, error = waiting.communicate(timeout=10)
                self.assertEqual(waiting.returncode, 0, error)
                second = self.run_phase(*order[1], "commit")
                self.assertEqual(second.returncode, 0, second.stderr)
                for channel, label in order:
                    self.run_phase(channel, label, "cleanup")
                self.assertTrue((self.store / "GGPO.pdb/BB1/GGPO.sym").is_file())
                self.assertFalse((self.store / "Sidecar.pdb/CC1").exists())
                self.assertIn("GGPO.pdb/BB1", (self.manifests / "stable-1.4.1.txt").read_text().split())
                self.assertEqual(sorted(p.name for p in self.store.iterdir()), ["GGPO.pdb", "Sidecar.pdb"])


if __name__ == "__main__":
    unittest.main()
