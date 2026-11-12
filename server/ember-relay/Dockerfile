# syntax=docker/dockerfile:1
# An iroh relay for Ember: the iroh-relay server binary, built from iroh's own
# tagged source so its lock file pins every dependency. 1.4.1 is newer than
# the iroh the helper pins (1.1.0): the relay protocol has been stable since
# iroh 0.91 and the relay releases since 1.1 carry server fixes (1.0.2's
# security fix among them), so the server tracks the latest relay.
#
#   docker build -t ember-relay .
#
# README.md says what a relay does for Ember and how to run the image.
ARG DEBIAN_RELEASE=trixie
ARG RUST_VERSION=1.98
ARG IROH_RELAY_VERSION=1.4.1

FROM rust:${RUST_VERSION}-${DEBIAN_RELEASE} AS build
ARG IROH_RELAY_VERSION
RUN cargo install iroh-relay --git https://github.com/n0-computer/iroh --tag v${IROH_RELAY_VERSION} \
        --features server --locked --root /out

FROM debian:${DEBIAN_RELEASE}-slim
ARG IROH_RELAY_VERSION
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    # A fixed id, next to ember-rooms (4783) and the bridge (4784).
    && groupadd --system --gid 4785 ember-relay \
    && useradd --system --uid 4785 --gid ember-relay --home-dir /var/lib/ember-relay \
        --no-create-home --shell /usr/sbin/nologin ember-relay \
    && install -d -o ember-relay -g ember-relay -m 0700 /var/lib/ember-relay
COPY --from=build /out/bin/iroh-relay /usr/local/bin/iroh-relay
LABEL org.opencontainers.image.title="ember-relay" \
      org.opencontainers.image.description="iroh relay for Ember rooms" \
      ember.iroh_relay_version=${IROH_RELAY_VERSION}
# The relay logs what it serves and refuses; without a filter it says nothing.
ENV RUST_LOG=info
USER ember-relay
# Certificates land here (config.example.toml, cert_dir).
WORKDIR /var/lib/ember-relay
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/iroh-relay", "--config-path", "/etc/ember-relay/config.toml"]
