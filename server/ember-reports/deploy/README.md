# Deploying ember-reports

Build in WSL (see [../README.md](../README.md)). Stage this folder as
`~/ember-reports/` on the VPS with the Linux binary in `bin/ember-reports`:

```sh
# From server/ember-reports in WSL:
ssh vps 'mkdir -p ~/ember-reports/bin ~/ember-reports/nginx'
scp target/release/ember-reports vps:ember-reports/bin/
scp deploy/setup.sh deploy/ember-reports.service deploy/config.example.json vps:ember-reports/
scp deploy/nginx/*.conf vps:ember-reports/nginx/
```

Bugsink must already accept envelopes on `127.0.0.1:47860` for Host
`bugs.embernetplay.link`; this script does not install it. Get its project ID and
public DSN key (the DSN username, not the full DSN or an account password). Put
the key in a root-owned 0600 file on the VPS without exposing it in shell
arguments, then run:

```sh
sudo bash ~/ember-reports/setup.sh /root/bugsink-report-key
sudoedit /etc/ember-reports/config.json
# Set project_id to the actual Bugsink project ID. Restart after edits.
sudo systemctl restart ember-reports
```

The owner runs setup with sudo. It creates the dedicated user/groups, adds
katie to `ember-symbols`, installs the binary/unit/key, creates config only if
missing, and includes the exact report location in the existing HTTPS site.
It runs `nginx -t` before reload and checks the local listener using GET (405).
After reload it polls HTTPS `/report/v1` through local nginx with
`--resolve embernetplay.link:443:127.0.0.1`, preserving certificate checks and
the public Host/SNI. Setup succeeds only after GET returns 405, within 10 seconds;
a timeout or wrong route fails setup and invokes the same restore path.
No firewall ports are opened or test reports submitted. Rerunning updates the
binary/unit/snippets/key while preserving configuration. Failures restore the
immediate predecessor's files and service state; created accounts/groups and
state directories remain. Failed-install backups under `/var/tmp` are private
to root and include the credential; remove them after recovery.

| Installed path | Purpose |
|---|---|
| `/usr/local/lib/ember-reports/ember-reports` | root-owned binary |
| `/etc/ember-reports/config.json` | 0640 root:ember-reports settings |
| `/etc/ember-reports/bugsink_dsn_key` | 0600 root:root systemd credential source |
| `/var/lib/ember-reports/` | 0710 ember-reports:ember-symbols state |
| `.../dumps/`, `.../outbox/` | 0700 private player data, files 0600 |
| `.../symbols/` | 2770 shared symbol store, read-only in service namespace |
| `/etc/systemd/system/ember-reports.service` | hardened unit and memory/CPU bounds |
| `/etc/nginx/conf.d/ember-reports.conf` | HTTP-context rate zone |
| `/etc/nginx/snippets/ember-reports.conf` | exact POST `/report/v1` proxy |
| `/etc/nginx/snippets/ember-reports-headers.conf` | HSTS, nosniff and no-referrer for the proxy and its 429 response |

The key reaches the service only as
`$CREDENTIALS_DIRECTORY/bugsink_dsn_key`. Envelope delivery sets `X-Sentry-Auth`
and the configured Host, with three attempts (250 ms then 1 s backoff, each
HTTP attempt bounded at 3 s). Replay runs on new-report notifications and every
minute, rotating batches of up to 16 pending events. Transport errors, 5xx,
redirects, 401/403/404/405/408 and 429 pause a batch after those retries. Other
HTTP rejections and local read/delete failures retain the affected event and
continue the batch. Rotation prevents retained failures from starving later
reports. Non-2xx responses remain in the bounded outbox for operator repair. The
event ID stays unchanged; remote acceptance followed by a crash before local
deletion can resend the same ID.

