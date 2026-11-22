---
name: pebble-vm-dev
description: Build, emulate, screenshot and install this Pebble watchface from a headless cloud VM or container (Claude Code on the web, CI, Docker). Use when the `pebble` command is missing, the emulator will not start, a Pebble account login is needed, or a build has to be installed on the user's real watch over the CloudPebble connection.
---

# Pebble development from a VM

The container is ephemeral: the SDK, the emulator state and the Pebble login are
gone in every new session. Start with setup, then use the sections you need.

## 1. Setup (every new container)

```sh
scripts/setup-pebble.sh          # pebble-tool via uv, latest SDK, IPv4 patch, npm install, build
export PATH="$HOME/.local/bin:$PATH" PYTHONWARNINGS=ignore
```

The script patches pypkjs (the emulator's phone simulator) to listen on
`0.0.0.0`. Without it, containers that have no IPv6 fail with
`OSError: [Errno 97] Address family not supported by protocol` and
`pebble install --emulator` reports `[Errno 111] Connection refused`.

## 2. Emulator

There is no display, so pass `--vnc` to every command that touches the emulator.

The `Makefile` wraps the common cases with the workarounds below built in:
`make emu`, `make shot SHOT=path.png`, `make kill`, `make wipe`, `make watch`
(run `make` for the full list; `PLATFORM=diorite` switches platform). The raw
commands are:

```sh
pebble build
pebble install --emulator flint --vnc >"$LOG" 2>&1 </dev/null \
  || pebble install --emulator flint --vnc >"$LOG" 2>&1 </dev/null
pebble screenshot --emulator flint --vnc --scale 6 --no-open "$OUT.png" >/dev/null 2>&1 </dev/null
```

Things that cost time if you do not know them:

- **Never pipe emulator commands** (`| tail`, `| grep`). The first one spawns
  QEMU, pypkjs and websockify, which inherit the pipe and keep it open, so the
  command never returns. Redirect to a file and read the file.
- **The first install after boot usually fails** with `Connection refused`: the
  tool connects before pypkjs is listening. Run it a second time, as above.
- Wait about 10 to 15 seconds after an install before the screenshot, so the
  phone-side JavaScript has fetched data.
- `pebble kill` stops the emulator; `pebble wipe` resets its stored data
  (persisted departures and settings). `pkill -f websockify` clears a stale
  VNC proxy.
- Target platforms are `diorite` and `flint` (see `package.json`). Both are
  144x168 black and white.

### A forced layout and a test position

The emulator has no location service and no saved settings, so the layouts that
need them cannot be reached by hand. `make emu-test` patches the phone side for
one run and restores it afterwards:

```sh
make emu-test LAYOUT=3 ROWS=8 SHOW=1   # nearby board, 8 departures, minutes only
make dump DUMP=/abs/path/screen.png    # the screen, straight from QEMU
```

`LAYOUT` is 0 to 3, `ROWS` 6 to 8, `SHOW` 0 (time and minutes), 1 (minutes), 2
(time). The position defaults to Park Street station and the data is live, so
leave the emulator clock alone. Wait about 25 seconds after it starts before
capturing.

### Testing the Bluetooth state

`pebble emu-bt-connection --emulator diorite --vnc --connected no` works on
diorite. Two limits:

- While disconnected, `pebble screenshot` fails with `Connected, but the watch
  didn't answer`. Capture the display through the QEMU monitor instead: take the
  port from `-monitor tcp::<port>` in the `qemu-pebble` command line
  (`pgrep -af qemu-pebble`), send `screendump /path/out.ppm` to it over TCP, and
  convert the PPM with Pillow.
- `--connected yes` cannot get through once disconnected. Restart with
  `pebble kill` and install again.

## 3. Pebble account login

`pebble login` expects a browser on the same machine. `make login`
(`scripts/pebble-login.sh`) works around that:

1. Run `make login` in the background with its output sent to a file. It
   prints an `appstore-api.repebble.com` authorize link, then waits up to ten
   minutes for the sign-in.
2. Fetch the link's headers once yourself. A `307` to `/dashboard/signin` means
   it is live. If it hangs, the sign-in service is down: tell the user, retry
   later, do not hand over a dead link.
3. Give the user the link. After signing in (Google, GitHub or Apple) their
   browser lands on `http://localhost:60000/?broker_status=ok` and shows a
   connection error. That is expected, and nothing has to be pasted back: the
   script has already sent that address to the waiting tool.
4. The command ends with `Signed in.` Do not print the account email.

A link lasts about ten minutes. One used later fails with `Flow not found or
expired`; run `make login` again for a new one.

The script was written in November 2026 and has not yet been through a real
sign-in. If it fails, the manual way still works within 30 seconds of the
sign-in: run `pebble login --no-open-browser`, hand over the link, and when
the user says they have signed in run
`curl "http://localhost:60000/?broker_status=ok"`.

## 4. Installing on the real watch

**Every commit is preceded by a watch install** (see `CLAUDE.md`): finish and
test the change in the emulator, install, then commit.

The install waits for the phone, and the user has to open the Pebble app for it
to connect (with *Dev Connection* on for the watch: Devices tab, three dot menu).
Do not ask whether the app is open and wait for an answer. Start the install in
the background, tell the user in the same turn to open the app, and carry on
when it finishes:

```sh
make watch-retry  # keeps trying for 30 minutes while the phone connects
make watch        # one attempt, waits up to 5 minutes
```

The connection often drops on the first attempt, which is why the retry exists.
If it runs out, say so, stop retrying, and start again when the user is back.

After a successful install, commit locally but **do not `git push` until the
user has confirmed the change on the watch**. Changes that do not affect the
build (docs, skills, the Makefile) skip the install and are pushed directly.

Success ends with `App install succeeded.` `pebble logs --cloudpebble` streams
logs from the watch and the phone-side JavaScript the same way.

## 5. Project facts worth knowing

- SDK 4.44 builds manifest version 2: PebbleKit JS only gets the network
  access listed under `usesPermissions` in `package.json`. A new host needs a
  new entry there; `pebble convert-manifest --dry-run` finds hosts written in
  the JavaScript. The emulator does not enforce this, a phone does.
- The user's phone is an iPhone. There PebbleKit JS has no `fetch`, no
  `XMLHttpRequest.timeout`, and timers stop while the Pebble app is suspended.
  Do not build anything that depends on a timer firing on time.
- Docs: every page on https://developer.repebble.com has a Markdown version
  (append `.md`); the index is `/llms.txt`.
- After a UI change, take a screenshot and look at it before reporting.

## 6. Writing the C

- **The emulator does not prove a build is safe on the watch.** Two scrolling
  alert headers (an `AppTimer` loop at 60ms, then a `PropertyAnimation` on a
  clipped `TextLayer`) ran clean on both emulator platforms and reset the real
  watch about 10 seconds into a scroll, twice needing an OS reinstall. The
  cause was never found. A third version works on the watch; it differs by
  leaving an idle gap after every frame, taking its position from the clock,
  drawing only whole characters that lie fully inside the label area (never a
  wide text box, never text partly off-screen), and caching the row layout.
  Keep those properties when touching the scroll.
  - Anything that animates, or changes how often the screen redraws, needs
    the user's explicit go-ahead before it is installed, with a plain
    statement that it is untested on hardware.
  - If the watch misbehaves after an install, stop installing, restore the
    last build the user confirmed, and tell them to keep the app off the
    watch until then: the phone reinstalls it, and saved settings can
    re-trigger the same fault.
- **Diagnostics are built in; use them.** The watch keeps a breadcrumb in
  persistent storage and reports how the previous run ended at the next
  launch (`prev clean|UNCLEAN stage=...`); the phone keeps the last 20 reports
  and replays them into the logs at launch and every 20 minutes. A heartbeat
  line each minute carries uptime, free heap and the slowest redraw. Capture
  with `make watch-capture` (reconnects when the phone drops, writes
  `build/watch-logs.txt`), and stop the capture before running `make watch`.
  A run that dies mid-scroll leaves alerts blocked until the settings are
  saved again. Add a breadcrumb stage for any new risky feature.
- **Look for the documented way first.** Before hand-rolling UI behaviour,
  check the guides (`/llms.txt`) for a system facility.
- **The app stack is about 2KB.** Text drawing uses much of it, so keep
  buffers and arrays out of draw-path locals (make them `static`). In the
  emulator an overflow shows up as `App fault! ... PC: 0 LR: 0` in the logs,
  the phone link dropping (hollow connection dot, `the watch didn't answer`),
  and eventually an "is not responding" screen. A formatted `APP_LOG` inside
  the draw path is enough to tip it over.
- **Finding a fault:** `pebble install --emulator flint --vnc --logs` streams
  logs from launch. Capture frames with the QEMU monitor `screendump`
  (section 2) when the phone link is down.
- **Do not start a watch install while the user may be testing.** An install
  closes the running watchface.
