#!/usr/bin/env bash
# SFV Ember's own bridge and database on the VPS (docs/design/SFV_BRIDGE.md):
# a third instance of the Ember bridge, ember-bridge-sfv, at
# https://sfv-bridge.embernetplay.link on 127.0.0.1:47822, beside SF4 Ember's
# production bridge (ember-bridge, 47820) and the staging bridge (47821). It
# shares the room pool with SF4's bridge (each bridge reconciles only the
# rooms it created) and never opens, writes or restarts anything of SF4's:
# it reads SF4's bridge.json (the rooms block), integrations.json (the
# Discord client secret) and a read-only snapshot of its database (SFV's
# ratings), and every phase checks at the end that SF4's bridge files and its
# running process are as they were.
#
# Run each phase with sudo, from your own account, after stage.sh has put this
# folder at ~/ember-sfv-bridge:
#
#   sudo bash ~/ember-sfv-bridge/upgrade.sh bridge   install ember-bridge-sfv (its own account, state,
#                                                    backups, units and nginx site), connect it to the
#                                                    room pool and Discord with SF4's settings
#   sudo bash ~/ember-sfv-bridge/upgrade.sh import   once, before players use it: copy SFV's ratings,
#                                                    sittings and ranking choices from a read-only
#                                                    snapshot of SF4's database into SFV's
#   sudo bash ~/ember-sfv-bridge/upgrade.sh site     embernetplay.link: /rooms-sfv.json, /rankings-sfv.json
#                                                    and SFV match links from SFV's bridge
#   sudo bash ~/ember-sfv-bridge/upgrade.sh check    read-only: both bridges, the listings, SF4 untouched
#
#   sudo bash ~/ember-sfv-bridge/upgrade.sh rollback-site     the site as it was before the site phase
#   sudo bash ~/ember-sfv-bridge/upgrade.sh rollback-bridge   SFV's bridge stopped, disabled and its site
#                                                    removed; its state and backups stay
#
# Before the bridge phase: a DNS A record sfv-bridge.embernetplay.link ->
# this VPS (the *.embernetplay.link certificate already covers the name).
# After it: the Discord application's OAuth2 redirects get
# https://sfv-bridge.embernetplay.link/v1/discord/callback.
# Order: bridge, import, site. SFV Ember builds that name the new origin go
# out after the site phase; until then SFV players keep using SF4's bridge.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo." >&2; exit 1; }
OPS=$(getent passwd "${SUDO_USER:?run with sudo from your own account}" | cut -d: -f6)
STAGE=$OPS/ember-sfv-bridge
OPERATOR=$OPS/ember-bridge-sfv
NAME=ember-bridge-sfv
LIB=/usr/local/lib/$NAME
STATE=/var/lib/$NAME
HOST=sfv-bridge.embernetplay.link
LOCAL=http://127.0.0.1:47822
SITE=/etc/nginx/sites-available/$HOST
SITE_ENABLED=/etc/nginx/sites-enabled/$HOST
NOTES=/var/lib/ember-sfv-bridge
SF4_LIB=/usr/local/lib/ember-bridge
SF4_STATE=/var/lib/ember-bridge
SF4_SITE=/etc/nginx/sites-available/bridge.embernetplay.link
SF4_LOCAL=http://127.0.0.1:47820
ROOMS_SECRET=/etc/ember-rooms/supervisor.secret
SHORT=$OPS/ember-short
SHORT_SNIPPET=/etc/nginx/snippets/ember-short.conf

sha() { sha256sum "$1" | cut -d' ' -f1; }
staged() { awk -v f="$1" '$2 == f || $2 == "*" f { print $1 }' "$STAGE/SHA256SUMS"; }
short() { echo "$1" | cut -c1-8; }
fail() { echo "$*" >&2; exit 1; }
install -d -o root -g root -m 0700 "$NOTES"

