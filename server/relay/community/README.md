# Hosting an Ember relay behind Nginx Proxy Manager

This folder runs an Ember relay on a machine you already use for Nginx Proxy
Manager (NPM). It is n0's `iroh-relay` 1.1.0 in one container. Ember's own US
relay runs the same server (`../README.md`); this kit is for a relay in
another region, hosted by someone else.

## What the relay does, and what it sees

Two players' helpers meet through a relay: it carries the first packets of a
connection and helps them find a direct path. Most matches then run directly
between the players. When no direct path exists, the match stays on the relay,
at a few KB/s per player.

The relay sees every player's IP address and which helper ids talk to each
other. It cannot read the traffic, which is encrypted between the players.
So: keep no access log for it (the steps below turn it off), and do not share
addresses you see in error logs.

## What you need

- The machine that runs NPM in Docker, with Docker Compose. The relay runs
  beside NPM so it can read the certificate NPM already renews. Building the
  image compiles the relay, which takes several minutes, once.
- A router where you can forward one UDP port.
- The relay's name. Ember builds pin the exact name, so it is agreed with
  Katie first and cannot change later without a new build. The examples use
  `relay-eu.embernetplay.link`: a name in Ember's domain that points at your
  proxy, so the relay can move to another host without a new build.

## Steps

### 1. The name

Katie adds the DNS record that points the name at your proxy. Go on when it
resolves:

```bash
nslookup relay-eu.embernetplay.link
```

### 2. The proxy host

In NPM, add a proxy host:

| Tab | Setting | Value |
|---|---|---|
| Details | Domain Names | `relay-eu.embernetplay.link` |
| Details | Scheme | `http` |
| Details | Forward Hostname / IP | this machine's LAN IPv4 address (not `127.0.0.1`, which is NPM's own container) |
| Details | Forward Port | `47895` |
| Details | Websockets Support | on |
| Details | Cache Assets, Block Common Exploits | off |
| SSL | SSL Certificate | Request a new SSL Certificate |
| SSL | Force SSL | **off** |
| SSL | HTTP/2 Support, HSTS | off |

Force SSL must stay off: the helper checks for a captive portal over plain
HTTP (`http://<name>/generate_204`), and a redirect there makes every player
look like they sit behind one.

On the Advanced tab, paste:

```nginx
access_log off;
proxy_read_timeout 1h;
proxy_send_timeout 1h;
proxy_buffering off;
```

Save, and wait until NPM shows the certificate as issued. The proxy host
answers 502 until the relay runs; that is expected.

### 3. The relay

Find the folder NPM keeps its certificates in (the source of its
`/etc/letsencrypt` volume):

```bash
docker inspect <npm container> --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{"\n"}}{{end}}'
```

Copy this folder to the machine, then:

```bash
cp .env.example .env
```

Set `RELAY_HOSTNAME` and `NPM_LETSENCRYPT_DIR` in `.env`, and start it:

```bash
docker compose up -d --build
```

```bash
docker logs ember-relay
```

The log's first line names the certificate it found, such as
`relay-eu.embernetplay.link with the certificate in live/npm-12`. After that
the relay logs warnings only. `docker ps` shows it as `healthy` within a
minute.

The relay uses the machine's own network (`network_mode: host`): plain HTTP
on TCP 47895 for the proxy and QUIC on UDP 7842 for players.

### 4. The router and the firewall

- Forward **UDP 7842** from the internet to this machine. TCP 80 and 443
  already reach NPM; leave them as they are.
- Never forward TCP 47895. It is plain HTTP for the proxy only.
- If the machine has its own firewall, allow UDP 7842 in, and TCP 47895 from
  Docker's networks so NPM can reach the relay, for example with ufw:

```bash
sudo ufw allow 7842/udp
```

```bash
sudo ufw allow from 172.16.0.0/12 to any port 47895 proto tcp
```

- The proxy's name also has an IPv6 address. If your router filters inbound
  IPv6, allow UDP 7842 to this machine there too. Without it the relay still
  works; players just learn only their IPv4 address from it.

### 5. Check it

From a line outside your own network (a phone hotspot will do), on Linux or
WSL with OpenSSL 3.2 or later:

```bash
bash check.sh relay-eu.embernetplay.link
```

All four checks must say `ok`:

| Check | If it fails |
|---|---|
| `/ping` answers 200 with a valid certificate | proxy host or certificate missing; relay not running (`docker ps`); 502 = NPM cannot reach TCP 47895 (wrong forward address, or the firewall) |
| `/relay` upgrades to a relay WebSocket | Websockets Support is off |
| `http://.../generate_204` reaches the relay | Force SSL is on |
| QUIC answers on UDP 7842 | the router forward or a firewall for UDP 7842 |

### 6. Tell Katie

She runs the helper's own live test against the relay (home relay, QUIC
address discovery, a relay-only connection). When it passes, the next build
pins the relay and players in its region start using it with that update.
Until then nothing uses it, so you can change or restart it freely.

## Running it

- **Certificate renewals** need nothing: NPM renews, and the relay re-reads
  the files once a day. If you delete and re-request the certificate in NPM,
  restart the relay (`docker compose restart`) so it finds the new one.
- **Restarts and reboots**: the container restarts by itself
  (`restart: unless-stopped`).
- **Updates**: Ember's relays must run the same iroh release as the helper.
  When that changes, this folder changes; copy the new one over and run
  `docker compose up -d --build`.
- **Logs**: `docker logs ember-relay` (warnings only, 10 MB at most). Leave
  `RUST_LOG` at `warn`: at `info` the relay logs every client.
- **Load**: a helper keeps up to three WebSockets open on its relay for as
  long as the game runs. Each one uses two of NPM's worker connections.

## Downtime and taking it down

Once a build pins this relay, players near it use it as their home relay. If
it stops answering, their helpers move to the next nearest relay by
themselves (iroh picks the home relay from the ones that answer its probes);
play continues, with a longer detour for relayed traffic and for the first
packets of each connection. So:

- A restart or a short outage needs no announcement.
- Before planned downtime of more than an hour or so, tell Katie.
- To stop hosting, tell Katie first. The name is in Ember's domain, so she
  can point it at another relay and no player needs a new build. Then:

```bash
docker compose down
```

Then delete the proxy host in NPM and the UDP 7842 forward on the router.
Nothing else was installed on the machine; `docker image rm ember-relay:1.1.0`
frees the image.

Before a build pins the relay, `docker compose down` undoes everything at any
time.

## Other layouts

The kit assumes NPM and the relay on one machine. Any other layout is fine
if `check.sh` and Katie's live test pass. What the relay needs:

1. `https://<name>/` (WebSocket upgrade at `/relay`, `/ping`) reaches the
   relay's HTTP port with a certificate valid for the name, and idle
   WebSockets are not cut after a few seconds.
2. `http://<name>/generate_204` reaches the relay without a redirect.
3. UDP 7842 reaches the relay with the packets' source address unchanged
   (no proxy, no Docker port mapping in between), and the relay holds a
   valid certificate and key for the name for it. `entrypoint.sh` shows the
   configuration; `RELAY_CERT_ROOT` can point it at any folder laid out like
   Let's Encrypt's (`live/<any>/fullchain.pem` and `privkey.pem`).
4. No access log.

On a machine where nothing else uses ports 80 and 443, `iroh-relay` can also
serve HTTPS itself and get its own certificate (`cert_mode = "LetsEncrypt"`).
That layout has not been tried for Ember; ask before going that way.
