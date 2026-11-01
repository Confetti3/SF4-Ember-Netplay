# Replay archive deployment

The archive page (`../site`), the bucket it links to, and the proxy in front
of both, on the VPS. No Docker: rustfs runs as a systemd service on loopback
and the existing nginx serves the site. Design: `docs/design/REPLAY_ARCHIVE.md`.

What runs where, for staging:

- rustfs 1.0.4 (musl binary) on `127.0.0.1:9400`, console off, data in
  `/var/lib/rustfs-archive`. The bridge writes to it with the bucket's key.
- `https://bridge-staging.embernetplay.link/archive/` is the page, on the
  staging bridge's own host (no DNS name of its own). `/archive/v1/archive` is
  the staging bridge's listing (`127.0.0.1:47821`) behind a rate limit and a
  15 second cache, and `/archive/replays/...` is the bucket's public read
  proxy. The bridge's own `/v1/archive` on that host is 404, so the listing
  has one way in. These locations are a snippet,
  `/etc/nginx/snippets/ember-bridge-staging-archive.conf`, which the bridge's
  site includes (`server/ember/bridge/deploy/nginx/bridge.conf.in`).
- The public can fetch `/archive/replays/games/<64 hex>.emberreplay` and
  `/archive/replays/sets/<match id>.emberpack` with GET, HEAD and OPTIONS. Any
  other path is 404 and any other method is 405, so the bucket cannot be listed.
  The bucket policy says the same for rustfs itself (`bucket-policy.json.in`).

## Install

Staging needs no DNS. Install the bridge first with a `bridge.conf.in` that
includes the archive snippet (the bridge's `setup.sh`); this script refuses
until the bridge's enabled site has that include. A site of its own (below)
needs its name to resolve to the VPS; the `embernetplay.link` wildcard
certificate the bridge sites use covers it.

Put the files in a folder in your home, as for the bridge. From the repository:

    ssh vps mkdir -p ember-archive-staging
    scp -r server/ember/archive-site/deploy/. vps:ember-archive-staging/
    scp -r server/ember/archive-site/site vps:ember-archive-staging/site
    scp -r server/ember-short/deploy/site/assets vps:ember-archive-staging/short-assets
    scp <rustfs-1.0.4-linux-x86_64-musl binary> vps:ember-archive-staging/rustfs

Build the binary or take the release; `rustfs --version` says `1.0.4` and
`file rustfs` says statically linked. Then on the VPS:

    sudo bash ~/ember-archive-staging/setup.sh ~/ember-archive-staging/rustfs

Run it again for a newer binary, page or configuration. It is safe to repeat:
the credentials, the data and the bucket stay, and rustfs restarts only when
the binary or the unit changed. Options:

- `--replace-site` replaces this script's own site file when it differs from
  the one the script writes (it refuses otherwise). Another enabled nginx file
  that serves the same name is always refused; disable it yourself.
- A failed run is rolled back: the previous rustfs binary, unit, nginx files
  and page are put back and the previous rustfs is restarted (a first install
  is removed). The run ends only after the host serves this run's own
  `install.txt` marker through nginx. If putting the files back fails, the
  backup is kept and its path printed, and the script exits non-zero.
- `HOST`, `PREFIX`, `BRIDGE_NAME`, `WEBROOT`, `BRIDGE_PORT`, `BUCKET`,
  `S3_PORT` in the environment (the top of `setup.sh` lists them) install
  another site from the same files. Production is a site of its own at
  `archive.embernetplay.link` (its DNS record exists): `HOST=archive.embernetplay.link
  PREFIX= WEBROOT=/var/www/ember-archive BRIDGE_PORT=47820 BUCKET=replays-live`,
  from `~/ember-archive`. It shares the one rustfs; its zone file and cache
  are named after the host.

The script makes the bucket and applies the policy through `bucket.sh`, which
you can run alone:

    sudo bash bucket.sh /etc/rustfs-archive/rustfs.env http://127.0.0.1:9400 replays bucket-policy.json

(render `bucket-policy.json.in` first: replace `@BUCKET@`). By hand, with the
env file's two values:

    curl --aws-sigv4 aws:amz:us-east-1:s3 --user "$ACCESS:$SECRET" -X PUT http://127.0.0.1:9400/replays
    curl --aws-sigv4 aws:amz:us-east-1:s3 --user "$ACCESS:$SECRET" -X PUT \
         -H 'Content-Type: application/json' --data-binary @policy.json 'http://127.0.0.1:9400/replays?policy'

## Point the bridge at it

The credentials were made on the first run in `/etc/rustfs-archive/rustfs.env`
(root only). `RUSTFS_ACCESS_KEY` is the bridge's `access_key`; its
`RUSTFS_SECRET_KEY` is the `archive_secret_key` secret. Read them with
`sudo cat /etc/rustfs-archive/rustfs.env`.

In `tenants.staging.json` (the staging bridge's deployed settings), add:

    "archive": {
      "endpoint": "http://127.0.0.1:9400",
      "bucket": "replays",
      "access_key": "emberarchive",
      "public_base": "https://bridge-staging.embernetplay.link/archive/replays"
    }

(the repository's `tenants.staging.json` has it). Store the secret key first,
since the bridge does not start with an archive and no key:

    sudo bash ~/ember-bridge-staging/set-integration-secret.sh archive /etc/rustfs-archive/rustfs.env

then run `~/ember-bridge-staging/setup.sh` to apply the tenants file. The
bridge must already be a build that knows `archive_secret_key` (an older one
refuses the secrets file).

## Check it works

setup.sh ends with checks. By hand, from anywhere:

    curl -si https://bridge-staging.embernetplay.link/archive/v1/archive | head -20   # 200, JSON; X-Cache: MISS then HIT
    curl -sI https://bridge-staging.embernetplay.link/v1/archive                      # 404
    curl -sI https://bridge-staging.embernetplay.link/archive/replays/                # 404
    curl -sI -X POST https://bridge-staging.embernetplay.link/archive/replays/games/$(printf 0%.0s {1..64}).emberreplay   # 405
    curl -s -o /dev/null -w '%{http_code}\n' 'http://127.0.0.1:9400/replays?list-type=2'   # on the VPS: 403

Then open the page, pick a game and download it. The game's file name is its
digest and the download is an attachment. `sudo systemctl status
rustfs-archive` and `sudo journalctl -u rustfs-archive` show the service.

## Limits to know

- `/v1/archive` allows 2 requests a second per address (burst 10), 6 at once
  for the whole site, and reuses an answer for 15 seconds per full URL. A
  caller can still bypass the cache with many distinct query strings; the
  per-address and site-wide limits bound what reaches the bridge.
- The bucket proxy allows 20 requests a second per address (burst 40) and 8
  connections per address. Games are cached for a year (immutable, named by
  digest); a set's pack for 5 minutes, since it is written again as games join.
- OPTIONS is forwarded to rustfs, which answers it without CORS headers; the
  page and the downloads are same origin and do not need it.
- `MemoryMax=2G` caps rustfs in the unit; the VPS has 8 GB shared with the
  bridges and nginx.