check_stage() {
    local f want
    [ -s "$STAGE/SHA256SUMS" ] || fail "$STAGE/SHA256SUMS is missing (stage.sh writes it)."
    while read -r want f; do
        f=${f#\*}
        [ -f "$STAGE/$f" ] && [ "$(sha "$STAGE/$f")" = "$want" ] ||
            fail "$STAGE/$f is missing or not the file SHA256SUMS names."
    done < "$STAGE/SHA256SUMS"
}

# SF4's production bridge as this script must leave it: its binary, its
# configuration, secrets and site, and the process that runs it (a restart
# would change the main PID). The database is left out: the running bridge
# writes it all the time.
sf4_fingerprint() {
    local f
    for f in "$SF4_LIB/ember-bridge" "$SF4_STATE/bridge.json" "$SF4_STATE/bridge-secrets.json" \
        "$SF4_STATE/integrations.json" "$SF4_SITE"; do
        if [ -e "$f" ]; then echo "$(sha "$f") $f"; else echo "absent $f"; fi
    done
    echo "pid $(systemctl show -p MainPID --value ember-bridge)"
}
SF4_BEFORE=
sf4_hold() {
    SF4_BEFORE=$(sf4_fingerprint)
    systemctl is-active --quiet ember-bridge || fail "SF4's bridge (ember-bridge) is not running; nothing done."
}
sf4_unchanged() {
    local now
    now=$(sf4_fingerprint)
    if [ "$now" != "$SF4_BEFORE" ]; then
        echo "SF4's bridge changed during this phase (it should not have):" >&2
        diff <(echo "$SF4_BEFORE") <(echo "$now") >&2 || :
        exit 1
    fi
    echo "SF4's bridge: binary $(short "$(sha "$SF4_LIB/ember-bridge")"), configuration, site and process unchanged."
}

sfv_id() {
    sudo -u "$NAME" python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["bridge_id"])' "$STATE/bridge.json"
}
wait_sfv() {
    local i
    for i in $(seq 1 15); do
        curl -fsS --max-time 3 "$LOCAL/.well-known/ember-bridge.json" >/dev/null 2>&1 && return 0
        sleep 1
    done
    fail "SFV's bridge does not answer on $LOCAL: journalctl -u $NAME"
}

# A secret handed to the instance's own set-integration-secret.sh on stdin,
# which skips its prompt when stdin is not a terminal, stores it as the
# service account and restarts SFV's bridge.
store_secret() {
    local kind=$1 value=$2
    { printf '%s' "$value"; echo; } | bash "$OPERATOR/set-integration-secret.sh" "$kind" >/dev/null
}

