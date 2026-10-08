"""Exercise setup's post-reload check without installing or contacting a server.

Run with: python3 tests/deploy_setup.py (requires Bash).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PublicRouteCheck(unittest.TestCase):
    def run_check(self, responses):
        setup = (Path(__file__).parents[1] / "deploy/setup.sh").read_text()
        # Exercise the real final setup block, with a restore trap and fake
        # commands. Fake sleep advances Bash's clock, keeping timeouts fast.
        block = setup.split("\nsystemctl reload nginx\n", 1)[1]
        prefix = r'''
set -euo pipefail
trap 'status=$?; if (( status != 0 )); then echo restored >&2; fi; exit "$status"' EXIT
unset SECONDS
SECONDS=0
calls=0
systemctl() { echo "$*" >&2; }
sleep() { SECONDS=$((SECONDS + 1)); }
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
            result = subprocess.run(
                [shutil.which("bash"), "-c", prefix + block],
                env={**os.environ, "ARGS_LOG": args_log.as_posix(),
                     "CALLS_FILE": calls_file.as_posix(), "RESPONSES": " ".join(responses)},
                capture_output=True, text=True, timeout=5,
            )
            args = args_log.read_text() if args_log.exists() else ""
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


if __name__ == "__main__":
    unittest.main()
