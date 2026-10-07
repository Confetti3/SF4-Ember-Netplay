# syntax=docker/dockerfile:1
# The Ember public room service in one image: the ember-rooms supervisor and
# the sf4e-room-host and sf4-net pair of one client build. Build from the
# repository root, with the client release's sidecar build hash:
#
#   docker build --build-arg BUILD_ID=<hash> -t ember-rooms:<hash> \
#       -f server/ember-rooms/docker/Dockerfile .
#
# A room host admits only clients of its own build, so an image is one build
# and is tagged with it; a new client release is a new image, run next to the
# old one until the old rooms have ended (docker/README.md).
#
# The three stages share one Debian release, so the room host (which links the
# C++ runtime statically and needs only glibc) runs on the glibc it was built
# against. The build-linux.sh flags and the cargo profiles are the same ones a
# bare-metal install uses.
ARG DEBIAN_RELEASE=trixie
ARG RUST_VERSION=1.98

# --- The supervisor and the helper.
FROM rust:${RUST_VERSION}-${DEBIAN_RELEASE} AS rust
WORKDIR /src
# The helper's path dependencies (server/ember, server/ember-short) come along.
COPY server/ember server/ember
COPY server/ember-short server/ember-short
COPY server/ember-rooms server/ember-rooms
COPY rust/sf4-net rust/sf4-net
# The image's own toolchain: rust/sf4-net/rust-toolchain.toml would have
# rustup fetch the Windows target and lint components the client build needs.
RUN export RUSTUP_TOOLCHAIN="$(rustup default | cut -d' ' -f1)" \
    && cd server/ember-rooms && cargo build --release --locked \
    && cd /src/rust/sf4-net && cargo build --profile server --locked

# --- The room host, with the distribution's header-only spdlog, fmt and json.
FROM debian:${DEBIAN_RELEASE} AS cpp
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ nlohmann-json3-dev libspdlog-dev libfmt-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src src
COPY server/roomhost server/roomhost
# A Windows checkout gives the scripts CRLF line ends, which bash refuses.
RUN sed -i 's/\r$//' server/roomhost/build-linux.sh \
    && bash server/roomhost/build-linux.sh /src /out/sf4e-room-host \
    && ldd /out/sf4e-room-host

# --- The service.
FROM debian:${DEBIAN_RELEASE}-slim
ARG BUILD_ID
RUN test -n "$BUILD_ID" || { echo "docker build needs --build-arg BUILD_ID=<sidecar build hash>" >&2; exit 1; } \
    && echo "$BUILD_ID" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' \
    || { echo "BUILD_ID must be 1 to 128 letters, digits, dots, dashes or underscores" >&2; exit 1; }
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    # A fixed id, so the host can make /etc/ember-rooms readable to it.
    && groupadd --system --gid 4783 ember-rooms \
    && useradd --system --uid 4783 --gid ember-rooms --home-dir /var/lib/ember-rooms \
        --no-create-home --shell /usr/sbin/nologin ember-rooms \
    && install -d -o ember-rooms -g ember-rooms -m 0700 /var/lib/ember-rooms \
    && install -d -m 0755 /etc/ember-rooms /usr/local/lib/ember-rooms/builds
COPY --from=rust /src/server/ember-rooms/target/release/ember-rooms /usr/local/lib/ember-rooms/ember-rooms
COPY --from=rust /src/rust/sf4-net/target/server/sf4-net /usr/local/lib/ember-rooms/builds/${BUILD_ID}/sf4-net
COPY --from=cpp /out/sf4e-room-host /usr/local/lib/ember-rooms/builds/${BUILD_ID}/sf4e-room-host
COPY server/ember-rooms/deploy/config.example.json /usr/local/lib/ember-rooms/config.example.json
COPY --chmod=0755 server/ember-rooms/docker/entrypoint.sh /usr/local/lib/ember-rooms/docker-entrypoint.sh
# As for build-linux.sh: a CRLF entrypoint would not start.
RUN sed -i 's/\r$//' /usr/local/lib/ember-rooms/docker-entrypoint.sh
# MALLOC_ARENA_MAX: as in deploy/ember-rooms.service, for the supervisor and
# every room host and helper it starts.
ENV MALLOC_ARENA_MAX=2 \
    HOME=/var/lib/ember-rooms \
    EMBER_ROOMS_BUILD_ID=${BUILD_ID}
LABEL org.opencontainers.image.title="ember-rooms" \
      org.opencontainers.image.description="Ember public room supervisor with the room host and helper of one client build" \
      ember.build_id=${BUILD_ID}
USER ember-rooms
WORKDIR /var/lib/ember-rooms
# SIGTERM drains: no new rooms, open ones run until empty or drain_secs; the
# run must allow that long (stop_grace_period in compose.yml).
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/lib/ember-rooms/docker-entrypoint.sh"]
