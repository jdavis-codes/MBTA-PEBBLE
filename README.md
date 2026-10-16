# watchface

A Pebble watchface (diorite / flint) that shows the time plus live MBTA
departures, fetched from the [MBTA v3 API](https://api-v3.mbta.com) by
PebbleKit JS (`src/pkjs/index.js`):

- **North Station → Outbound** (Newburyport/Rockport Line): next 3 departures
- **Salem → Inbound** to North Station: next 3 departures
- **Central → Southbound** Red Line: minutes until next 3 trains
  (`A` = Ashmont, `B` = Braintree)

### Settings

Open the settings page (via [Clay](https://github.com/pebble-dev/clay), defined
in `src/pkjs/config.js`) from the Pebble app, or in the emulator with
`pebble emu-app-config --emulator diorite`.

- **Departures shown per station** (1–5, default 3). Each station's departures
  stay on one line; with 4 or 5 the text shrinks to fit and the clock gets
  the freed-up space.
- **Departure warning** per station: while a departure is inside its range it
  blinks (inverted), blinking faster as the end of the range approaches — a
  cue that it's time to leave. Defaults: North Station on, 30 → 25 min;
  Salem 15 → 10 and Central 10 → 5, both off.

Commuter rail times are scheduled times, replaced by live predictions when
the MBTA has them (cancelled trips are hidden). The watch requests fresh data
every 2 minutes; set `MBTA_API_KEY` in `src/pkjs/index.js` for a higher
rate limit.

## Building & running

```sh
pebble build                          # build for all targetPlatforms
pebble install --emulator emery       # install on the emery emulator
pebble install --phone <ip>           # install to a paired phone
```

## Target platforms

`targetPlatforms` in `package.json` controls which watches you build for. The
modern Pebble hardware is **emery** (Pebble Time 2), **gabbro** (Pebble Round
2), and **flint** (Pebble 2 Duo); the original Pebble platforms (aplite,
basalt, chalk, diorite) are included by default for backwards compatibility.

## Project layout

```
src/c/           C source for the watchapp
src/pkjs/        PebbleKit JS (phone-side) source, if any
worker_src/c/    Background worker source, if any
resources/       Images, fonts, and other bundled resources
package.json     Project metadata (UUID, platforms, resources, message keys)
wscript          Build rules — usually no need to edit
```

By default this project is configured as a watchapp. To make it a watchface,
set `pebble.watchapp.watchface` to `true` in `package.json`.

## Documentation

Full SDK docs, tutorials, and API reference: <https://developer.repebble.com>
