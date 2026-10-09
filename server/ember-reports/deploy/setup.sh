#!/usr/bin/env bash
# Staged alongside bin/ember-reports, this unit, config.example.json and nginx/.
# sudo bash ~/ember-reports/setup.sh /root/bugsink-report-key
# The argument is a root-owned 0600 file containing the Bugsink project DSN key.
set -euo pipefail
umask 077
SRC="$(cd "$(dirname "$0")" && pwd)"
LIB=/usr/local/lib/ember-reports
ETC=/etc/ember-reports
STATE=/var/lib/ember-reports
SITE=/etc/nginx/sites-available/embernetplay.link
UNIT=/etc/systemd/system/ember-reports.service
ZONE=/etc/nginx/conf.d/ember-reports.conf
LOCATIONS=/etc/nginx/snippets/ember-reports.conf
HEADERS=/etc/nginx/snippets/ember-reports-headers.conf

[[ $(id -u) == 0 ]] || { echo "Run with sudo." >&2; exit 1; }
[[ $# == 1 ]] || { echo "usage: sudo bash setup.sh /root/bugsink-report-key" >&2; exit 2; }
KEY=$(realpath -e -- "$1")
[[ -f "$KEY" && ! -L "$1" && $(stat -c %u "$KEY") == 0 ]] || { echo "Credential must be a root-owned regular file." >&2; exit 1; }
mode=$(stat -c %a "$KEY")
(( (8#$mode & 077) == 0 )) || { echo "Credential file must have no group/other permissions (use 0600)." >&2; exit 1; }
for file in bin/ember-reports ember-reports.service config.example.json nginx/ember-reports-zone.conf nginx/ember-reports-locations.conf nginx/ember-reports-headers.conf; do
    [[ -f "$SRC/$file" ]] || { echo "Missing staged $file." >&2; exit 1; }
done
[[ -f "$SITE" ]] || { echo "Missing HTTPS site $SITE." >&2; exit 1; }
getent passwd katie >/dev/null || { echo "Owner account katie does not exist." >&2; exit 1; }
for tool in python3 nginx systemctl curl; do command -v "$tool" >/dev/null; done
# Check without printing the key, even on validation failure.
python3 - "$KEY" "$SRC/config.example.json" <<'PY'
import json, pathlib, re, sys
key = pathlib.Path(sys.argv[1]).read_bytes()
if len(key) > 256 or not re.fullmatch(rb'[A-Za-z0-9]{1,128}\s*', key):
    sys.exit('Invalid credential file.')
json.loads(pathlib.Path(sys.argv[2]).read_text())
PY

getent group ember-symbols >/dev/null || groupadd --system ember-symbols
getent group ember-reports >/dev/null || groupadd --system ember-reports
id ember-reports >/dev/null 2>&1 || useradd --system --gid ember-reports --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin ember-reports
usermod -a -G ember-symbols katie
install -d -o root -g root -m 0755 "$LIB"
install -d -o root -g ember-reports -m 0750 "$ETC"
install -d -o ember-reports -g ember-symbols -m 0710 "$STATE"
install -d -o ember-reports -g ember-symbols -m 2770 "$STATE/symbols"
install -d -o ember-reports -g ember-reports -m 0700 "$STATE/dumps" "$STATE/outbox"

# Preserve the immediate predecessor. The backup is root-private and removed
# after success; a failed install restores files and the prior service state.
backup=$(mktemp -d /var/tmp/ember-reports-install.XXXXXX)
targets=("$LIB/ember-reports" "$UNIT" "$ETC/config.json" "$ETC/bugsink_dsn_key" "$ZONE" "$LOCATIONS" "$HEADERS" "$SITE")
for i in "${!targets[@]}"; do
    [[ ! -e "${targets[$i]}" ]] || cp -a -- "${targets[$i]}" "$backup/$i"
done
active=0; systemctl is-active --quiet ember-reports && active=1
enabled=0; systemctl is-enabled --quiet ember-reports && enabled=1
changed=0
rollback() {
    local status=$?
    trap - EXIT
    if (( status != 0 && changed )); then
        echo "Install failed; restoring preceding files." >&2
        systemctl stop ember-reports || true
        for i in "${!targets[@]}"; do
            if [[ -e "$backup/$i" ]]; then cp -a -- "$backup/$i" "${targets[$i]}";
            else rm -f -- "${targets[$i]}"; fi
        done
        systemctl daemon-reload || true
        (( enabled )) || systemctl disable ember-reports || true
        if (( active )); then systemctl start ember-reports || true; fi
        nginx -t && systemctl reload nginx || true
        echo "Account/group membership and state directories are retained; backup: $backup" >&2
    elif (( status == 0 )); then
        # This absolute mktemp target is fixed to our own install backup.
        [[ "$backup" == /var/tmp/ember-reports-install.* ]] && rm -rf -- "$backup"
    fi
    exit "$status"
}
trap rollback EXIT
changed=1
install -o root -g root -m 0755 "$SRC/bin/ember-reports" "$LIB/ember-reports.new"
mv -f -- "$LIB/ember-reports.new" "$LIB/ember-reports"
install -o root -g root -m 0644 "$SRC/ember-reports.service" "$UNIT"
if [[ ! -e "$ETC/config.json" ]]; then install -o root -g ember-reports -m 0640 "$SRC/config.example.json" "$ETC/config.json"; fi
install -o root -g root -m 0600 "$KEY" "$ETC/bugsink_dsn_key.new"
mv -f -- "$ETC/bugsink_dsn_key.new" "$ETC/bugsink_dsn_key"
install -o root -g root -m 0644 "$SRC/nginx/ember-reports-zone.conf" "$ZONE"
install -o root -g root -m 0644 "$SRC/nginx/ember-reports-locations.conf" "$LOCATIONS"
install -o root -g root -m 0644 "$SRC/nginx/ember-reports-headers.conf" "$HEADERS"
python3 - "$SITE" <<'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
include = 'include snippets/ember-reports.conf;'
if not re.search(r'(?m)^\s*include\s+snippets/ember-reports\.conf\s*;', text):
    # Locate a top-level server block, then insert before its closing brace.
    # Ignore comment text while counting; refuse ambiguous HTTPS servers.
    candidates = []
    for match in re.finditer(r'(?m)^\s*server\s*\{', text):
        start = text.index('{', match.start())
        depth = 0
        end = None
        for line_match in re.finditer(r'.*(?:\n|$)', text[start:]):
            line = line_match.group().split('#', 1)[0]
            for index, char in enumerate(line):
                depth += (char == '{') - (char == '}')
                if depth == 0:
                    end = start + line_match.start() + index
                    break
            if end is not None: break
        if end is None: sys.exit('Cannot parse nginx server block; add the include by hand.')
        block = re.sub(r'(?m)#.*$', '', text[start:end])
        if re.search(r'\blisten\s+[^;]*\b443\b', block) and re.search(r'\bserver_name\s+[^;]*\bembernetplay\.link\b', block):
            candidates.append(end)
    if len(candidates) != 1: sys.exit('Expected exactly one embernetplay.link HTTPS server; add include by hand.')
    end = candidates[0]
    path.write_text(text[:end] + '    ' + include + '\n' + text[end:])
PY
nginx -t
systemctl daemon-reload
systemctl enable ember-reports
systemctl restart ember-reports
systemctl is-active --quiet ember-reports
# A read-only method check proves the HTTP listener is answering without
# consuming a report/rate token or creating player state. Read custom bind.
bind=$(python3 - "$ETC/config.json" <<'PY'
import json, pathlib, sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text()).get('bind', '127.0.0.1:47850'))
PY
)
healthy=0
for attempt in 1 2 3 4 5; do
    if [[ $(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "http://$bind/report/v1" || true) == 405 ]]; then healthy=1; break; fi
    sleep 1
done
(( healthy )) || { echo "Report listener did not answer." >&2; exit 1; }
systemctl reload nginx
# Reload can return while old workers still serve the preceding site. Wait
# for the exact HTTPS route, with TLS/SNI/Host intact and no proxy or DNS hop.
healthy=0
deadline=$((SECONDS + 10))
while (( SECONDS < deadline )); do
    remaining=$((deadline - SECONDS))
    if [[ $(curl -sS --noproxy '*' --resolve embernetplay.link:443:127.0.0.1 \
        --connect-timeout 2 --max-time "$remaining" -o /dev/null -w '%{http_code}' \
        https://embernetplay.link/report/v1 || true) == 405 ]]; then
        healthy=1
        break
    fi
    (( SECONDS < deadline )) && sleep 1
done
(( healthy )) || { echo "Report HTTPS route did not answer 405 within 10 seconds." >&2; exit 1; }
echo "ember-reports installed. katie must reconnect SSH for ember-symbols membership."
echo "Configure the Bugsink project ID in $ETC/config.json and restart after edits."
