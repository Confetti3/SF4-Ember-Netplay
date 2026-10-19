#!/usr/bin/env bash
# Builds the Decky plugin zip for "Install plugin from ZIP": out/Ember.zip
set -euo pipefail
cd "$(dirname "$0")"
pnpm install --frozen-lockfile
pnpm build
rm -rf out && mkdir -p out/Ember/dist
cp dist/index.js out/Ember/dist/
cp plugin.json package.json main.py ../steamdeck-install.sh ../../../LICENSE out/Ember/
python3 -m zipfile -c out/Ember.zip out/Ember