Configuration can tighten limits within hard bounds. Bind and Bugsink are
IPv4 loopback only, and the bind port must stay 47850 because the supplied
nginx proxies there; the service refuses to start on any other port. The unit
uses the default state path, so update both together if editing `state_dir`. `Group=ember-symbols` permits katie to
traverse state to symbols; `SupplementaryGroups=ember-reports` lets the service
read private configuration. Reconnect katie's SSH session after setup for the
new group membership. The service mounts symbols read-only; katie uploads
outside its mount namespace.

## Symbols

Generate symbols from the **exact release binary and matching PDB**, on the
build machine that has that PDB. `dump_syms -s` creates the store layout:

```sh
umask 002
dump_syms -s symbols Launcher.exe
dump_syms -s symbols Sidecar.dll
dump_syms -s symbols Updater.exe
dump_syms -s symbols sf4-net.exe
chmod -R g+rX symbols/
scp -r symbols/. vps:/var/lib/ember-reports/symbols/
# On the VPS as katie, also fix modes preserved by scp (including old uploads):
umask 002
chmod -R g+rX /var/lib/ember-reports/symbols/
```

These options refer to [Mozilla's dump_syms](https://github.com/mozilla/dump_syms/blob/main/src/main.rs).

Preserve the tool's case/layout: `<module debug filename>/<DEBUGID>/<module>.sym`.
For Windows PDBs this can be `Sidecar.pdb/<GUID-and-age>/Sidecar.sym`, rather
than a directory named Sidecar.dll. The Breakpad lookup uses the dump's
debug filename and identifier, stripping `.pdb` from the symbol leaf. Executable
debug names can instead use `Launcher.exe/<DEBUGID>/Launcher.exe.sym`. The
synthetic test exercises actual local-path loading with that layout.

In production, scp into a temporary sibling first, ensure directories are
group-traversable and files group-readable, then move complete DEBUGID
directories into the store on the VPS. This prevents partially uploaded `.sym`
files being read. scp's modes are not controlled by the service's UMask. Symbol
uploads need `umask 002` and `chmod -R g+rX` on the uploaded tree before moving
complete directories into the store. If the service cannot read a `.sym`, the
event's `extra.symbolication` reports `partial: symbols unreadable for
<store-relative name>` (alongside missing or other per-module problems), and
the journal contains one `ember-reports: symbols unreadable: <store-relative name>`
line per affected file per walk. These diagnostics omit absolute paths, report
IDs, addresses, contents and credentials. Fix the group permissions and resubmit
the dump to symbolicate it; already-built events are not rewritten.
Symbol files above 8 MiB, or beyond 16 MiB total for a report, are skipped. Only
trusted build accounts should belong to `ember-symbols`.

SSFIV.exe has no symbols. Its frames remain `SSFIV.exe+0x<RVA>`; add its static
0x400000 image base to locate the IDA effective address in the matching binary.
The Sentry `instruction_addr` already contains the absolute instruction address.

## Privacy and checks

Reports carry schema/build/Windows/crash metadata, optional comment, up to four
redacted logs and an optional dump. Dumps may expose process memory. Dumps
expire within 30 days or sooner at 2 GiB/4096 files; pending JSON is capped at
512 files/256 MiB and deleted on delivery. Bugsink retention is configured
separately. There are no addresses or report contents in service logs. nginx
streams bodies, limits them to 5 MiB, accepts only exact POST `/report/v1`,
applies 6/minute with burst 3, and disables report access logging.

```sh
sudo systemctl status ember-reports
sudo journalctl -u ember-reports -f
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:47850/report/v1  # 405
sudo du -sh /var/lib/ember-reports/dumps /var/lib/ember-reports/outbox
```

These establish a listener/state footprint only. After installation, submit a
deliberately synthetic report, record the 202 ID, confirm it in Bugsink and
confirm its outbox JSON disappears. Then test a real release dump against its
own symbols. Live nginx/systemd/Bugsink deployment and real launcher dumps were
not verified during the initial Windows edit-only task.
