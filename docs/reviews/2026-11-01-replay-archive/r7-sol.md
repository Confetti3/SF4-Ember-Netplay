**R7-01 — P2: Matching transport failures incorrectly pass the listing check.**

Location: [server/ember/archive-site/deploy/setup.sh:422](C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:422), with curl failures suppressed at lines 413–414.

**Scenario:** The bridge’s listing handler stalls before sending headers. The direct probe times out and curl prints `000`. The nginx probe can also time out before nginx’s 10-second upstream timeout produces a response, yielding another `000`. Because both commands use `|| :`, their failure statuses disappear. The equality branch accepts `000 == 000`, reports that the bridge needs its archive block, and reaches `armed=`, disarming rollback even though neither probe received an HTTP response.

This is distinct from the supported bootstrap state: two actual 404 responses should pass; two failed requests should fail.

**Fix:** Require both curl commands to complete successfully before comparing their HTTP statuses. Remove the unconditional failure suppression or handle each failed assignment explicitly, exiting while rollback remains armed. Keep the intended status comparison and the 200 body check after that transport boundary.

Validation: `bash -n` passed. An in-memory Bash harness using the exact listing-check block correctly handled healthy, bootstrap, wrong-route, missing-games and status-mismatch cases, but reproduced successful completion with rollback disarmed when both curls returned exit 28 and `000`. A separate loopback curl probe confirmed that timeout output. No files were edited and no build was run.

The canonical `proxy_pass .../v1/archive` change resolves the original routing issue without adding structural complexity. I found no additional findings in the requested fix.

VERDICT: CHANGES REQUIRED