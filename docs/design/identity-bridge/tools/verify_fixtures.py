#!/usr/bin/env python3
"""Offline conformance checks for documentation fixtures, NOT a server or SDK.

The canonicalizer below accepts only ASCII strings/keys and safe integers. It is
sufficient for these fixtures and deliberately refuses the full JSON/JCS space.
Use a conforming RFC 8785 library in production. All fixture secrets are public.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import hmac
import json
import re
import sys
from pathlib import Path
from typing import Any, Callable

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey,
    )
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    from jsonschema import Draft202012Validator, FormatChecker, ValidationError
except ImportError as exc:
    raise SystemExit(
        'Missing verifier dependency. Install requirements-verifier.txt in a '
        'virtual environment, then rerun this script.'
    ) from exc

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / 'examples'
PASSED: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, f'Duplicate JSON property: {key}')
        result[key] = value
    return result


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=no_duplicate_keys)


def fixture_jcs(value: Any) -> bytes:
    """Restricted fixture subset; intentionally not a production canonicalizer."""
    def check(item: Any, depth: int = 0) -> None:
        require(depth <= 16, 'Too deeply nested')
        if item is None or type(item) is bool:
            return
        if type(item) is int:
            require(abs(item) <= 9007199254740991, 'Unsafe JSON integer')
            return
        if isinstance(item, str):
            require(item.isascii(), 'Fixture canonicalizer only accepts ASCII')
            return
        if isinstance(item, list):
            for child in item:
                check(child, depth + 1)
            return
        if isinstance(item, dict):
            for key, child in item.items():
                require(isinstance(key, str) and key.isascii(), 'Non-ASCII property')
                check(child, depth + 1)
            return
        raise ValueError('Unsupported fixture JSON type')
    check(value)
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False, allow_nan=False).encode('utf-8')


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def decode_b64u(text: str, length: int) -> bytes:
    require(bool(re.fullmatch(r'[A-Za-z0-9_-]+', text)), 'Invalid Base64url alphabet')
    raw = base64.b64decode(text + '=' * ((-len(text)) % 4), altchars=b'-_', validate=True)
    require(len(raw) == length, 'Wrong decoded length')
    require(b64u(raw) == text, 'Noncanonical Base64url')
    return raw


def ember_id(public_key: bytes) -> str:
    require(len(public_key) == 32, 'Wrong public key length')
    digest = hashlib.sha256(b'ember-id-v1\x00' + public_key).digest()
    return 'emb1_' + base64.b32encode(digest).rstrip(b'=').decode('ascii').lower()


SCHEMA = load(ROOT / 'schemas' / 'core.schema.json')


def validate(name: str, value: Any) -> None:
    schema = {'$schema': SCHEMA['$schema'], '$defs': SCHEMA['$defs'],
              '$ref': '#/$defs/' + name}
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(value)


def verify_challenge(challenge: dict[str, Any], proof: dict[str, Any],
                     public_key: bytes, command: dict[str, Any], now: int) -> None:
    validate('Challenge', challenge)
    validate('Proof', proof)
    require(challenge['ember_id'] == ember_id(public_key), 'Identity mismatch')
    require(challenge['challenge_id'] == proof['challenge_id'], 'Challenge mismatch')
    require(challenge['audience'] == 'https://bridge.ember.example', 'Wrong fixture audience')
    require(challenge['action'] == 'session.create', 'Wrong fixture action')
    require(challenge['method'] == 'POST' and challenge['path'] == '/v1/sessions', 'Wrong target')
    require(0 < challenge['expires_at'] - challenge['issued_at'] <= 60, 'Invalid lifetime')
    require(challenge['issued_at'] <= now < challenge['expires_at'], 'Expired or future challenge')
    require(challenge['request_digest'] == b64u(hashlib.sha256(fixture_jcs(command)).digest()),
            'Command digest mismatch')
    decode_b64u(challenge['nonce'], 32)
    Ed25519PublicKey.from_public_bytes(public_key).verify(
        decode_b64u(proof['signature'], 64),
        b'EMBER:CHALLENGE:1\n' + fixture_jcs(challenge),
    )


def verify_report(envelope: dict[str, Any]) -> dict[str, Any]:
    validate('SignedReport', envelope)
    report = envelope['report']
    public_key = decode_b64u(envelope['public_key'], 32)
    require(ember_id(public_key) == report['reporter_id'], 'Report identity mismatch')
    require(report['p1_id'] != report['p2_id'], 'Duplicate fighter identity')
    require({report['reporter_id'], report['opponent_id']} ==
            {report['p1_id'], report['p2_id']}, 'Reporter/roster mismatch')
    for field in ['assignment_generation', 'match_generation',
                  'capture_frame', 'confirmed_input_frame']:
        if report[field] is not None:
            require(int(report[field]) <= 2**64 - 1, 'Counter overflow')
    if report['result'] in ('p1_win', 'p2_win', 'draw'):
        require(int(report['confirmed_input_frame']) >= int(report['capture_frame']) - 1,
                'Result inputs not confirmed')
    Ed25519PublicKey.from_public_bytes(public_key).verify(
        decode_b64u(envelope['signature'], 64),
        b'EMBER:GAME-REPORT:1\n' + fixture_jcs(report),
    )
    return report


def verify_fixture_webhook(body: bytes, headers: dict[str, str],
                           secret: bytes, now: int) -> None:
    require(len(body) <= 65536, 'Webhook too large')
    ts = headers['webhook-timestamp']
    require(bool(re.fullmatch(r'[0-9]{1,16}', ts)), 'Bad timestamp')
    require(abs(now - int(ts)) <= 300, 'Webhook outside timestamp tolerance')
    require(len(secret) == 32, 'Wrong fixture secret length')
    signed = headers['webhook-id'].encode('ascii') + b'.' + ts.encode('ascii') + b'.' + body
    expected = hmac.new(secret, signed, hashlib.sha256).digest()
    candidates = headers['webhook-signature'].split()
    require(len(candidates) <= 8, 'Too many signatures')
    valid = False
    for candidate in candidates:
        version, separator, signature = candidate.partition(',')
        if version == 'v1' and separator:
            raw = base64.b64decode(signature, validate=True)
            valid = valid or hmac.compare_digest(expected, raw)
    require(valid, 'Invalid webhook signature')


def check(name: str, function: Callable[[], Any]) -> None:
    function()
    PASSED.append(name)
    print('PASS:', name)


def rejects(function: Callable[[], Any]) -> None:
    try:
        function()
    except (ValueError, ValidationError, InvalidSignature):
        return
    raise ValueError('Expected invalid fixture to be rejected')


def main() -> int:
    Draft202012Validator.check_schema(SCHEMA)
    check('JSON Schema 2020-12 structure', lambda: Draft202012Validator.check_schema(SCHEMA))
    files = {
        'identity.json':'Identity', 'auth-challenge.json':'Challenge',
        'auth-proof.json':'Proof', 'create-match.json':'CreateMatch',
        'game-report-p1.json':'SignedReport', 'game-report-p2.json':'SignedReport',
        'match-completed-event.json':'Event',
    }
    for filename, name in files.items():
        check(f'schema: {filename}', lambda f=filename, n=name: validate(n, load(EXAMPLES/f)))
    vectors = load(EXAMPLES/'test-vectors.json')
    for index, identity in enumerate(vectors['identities']):
        def verify_identity(v: dict[str, str] = identity) -> None:
            seed = bytes.fromhex(v['seed_hex'])
            public_key = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
                Encoding.Raw, PublicFormat.Raw)
            require(public_key.hex() == v['public_key_hex'], 'Public key vector mismatch')
            require(b64u(public_key) == v['public_key_base64url'], 'Public key encoding mismatch')
            require(ember_id(public_key) == v['ember_id'], 'Ember ID vector mismatch')
            require(len(v['ember_id']) == 57, 'ID length mismatch')
        check(f'identity derivation vector {index+1}', verify_identity)

    challenge=load(EXAMPLES/'auth-challenge.json')
    proof=load(EXAMPLES/'auth-proof.json')
    command=vectors['challenge']['command']
    key=bytes.fromhex(vectors['identities'][0]['public_key_hex'])
    now=challenge['issued_at']+1
    check('challenge canonical bytes', lambda: require(
        fixture_jcs(challenge).decode()==vectors['challenge']['canonical_utf8'], 'Canonical mismatch'))
    check('challenge signing bytes', lambda: require(
        (b'EMBER:CHALLENGE:1\n'+fixture_jcs(challenge)).hex()==vectors['challenge']['signing_bytes_hex'],
        'Signing bytes mismatch'))
    check('challenge signature and request binding', lambda: verify_challenge(challenge,proof,key,command,now))
    changed=copy.deepcopy(challenge);changed['nonce']=b64u(bytes(32))
    check('reject tampered challenge nonce', lambda: rejects(lambda: verify_challenge(changed,proof,key,command,now)))
    altered=copy.deepcopy(challenge);altered['audience']='https://attacker.example'
    check('reject wrong audience', lambda: rejects(lambda: verify_challenge(altered,proof,key,command,now)))
    check('reject wrong command digest', lambda: rejects(lambda: verify_challenge(challenge,proof,key,{'requested_scopes':['admin']},now)))
    check('reject expired challenge', lambda: rejects(lambda: verify_challenge(challenge,proof,key,command,challenge['expires_at'])))

    a=load(EXAMPLES/'game-report-p1.json'); b=load(EXAMPLES/'game-report-p2.json')
    check('player-one report signature and consistency', lambda: verify_report(a))
    check('player-two report signature and consistency', lambda: verify_report(b))
    normalized=['bridge_id','match_id','assignment_generation','attempt_id','permit_id','room_id',
                'table_id','match_generation','p1_id','p2_id','rules_digest','roster_digest','build_id','result']
    def agreement() -> None:
        ra,rb=verify_report(a),verify_report(b)
        require(ra['reporter_id'] != rb['reporter_id'], 'Not distinct signers')
        require(all(ra[k]==rb[k] for k in normalized), 'Reports do not agree')
        require(ra['capture_frame'] != rb['capture_frame'], 'Expected different frame evidence')
    check('distinct players agree despite different confirmation frames', agreement)
    bad=copy.deepcopy(a);bad['report']['result']='p2_win'
    check('reject altered winner',lambda:rejects(lambda:verify_report(bad)))
    bad_key=copy.deepcopy(a);bad_key['public_key']=b['public_key']
    check('reject substituted signing identity',lambda:rejects(lambda:verify_report(bad_key)))
    bad_frame=copy.deepcopy(a);bad_frame['report']['confirmed_input_frame']='1'
    check('reject unconfirmed native result claim',lambda:rejects(lambda:verify_report(bad_frame)))
    bad_counter=copy.deepcopy(a);bad_counter['report']['match_generation']='03'
    check('reject noncanonical counter',lambda:rejects(lambda:verify_report(bad_counter)))
    over=copy.deepcopy(a);over['report']['match_generation']=str(2**64)
    check('reject counter overflow beyond schema pattern',lambda:rejects(lambda:verify_report(over)))
    sig=decode_b64u(a['signature'],64)
    check('reject cross-domain report signature',lambda:rejects(lambda:Ed25519PublicKey.from_public_bytes(key).verify(
        sig,b'EMBER:CHALLENGE:1\n'+fixture_jcs(a['report']))))
    check('reject duplicate JSON properties',lambda:rejects(lambda:json.loads('{"a":1,"a":2}',object_pairs_hook=no_duplicate_keys)))
    check('restricted fixture canonicalizer rejects floats',lambda:rejects(lambda:fixture_jcs({'n':1.5})))

    event=load(EXAMPLES/'match-completed-event.json')
    def result_consistency() -> None:
        d=event['data']; scores={row['ember_id']:row['wins'] for row in d['scores']}
        require(len(scores)==2,'Duplicate score identities')
        require(scores[d['winner_id']]==d['games_to_win'],'Winning threshold not reached')
        require(all(v<d['games_to_win'] for k,v in scores.items() if k!=d['winner_id']), 'Two winners')
        require(sum(scores.values())==len(d['accepted_attempt_ids']), 'Example game accounting mismatch')
    check('FT2 completed-event score consistency', result_consistency)
    webhook=load(EXAMPLES/'webhook-fixture.json')
    body=(EXAMPLES/webhook['body_file']).read_bytes()
    secret=base64.b64decode(webhook['secret_base64'],validate=True)
    headers=webhook['headers']; clock=webhook['verification_time_unix']
    check('Standard Webhooks raw-body HMAC fixture',lambda:verify_fixture_webhook(body,headers,secret,clock))
    check('reject modified raw webhook body',lambda:rejects(lambda:verify_fixture_webhook(body+b' ',headers,secret,clock)))
    check('reject stale webhook delivery',lambda:rejects(lambda:verify_fixture_webhook(body,headers,secret,clock+301)))
    check('reject wrong webhook secret',lambda:rejects(lambda:verify_fixture_webhook(body,headers,bytes(32),clock)))
    result={'status':'passed','checks_passed':len(PASSED),'checks':PASSED,
            'scope':'Documentation schema and cryptographic fixture checks only.',
            'not_tested':['Ember build/runtime','Windows DPAPI','Wine/Proton','Iroh networking',
                          'actual bridge service','database transactions','BluMint API',
                          'full RFC 8785 Unicode/floating-point conformance','complete acceptance matrix']}
    (ROOT/'VALIDATION_REPORT.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(f'\n{len(PASSED)} fixture checks passed. No product integration tests were run.')
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (ValueError, ValidationError, InvalidSignature, KeyError, OSError) as exc:
        print(f'FAIL: {type(exc).__name__}: {exc}',file=sys.stderr)
        raise SystemExit(1) from exc