phase_bridge() {
    check_stage
    sf4_hold
    local new zone
    new=$(staged ember-bridge)
    getent hosts "$HOST" >/dev/null ||
        echo "note: $HOST does not resolve yet; add its A record. The install goes on (its checks use loopback)."

    echo "== bridge 1. the operator folder $OPERATOR"
    install -d -o "$SUDO_USER" -g "$SUDO_USER" -m 0755 "$OPERATOR" "$OPERATOR/bin"
    (cd "$STAGE/bridge-deploy" && find . -type f) | while read -r f; do
        install -D -o "$SUDO_USER" -g "$SUDO_USER" -m 0644 "$STAGE/bridge-deploy/$f" "$OPERATOR/$f"
    done
    chmod 0755 "$OPERATOR"/*.sh
    if [ -f "$OPERATOR/bin/ember-bridge" ] && [ "$(sha "$OPERATOR/bin/ember-bridge")" != "$new" ]; then
        cp -p "$OPERATOR/bin/ember-bridge" "$OPERATOR/bin/ember-bridge.prev"
    fi
    install -o "$SUDO_USER" -g "$SUDO_USER" -m 0755 "$STAGE/ember-bridge" "$OPERATOR/bin/ember-bridge"
    # The request zones are shared with SF4's bridge, so setup.sh refuses to
    # install a different file; the installed one is production's and stays.
    zone=/etc/nginx/conf.d/ember-bridge.conf
    if [ -e "$zone" ] && ! cmp -s "$zone" "$OPERATOR/nginx/ember-bridge-zone.conf"; then
        cp "$zone" "$OPERATOR/nginx/ember-bridge-zone.conf"
        chown "$SUDO_USER:$SUDO_USER" "$OPERATOR/nginx/ember-bridge-zone.conf"
        echo "the installed request zones differ from the staged ones: SFV's folder takes the installed file"
    fi

    echo "== bridge 2. ember-bridge-sfv $(short "$new") (setup.sh: account, state, backups, units, site)"
    bash "$OPERATOR/setup.sh"
    [ "$(sha "$LIB/ember-bridge")" = "$new" ] && systemctl is-active --quiet "$NAME" ||
        fail "SFV's bridge did not install or start: journalctl -u $NAME"
    sfv_id > "$NOTES/bridge-id"
    echo "SFV's bridge ID: $(cat "$NOTES/bridge-id")"

    echo "== bridge 3. public rooms: SF4's rooms settings (supervisor, badges, developers)"
    local rooms
    rooms=$(python3 -c 'import json, sys; r = json.load(open(sys.argv[1])).get("rooms"); print(json.dumps(r) if r else "")' "$SF4_STATE/bridge.json")
    if [ -z "$rooms" ]; then
        echo "SF4's bridge has no rooms settings; SFV's has no public rooms until it gets some."
    else
        printf '%s' "$rooms" | sudo -u "$NAME" python3 -c '
import json, os, sys, tempfile
path = sys.argv[1]
rooms = json.load(sys.stdin)
with open(path) as file:
    config = json.load(file)
if config.get("rooms") == rooms:
    sys.exit(0)
config["rooms"] = rooms
handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".bridge.json.")
with os.fdopen(handle, "w") as file:
    json.dump(config, file, indent=2)
    file.write("\n")
os.replace(temporary, path)' "$STATE/bridge.json"
        [ -s "$ROOMS_SECRET" ] || fail "Missing $ROOMS_SECRET (the room pool's caller secret)."
        store_secret rooms "$(tr -d '[:space:]' < "$ROOMS_SECRET")"
        echo "rooms: $(python3 -c 'import json, sys; print(json.loads(sys.argv[1])["supervisor_url"])' "$rooms")"
    fi

    echo "== bridge 4. Discord sign-in: SF4's Ember application and client secret"
    local discord
    discord=$(python3 -c '
import json, sys
try:
    print(json.load(open(sys.argv[1])).get("discord_client_secret", ""))
except FileNotFoundError:
    print("")' "$SF4_STATE/integrations.json")
    if [ -n "$discord" ]; then
        store_secret discord "$discord"
        echo "discord: client secret stored. Add https://$HOST/v1/discord/callback to the application's OAuth2 redirects."
    else
        echo "SF4's bridge has no Discord client secret; store one: sudo bash $OPERATOR/set-integration-secret.sh discord"
    fi
    unset discord

    echo "== bridge 5. answers"
    wait_sfv
    curl -fsS "$LOCAL/v1/rooms/public?game=sfv" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print("SFV listing:", len(d["rooms"]), "room(s), bridge", d["bridge_id"])' ||
        echo "SFV listing: no answer (public rooms are off until the rooms settings and secret are in)" >&2
    curl -fsS --resolve "$HOST:443:127.0.0.1" "https://$HOST/.well-known/ember-bridge.json" >/dev/null &&
        echo "nginx: https://$HOST answers" || fail "nginx does not serve https://$HOST"
    sf4_unchanged
    echo "Bridge done. Next: sudo bash $STAGE/upgrade.sh import"
}

phase_import() {
    sf4_hold
    [ ! -f "$NOTES/imported" ] || { echo "Imported already ($(cat "$NOTES/imported")). Nothing done."; return 0; }
    systemctl is-active --quiet "$NAME" || fail "SFV's bridge is not running: run the bridge phase first."
    local stamp
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    # Global, for the EXIT trap below.
    snap=$STATE/import-from-sf4-$stamp.sqlite3

    echo "== import 1. a read-only snapshot of SF4's database"
    python3 - "$SF4_STATE/bridge.sqlite3" "$snap.partial" <<'PY'
import sqlite3, sys
source = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
target = sqlite3.connect(sys.argv[2])
source.backup(target)
check = target.execute("PRAGMA integrity_check").fetchone()[0]
target.close()
source.close()
if check != "ok":
    sys.exit(f"the snapshot failed its integrity check: {check}")
PY
    chown "$NAME:$NAME" "$snap.partial"
    chmod 0600 "$snap.partial"
    mv "$snap.partial" "$snap"
    trap 'rm -f "$snap"' EXIT

    echo "== import 2. SFV's bridge backed up and stopped"
    systemctl start "$NAME-backup.service"
    systemctl stop "$NAME"

    echo "== import 3. SFV's rows"
    if ! sudo -u "$NAME" python3 - "$STATE/bridge.sqlite3" "$snap" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("ATTACH DATABASE ? AS sf4", (sys.argv[2],))
def migrations(schema):
    return {row[0] for row in db.execute(f"SELECT version FROM {schema}.schema_migrations")}
for schema, who in (("main", "SFV's database"), ("sf4", "SF4's snapshot")):
    if 58 not in migrations(schema):
        sys.exit(f"{who} has no ranking games (058): this is not the database this import expects")
def columns(schema, table):
    return [row[1] for row in db.execute(f"PRAGMA {schema}.table_info({table})")]
TABLES = ("skill_ratings", "rated_pairs", "ranking_choices")
for table in TABLES:
    if columns("main", table) != columns("sf4", table):
        sys.exit(f"{table} differs between the two databases: {columns('main', table)} / {columns('sf4', table)}")
    if db.execute(f"SELECT COUNT(*) FROM main.{table} WHERE game = 'sfv'").fetchone()[0]:
        sys.exit(f"SFV's database has SFV rows in {table} already; the import runs once, before anyone plays on it")
with db:
    for table in TABLES:
        names = ", ".join(columns("main", table))
        moved = db.execute(f"INSERT INTO main.{table} ({names}) SELECT {names} FROM sf4.{table} WHERE game = 'sfv'").rowcount
        print(f"  {table}: {moved} row(s)")
listed = db.execute("SELECT COUNT(*) FROM main.ranking_choices WHERE game = 'sfv' AND ranked = 1").fetchone()[0]
print(f"  {listed} SFV player(s) chose to be ranked")
# What stays with SF4's bridge: rooms are live state, and an organised match,
# lobby or tournament belongs to the connection and bridge that made it.
for table in ("rooms", "matches", "lobbies", "tournaments"):
    count = db.execute(f"SELECT COUNT(*) FROM sf4.{table} WHERE game = 'sfv'").fetchone()[0]
    if count:
        print(f"  not copied: {count} SFV row(s) in SF4's {table}")
problems = db.execute("PRAGMA main.integrity_check").fetchone()[0]
if problems != "ok":
    sys.exit(f"SFV's database after the import: {problems}")
PY
    then
        systemctl start "$NAME"
        fail "Nothing was imported (the copy runs in one transaction). SFV's bridge is started again."
    fi
    systemctl start "$NAME"
    wait_sfv
    echo "$stamp" > "$NOTES/imported"
    curl -fsS "$LOCAL/v1/ranking/board/public?game=sfv" | python3 -c '
import json, sys
print("SFV board on its own bridge:", len(json.load(sys.stdin).get("players", [])), "player(s)")' || :
    sf4_unchanged
    echo "Import done. SFV's rows stay in SF4's database as well, untouched: SF4 1.6.0 clears them (SFV_BRIDGE.md)."
}

phase_site() {
    check_stage
    sf4_hold
    [ -s "$NOTES/bridge-id" ] && systemctl is-active --quiet "$NAME" || fail "SFV's bridge is not running: run the bridge phase first."
    [ -f "$NOTES/imported" ] || fail "Run the import phase first, so the SFV board does not show empty."
    [ -d "$SHORT" ] || fail "No $SHORT (the site's operator folder)."
    echo "== site 1. /rooms-sfv.json, /rankings-sfv.json and /link-sfv/ from SFV's bridge, and its links"
    [ -f "$NOTES/ember-short-before-sfv-bridge.tgz" ] || tar -C "$SHORT" -czf "$NOTES/ember-short-before-sfv-bridge.tgz" setup.sh nginx site
    (cd "$STAGE/ember-short" && find setup.sh nginx site -type f) | while read -r f; do
        install -D -o "$SUDO_USER" -g "$SUDO_USER" -m 0644 "$STAGE/ember-short/$f" "$SHORT/$f"
    done
    bash "$SHORT/setup.sh"
    echo "== site 2. served"
    local site=(curl -fsS --resolve embernetplay.link:443:127.0.0.1)
    "${site[@]}" https://embernetplay.link/rooms-sfv.json | python3 -c '
import json, sys
d = json.load(sys.stdin)
found, want = d.get("bridge_id"), sys.argv[1]
if found != want:
    sys.exit("rooms-sfv.json comes from bridge %s, not from the SFV bridge %s" % (found, want))
print("rooms-sfv.json:", len(d["rooms"]), "room(s) from the SFV bridge")' "$(cat "$NOTES/bridge-id")"
    "${site[@]}" https://embernetplay.link/rankings-sfv.json | python3 -c 'import json, sys; print("rankings-sfv.json:", len(json.load(sys.stdin).get("players", [])), "player(s)")'
    "${site[@]}" https://embernetplay.link/rooms.json | python3 -c 'import json, sys; print("rooms.json:", len(json.load(sys.stdin)["rooms"]), "room(s) from the SF4 bridge")'
    local code
    code=$(curl -sS -o /dev/null -w '%{http_code}' --resolve embernetplay.link:443:127.0.0.1 "https://embernetplay.link/link-sfv/emt_00000000-0000-4000-8000-000000000000")
    [ "$code" = 404 ] || fail "/link-sfv/ answers $code (404 from SFV's bridge expected)."
    echo "/link-sfv/: SFV's bridge answers"
    sf4_unchanged
    echo "Site done. Rollback: sudo bash $STAGE/upgrade.sh rollback-site"
}

phase_check() {
    echo "SF4 bridge  $(short "$(sha "$SF4_LIB/ember-bridge")") ($(systemctl is-active ember-bridge))"
    if [ -f "$LIB/ember-bridge" ]; then
        echo "SFV bridge  $(short "$(sha "$LIB/ember-bridge")") ($(systemctl is-active "$NAME")) $(cat "$NOTES/bridge-id" 2>/dev/null)"
        echo "SFV backups $(systemctl is-active "$NAME-backup.timer"), newest: $(ls -1t /var/backups/$NAME/bridge-*.sqlite3 2>/dev/null | head -1)"
        echo "imported    $(cat "$NOTES/imported" 2>/dev/null || echo no)"
        curl -fsS "$LOCAL/v1/rooms/public?game=sfv" | python3 -c 'import json, sys; print("SFV rooms  ", len(json.load(sys.stdin)["rooms"]))' || echo "SFV rooms   no answer"
        curl -fsS "$LOCAL/v1/ranking/board/public?game=sfv" | python3 -c 'import json, sys; print("SFV board  ", len(json.load(sys.stdin).get("players", [])))' || echo "SFV board   no answer"
    else
        echo "SFV bridge  not installed"
    fi
    curl -fsS "$SF4_LOCAL/v1/rooms/public" | python3 -c 'import json, sys; print("SF4 rooms  ", len(json.load(sys.stdin)["rooms"]))' || echo "SF4 rooms   no answer"
    curl -fsS "$SF4_LOCAL/v1/rooms/public?game=sfv" | python3 -c 'import json, sys; print("SFV rooms on SF4 bridge (older SFV builds):", len(json.load(sys.stdin)["rooms"]))' || :
    if grep -q '127.0.0.1:47822' "$SHORT_SNIPPET" 2>/dev/null; then echo "site        SFV feeds from SFV's bridge"; else echo "site        SFV feeds from SF4's bridge"; fi
}

rollback_site() {
    sf4_hold
    [ -f "$NOTES/ember-short-before-sfv-bridge.tgz" ] || fail "No site backup recorded. Nothing done."
    tar -C "$SHORT" -xzf "$NOTES/ember-short-before-sfv-bridge.tgz"
    chown -R "$SUDO_USER:$SUDO_USER" "$SHORT/setup.sh" "$SHORT/nginx" "$SHORT/site"
    bash "$SHORT/setup.sh"
    sf4_unchanged
    echo "Rolled back: the site as it was before the site phase (SFV feeds from SF4's bridge)."
}

rollback_bridge() {
    sf4_hold
    if grep -q '127.0.0.1:47822' "$SHORT_SNIPPET" 2>/dev/null; then
        fail "The site still reads SFV's bridge: sudo bash $STAGE/upgrade.sh rollback-site first."
    fi
    systemctl disable --now "$NAME-backup.timer" "$NAME" 2>/dev/null || :
    if [ -e "$SITE_ENABLED" ] || [ -L "$SITE_ENABLED" ]; then
        rm -f "$SITE_ENABLED"
        if nginx -t; then
            systemctl reload nginx
        else
            ln -s "$SITE" "$SITE_ENABLED"
            fail "nginx -t failed without SFV's site; put its link back. Fix nginx first."
        fi
    fi
    sf4_unchanged
    echo "Rolled back: $NAME stopped and disabled, $HOST no longer served."
    echo "Kept: $STATE, /var/backups/$NAME, $LIB, the account and units, so the bridge phase brings it back as it was."
}

case "${1:-}" in
    bridge) phase_bridge ;;
    import) phase_import ;;
    site) phase_site ;;
    check) phase_check ;;
    rollback-site) rollback_site ;;
    rollback-bridge) rollback_bridge ;;
    *) sed -n '2,33p' "$0"; exit 2 ;;
esac
