#!/usr/bin/env bash
# Installs the Pebble CLI + SDK in a fresh (cloud/headless) Linux container.
# Usage: scripts/setup-pebble.sh
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"

command -v pebble >/dev/null || uv tool install pebble-tool
ls "$HOME/.local/share/pebble-sdk/SDKs" 2>/dev/null | grep -q . || pebble sdk install latest

# Containers without IPv6 can't bind pypkjs's default ("" -> ::) listener,
# which breaks `pebble install --emulator`. Bind IPv4 instead.
ws="$(uv tool dir)/pebble-tool/lib/python3*/site-packages/pypkjs/runner/websocket.py"
# shellcheck disable=SC2086
sed -i 's/pywsgi.WSGIServer(("", self.port)/pywsgi.WSGIServer(("0.0.0.0", self.port)/' $ws

npm install
pebble build
