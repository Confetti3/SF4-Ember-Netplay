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

Symbols come from the **exact release binary and matching PDB**, on the build
machine that has that PDB, and only for the modules a game crash dump shows:
`Sidecar.dll` and the vcpkg DLLs it loads, `GGPO.dll`, `spdlog.dll`,
`fmt.dll` and `zlib1.dll`. Launcher.exe, Updater.exe and sf4-net.exe are
separate processes and never appear in a game dump; SSFIV.exe and
`discord_partner_sdk.dll` have no PDB.

The publish paths do this themselves. `scripts/build-current.ps1 -Symbols`
(passed by `publish-nightly.ps1` and `release-team-build.ps1`) runs Mozilla
`dump_syms -s` in WSL Ubuntu (`~/.cargo/bin/dump_syms`, from
`cargo install dump_syms --locked`) on each DLL beside its PDB, checks that it
is the staged binary and that the symbol file has functions, writes the store
layout to `build/current/symbols` and records it in `build-provenance.json`.
Once a release has its label, `publish-nightly.ps1` and `github-release.ps1`
run `scripts/upload-symbols.ps1`, which refuses a build without Sidecar's
symbols and sends [install-symbols.sh](install-symbols.sh) over
`ssh vps` as katie:

1. `prepare` makes `/var/lib/ember-reports/symbols/.incoming-<channel>-<label>`;
2. scp copies the store's folders there;
3. `commit` sets group read on them, moves each complete `<debug file>/<DEBUGID>`
   folder into the store with a rename (one already there is kept), writes
   `~/symbols-manifests/<channel>-<label>.txt` and deletes the folders of
   Nightly releases older than 45 days that no other release lists;
4. `cleanup` removes the upload folder.

Stable symbols are never pruned. To upload by hand, for example after a
publish that failed before its upload, run
`pwsh -File scripts/upload-symbols.ps1 -Label <version>` from the checkout
whose build it was. These options refer to
[Mozilla's dump_syms](https://github.com/mozilla/dump_syms/blob/main/src/main.rs).

Preserve the tool's case/layout: `<module debug filename>/<DEBUGID>/<module>.sym`.
For Windows PDBs this can be `Sidecar.pdb/<GUID-and-age>/Sidecar.sym`, rather
than a directory named Sidecar.dll. The Breakpad lookup uses the dump's
debug filename and identifier, stripping `.pdb` from the symbol leaf. Executable
debug names can instead use `Launcher.exe/<DEBUGID>/Launcher.exe.sym`. The
synthetic test exercises actual local-path loading with that layout.

The upload folder inside the store keeps partly uploaded `.sym` files from
being read, and scp's modes are not controlled by the service's UMask, so
install-symbols.sh runs with `umask 002` and `chmod -R g+rX` before the move.
If the service cannot read a `.sym`, the
event's `extra.symbolication` reports `partial: symbols unreadable for
<store-relative name>` (alongside missing or other per-module problems), and
the journal contains one `ember-reports: symbols unreadable: <store-relative name>`
line per affected file per walk. These diagnostics omit absolute paths, report
IDs, addresses, contents and credentials. Fix the group permissions and resubmit
the dump to symbolicate it; already-built events are not rewritten.
Symbol files above 64 MiB, or beyond 96 MiB total for a report, are skipped
(Sidecar.sym measured 29.8 MiB at 1.5.0). Only trusted build accounts should
belong to `ember-symbols`.

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
