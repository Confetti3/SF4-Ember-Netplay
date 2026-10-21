# syntax=docker/dockerfile:1
# The bracket site (server/bracket-site) on Node, which runs its TypeScript
# as it is. Build from the repository root:
#
#   docker build -t ember-bracket-site -f server/bracket-site/docker/Dockerfile .
#
# compose.yml next to this file runs it; its header has the settings.
FROM node:22-slim
# The site imports the Ember API SDK's sources by their path in the
# repository, so both keep their places under /app.
WORKDIR /app
COPY sdk/typescript/src sdk/typescript/src
COPY server/bracket-site/package.json server/bracket-site/
COPY server/bracket-site/src server/bracket-site/src
# A fixed id, so the host folder mounted at /var/lib/bracket-site can be given to it.
RUN groupadd --system --gid 4790 site \
    && useradd --system --uid 4790 --gid site --home-dir /var/lib/bracket-site \
        --no-create-home --shell /usr/sbin/nologin site
USER site
ENV BRACKET_SITE_LISTEN=0.0.0.0:8490 \
    BRACKET_SITE_DATABASE=/var/lib/bracket-site/site.sqlite3 \
    BRACKET_SITE_CONFIG=/var/lib/bracket-site/site.json \
    NODE_ENV=production
EXPOSE 8490
CMD ["node", "server/bracket-site/src/main.ts"]
