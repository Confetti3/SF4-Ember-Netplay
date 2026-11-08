# An Ember relay

Ember's helper is an [iroh](https://www.iroh.computer/) endpoint. Two players
find each other through a relay server, send their first packets through it
while they punch a hole in their routers, and fall back to it when no direct
path exists. The helper also measures its public address against the relay
over QUIC. Today every client uses the four public relays run by n0
(`*.relay.n0.iroh.link`), and the web filter in some US cable gateways has
blocked three of them on port 443, leaving those players one relay in
Europe. A relay Ember runs is not on anybody's list and sits where the
players are.

This folder builds that relay as a container: the `iroh-relay` server from
iroh's tagged source, a system user, and a settings file. Clients do not use
it yet; the helper's relay map and the relay dictionary of room codes change
together when it does.

## What it needs

A machine with a public address, a DNS name for it, Docker Engine with the
compose plugin, and ports:

| Port | Standalone | Behind a reverse proxy |
| --- | --- | --- |
| TCP 443 | the relay, HTTPS and WebSocket | the proxy, forwarding to the relay's HTTP port with WebSocket upgrades on |
| TCP 80 | Let's Encrypt challenge and the captive portal check (`/generate_204`) | the proxy, serving the same path over plain HTTP, no redirect to HTTPS |
| UDP 7842 | QUIC address discovery | not available: it needs the certificate in the relay |

A relay moves little data (the handshake and the fallback path of a match;
direct connections carry the rest) and runs in well under 512 MB.

## Run

```
cd server/ember-relay
cp config.example.toml config.toml   # set the hostname; pick proxy or standalone as the comments say
docker compose up -d --build
```

The first build compiles `iroh-relay` (a few minutes). The image is also
built alone with `docker build -t ember-relay .` in this folder, and moved
without a registry with
`docker save ember-relay | gzip | ssh vps 'gunzip | sudo docker load'`.

Standalone, the relay asks Let's Encrypt for its certificate on the first
start and keeps it in `./certs`. Behind a proxy, point the proxy's host
entry at `http://<this machine>:3340`, WebSocket support on, HTTP not forced
to HTTPS.

## Check

```
curl -i https://relay.example.org/ping          # 200, empty body
curl -i http://relay.example.org/generate_204   # 204, the helper's captive portal check
```

An iroh endpoint whose relay map holds only this relay
(`RelayMode::custom(["https://relay.example.org/"])`) reports it as its home
relay within a few seconds. Metrics are at `http://127.0.0.1:9090/metrics`
on the machine. The relay admits every endpoint, as n0's public relays do,
and limits each client to 4 MiB/s (`config.example.toml`).
