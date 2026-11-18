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

`pebble login` expects a browser on the same machine. From a VM:

1. Run `pebble login --no-open-browser` in the background with output sent to
   a file and `PYTHONUNBUFFERED=1`. It prints an `appstore-api.repebble.com`
   authorize URL and then waits on `localhost:60000`.
2. Fetch the URL's headers once yourself. A `307` to `/dashboard/signin` means
   it is live. If the start request or the URL hangs, the sign-in service is
   down: tell the user, retry later, do not hand over a dead link.
3. Give the user the URL. After signing in (Google, GitHub or Apple) their
   browser lands on `http://localhost:60000/?...` and shows a connection error.
   That is expected. Ask them to paste that full address back.
4. Replay it inside the container: `curl "http://localhost:60000/?<their query>"`.
   The waiting login then completes.
5. Confirm with `pebble login --status`. Do not print the account email.

## 4. Installing on the real watch

**Every commit is preceded by a watch install** (see `CLAUDE.md`): finish and
test the change in the emulator, install, then commit.

The install waits for the phone, and the user has to open the Pebble app for it
to connect (with *Dev Connection* on for the watch: Devices tab, three dot menu).
Do not ask whether the app is open and wait for an answer. Start the install in
the background, tell the user in the same turn to open the app, and carry on
when it finishes:

```sh
make watch        # waits up to 5 minutes for the phone
```

If it times out, say so and start it again when the user is ready.

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

- **The emulator does not prove a build is safe on the watch.** A scrolling
  service-alert header (first an `AppTimer` loop at 60ms, then a
  `PropertyAnimation` on a clipped `TextLayer`) ran clean on both emulator
  platforms and took down the real watch twice, about 10 seconds into a
  scroll, badly enough to need an OS reinstall. The cause was never
  confirmed; the leading guess is that many seconds of back-to-back
  full-screen redraws starve the system on real hardware. The feature was
  removed. Until that is understood:
  - Do not add any animation that redraws continuously for more than a
    second or two. The departure blink (one redraw per 125ms or slower) and
    the fetch pulse (150ms, normally a few seconds) are the only animations
    known to be fine on hardware.
  - Treat anything that animates, or that changes how often the screen
    redraws, as needing the user's explicit go-ahead before it is installed
    on the watch, and say plainly that it is untested on hardware.
  - If the watch misbehaves after an install, stop installing, restore the
    last build the user confirmed, and tell them to keep the app off the
    watch until then: the phone reinstalls it, and saved settings can
    re-trigger the same fault.
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
