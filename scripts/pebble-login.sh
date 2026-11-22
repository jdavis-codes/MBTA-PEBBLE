#!/usr/bin/env bash
# Signs in to the Pebble account from a machine with no browser.
# Usage: scripts/pebble-login.sh [LOGFILE]
#
# `pebble login` prints a link, then waits for the browser to come back to
# localhost:60000, which only works when the browser is on the same machine.
# The address the browser comes back to is always "?broker_status=ok", and all
# it does is tell the tool to start asking the sign-in service whether the
# sign-in is done. So this script sends that itself, straight away, and lets
# the tool keep asking for ten minutes while the link is opened anywhere.
# Nothing has to be pasted back.
set -uo pipefail
export PATH="$HOME/.local/bin:$PATH" PYTHONWARNINGS=ignore PYTHONUNBUFFERED=1
log="${1:-build/login.log}"
mkdir -p "$(dirname "$log")"

# The tool gives up asking after 30 seconds (60 tries); make it ten minutes.
account="$(ls "$(uv tool dir)"/pebble-tool/lib/python3*/site-packages/pebble_tool/firebase_account.py)"
sed -i 's/for _ in range(60):/for _ in range(1200):/' "$account"
grep -q 'for _ in range(1200):' "$account" || { echo "pebble-login: the tool's sign-in code has changed; paste the address back by hand."; }

pkill -f "pebble login --no-open-browser" 2>/dev/null
pebble login --no-open-browser >"$log" 2>&1 </dev/null &
pid=$!
for _ in $(seq 1 40); do grep -q 'https://' "$log" && break; sleep 0.5; done
url="$(grep -o 'https://[^ ]*' "$log" | head -1)"
if [ -z "$url" ]; then echo "pebble-login: no link was printed. The sign-in service may be down."; cat "$log"; exit 1; fi
echo "Open this link and sign in (it lasts about ten minutes):"
echo "$url"
curl -s -m 10 -o /dev/null "http://localhost:60000/?broker_status=ok"
wait "$pid"
# The account's email stays out of the output.
if pebble login --status 2>/dev/null | grep -q 'logged in'; then echo "Signed in."; else
  echo "Not signed in: $(grep -v 'https://\|Open this URL' "$log" | sed 's/[A-Za-z0-9._%+-]*@[A-Za-z0-9.-]*/<email>/g' | tail -1)"; exit 1; fi
