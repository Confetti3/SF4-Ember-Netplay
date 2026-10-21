# syntax=docker/dockerfile:1
# The Ember tournament bridge, bracket connector and notifier in one image, for a bridge
# run under Docker instead of bridge/deploy/setup.sh. Build from the
# repository root:
#
#   docker build -t ember-bridge -f server/ember/docker/Dockerfile .
#
# compose.yml next to this file runs it; its header has the first-run steps.
ARG DEBIAN_RELEASE=trixie
ARG RUST_VERSION=1.98

FROM rust:${RUST_VERSION}-${DEBIAN_RELEASE} AS rust
WORKDIR /src
COPY server/ember server/ember
# The image's own toolchain: rust-toolchain.toml would have rustup fetch the
# Windows target the client build needs.
RUN export RUSTUP_TOOLCHAIN="$(rustup default | cut -d' ' -f1)" \
    && cd server/ember \
    && cargo build --release --locked -p ember-bridge -p ember-brackets -p ember-notifier

FROM debian:${DEBIAN_RELEASE}-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    # A fixed id, so the host folders mounted at /var/lib/ember can be given to it.
    && groupadd --system --gid 4784 ember \
    && useradd --system --uid 4784 --gid ember --home-dir /var/lib/ember \
        --no-create-home --shell /usr/sbin/nologin ember
COPY --from=rust /src/server/ember/target/release/ember-bridge /src/server/ember/target/release/ember-brackets /src/server/ember/target/release/ember-notifier /usr/local/bin/
USER ember
ENTRYPOINT ["tini", "--"]
