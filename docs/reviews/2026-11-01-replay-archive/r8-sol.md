No P1/P2/P3 findings in the scoped R7-01 fix.

At [setup.sh:416](/C:/Users/Kate/Desktop/sf4/sf4-nightly/server/ember/archive-site/deploy/setup.sh:416), the guard rejects `000` from either probe before status comparison. It removes the temporary listing file and exits with status 1 while rollback remains armed. This directly closes the `000 == 000` success path without adding unnecessary abstraction or changing the intended HTTP-response handling.

Validation: `bash -n` passed. An in-memory Bash harness using the exact listing block confirmed:

- `000/000`, `000/404`, and `404/000` fail with rollback armed.
- Healthy `200/200` with a games body and bootstrap `404/404` succeed.
- Missing games and mismatched statuses fail with rollback armed.

No files were edited, no build was run, and deployment rollback itself was not exercised.

VERDICT: APPROVE