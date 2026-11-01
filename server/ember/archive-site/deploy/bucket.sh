#!/usr/bin/env bash
# Creates the archive bucket on the local rustfs and applies the read-only
# public policy, then proves it: an anonymous request for a stored game works
# and an anonymous listing is refused. Run by setup.sh; safe to run again.
#   bash bucket.sh <rustfs.env> <endpoint> <bucket> <policy.json>
# The credentials are RUSTFS_ACCESS_KEY and RUSTFS_SECRET_KEY in the env file.
# curl reads them from a config on stdin, so they never appear on a command
# line.
set -euo pipefail

[ $# -eq 4 ] || { echo "usage: bucket.sh <rustfs.env> <endpoint> <bucket> <policy.json>" >&2; exit 2; }
ENVFILE=$1
ENDPOINT=${2%/}
BUCKET=$3
POLICY=$4
for file in "$ENVFILE" "$POLICY"; do
    [ -f "$file" ] || { echo "Missing $file" >&2; exit 1; }
done
ACCESS=$(sed -n 's/^RUSTFS_ACCESS_KEY=//p' "$ENVFILE")
SECRET=$(sed -n 's/^RUSTFS_SECRET_KEY=//p' "$ENVFILE")
[ -n "$ACCESS" ] && [ -n "$SECRET" ] || { echo "$ENVFILE needs RUSTFS_ACCESS_KEY and RUSTFS_SECRET_KEY." >&2; exit 1; }

# s3 <curl options>...: a signed request; prints the HTTP status, and the
# body goes to $BODY.
BODY=$(mktemp)
SENT=$(mktemp)
PROBE=games/0000000000000000000000000000000000000000000000000000000000000000.emberreplay
cleanup() {
    # The probe object is the only thing this script stores.
    s3 -X DELETE "$ENDPOINT/$BUCKET/$PROBE" >/dev/null 2>&1 || :
    rm -f "$BODY" "$SENT"
}
trap cleanup EXIT
s3() {
    curl -sS -o "$BODY" -w '%{http_code}' --max-time 20 -K - --aws-sigv4 "aws:amz:us-east-1:s3" "$@" \
        <<< "user = \"$ACCESS:$SECRET\""
}
expect() {
    local what=$1 got=$2; shift 2
    local want
    for want in "$@"; do [ "$got" = "$want" ] && return 0; done
    echo "$what: HTTP $got, expected $*" >&2
    head -c 300 "$BODY" >&2; echo >&2
    exit 1
}

# Creating a bucket that exists answers 200 or 409 (already owned by us).
# A starting rustfs answers 503 until its identity service is up.
for attempt in $(seq 1 20); do
    code=$(s3 -X PUT "$ENDPOINT/$BUCKET")
    [ "$code" != 503 ] && break
    sleep 1
done
if [ "$code" = 409 ] && grep -q BucketAlreadyOwnedByYou "$BODY"; then code=200; fi
expect "create bucket $BUCKET" "$code" 200

code=$(s3 -X PUT -H 'Content-Type: application/json' --data-binary "@$POLICY" "$ENDPOINT/$BUCKET?policy")
expect "apply policy" "$code" 200 204

# One object, public, then gone. The listing check does not need it.
printf 'archive-probe' > "$SENT"
HASH=$(sha256sum "$SENT" | cut -d' ' -f1)
code=$(s3 -X PUT -H "x-amz-content-sha256: $HASH" --data-binary "@$SENT" "$ENDPOINT/$BUCKET/$PROBE")
expect "store probe object" "$code" 200
code=$(curl -sS -o "$BODY" -w '%{http_code}' --max-time 20 "$ENDPOINT/$BUCKET/$PROBE")
expect "anonymous read of a game" "$code" 200
grep -q archive-probe "$BODY" || { echo "anonymous read returned another body" >&2; exit 1; }
for path in "" "/" "/?list-type=2" "/?policy" "/sets/anything" "/other/anything"; do
    code=$(curl -sS -o "$BODY" -w '%{http_code}' --max-time 20 "$ENDPOINT/$BUCKET$path")
    expect "anonymous GET $BUCKET$path" "$code" 403 404
done
code=$(curl -sS -o "$BODY" -w '%{http_code}' --max-time 20 -X PUT --data x "$ENDPOINT/$BUCKET/games/anonymous-write")
expect "anonymous write" "$code" 403
echo "bucket $BUCKET: policy applied; anonymous read of games/ and sets/ only, no listing, no writes"
