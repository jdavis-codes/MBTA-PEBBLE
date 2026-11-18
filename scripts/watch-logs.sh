#!/usr/bin/env bash
# Captures logs from the real watch and phone to a file, reconnecting whenever
# the phone drops off, until stopped or MINUTES have passed.
# Usage: scripts/watch-logs.sh [MINUTES] [FILE]
set -u
export PATH="$HOME/.local/bin:$PATH" PYTHONWARNINGS=ignore PYTHONUNBUFFERED=1
minutes="${1:-110}"
out="${2:-build/watch-logs.txt}"
mkdir -p "$(dirname "$out")"
end=$(( $(date +%s) + minutes * 60 ))
while [ "$(date +%s)" -lt "$end" ]; do
  echo "=== $(date -u +%FT%TZ) connecting" >>"$out"
  timeout $(( end - $(date +%s) + 1 )) pebble logs --cloudpebble >>"$out" 2>&1 </dev/null
  echo "=== $(date -u +%FT%TZ) disconnected" >>"$out"
  sleep 5
done
