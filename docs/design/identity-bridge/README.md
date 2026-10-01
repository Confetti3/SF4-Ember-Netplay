# Ember identity and tournament bridge specification package

Start with **EMBER_IDENTITY_AND_TOURNAMENT_BRIDGE_SPEC.md**, or open the HTML edition in a browser. This is a proposed implementation specification, not a shipped Ember feature or a working tournament service.

## Contents

- `EMBER_IDENTITY_AND_TOURNAMENT_BRIDGE_SPEC.md`: full design, requirements, protocol semantics, architecture, recovery, implementation work packages, acceptance tests, and sources.
- `EMBER_IDENTITY_AND_TOURNAMENT_BRIDGE_SPEC.html`: self-contained reading edition with section navigation; no external scripts, fonts, or stylesheets.
- `IMPLEMENTATION_CHECKLIST.md`: work packages, release gates, and unexecuted product acceptance tests extracted from the specification.
- `schemas/core.schema.json`: JSON Schema 2020-12 definitions for identity, challenge, proof, match creation, signed native report, and event core. This is not a complete OpenAPI document or generated server.
- `examples/`: concrete, deterministic protocol examples and public test vectors.
- `tools/verify_fixtures.py`: offline fixture/schema checks and selected negative tests.
- `requirements-verifier.txt`: versions used for the supplied verifier run.
- `VALIDATION_REPORT.json`: exact list and scope of executed fixture checks.
- `SHA256SUMS.txt`: file-integrity checksums for this package, excluding this checksum file itself.

## Run the fixture checks

From this directory, using a Python virtual environment:

```sh
python -m pip install -r requirements-verifier.txt
python tools/verify_fixtures.py
```

The verifier does not access the network or write to any user account. It writes `VALIDATION_REPORT.json` in this package directory. Its canonicalizer is intentionally limited to the ASCII/safe-integer fixture subset. Production implementations need a conforming RFC 8785 implementation, real authorization/storage logic, and the full acceptance suite.

**All fixture seeds and webhook secrets are public test data. Never use them as real Ember identities or production secrets.** The identity fixture is not an identity created for the user. The completed-match event assumes two earlier accepted games; the supplied signed report pair illustrates only the final game, not a complete gameplay transcript.

## Verification boundaries

Repository facts were checked at Ember commit `92aad00459e36b009951a6145b6dab19f6e7f223`. No repository changes were made. The current BluMint staging documentation could not be retrieved during preparation; the adapter contract is explicitly provisional and gated on provider-approved documentation and staging tests.

Passing the supplied fixture checks does **not** mean the implementation checklist passed. Windows DPAPI, Wine/Proton, actual Ember gameplay, a deployed bridge/database, and BluMint were not executed by these tests.
