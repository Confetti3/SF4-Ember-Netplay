# An Ember relay: n0's iroh-relay 1.1.0, the same release as the helper's
# iroh, built from crates.io with the lock file the crate ships (--locked).
FROM rust:1.98-slim-trixie AS build
RUN cargo install --locked iroh-relay --version =1.1.0 --features server --root /out

FROM debian:trixie-slim
# openssl picks the certificate for the relay's name at start; curl is the
# health check.
RUN apt-get update \
    && apt-get install -y --no-install-recommends openssl curl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/iroh-relay /usr/local/bin/iroh-relay
COPY --chmod=0755 entrypoint.sh /usr/local/bin/ember-relay
ENTRYPOINT ["/usr/local/bin/ember-relay"]
