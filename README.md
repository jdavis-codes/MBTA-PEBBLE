# watchface

A Pebble watchface (diorite / flint) that shows the time plus live MBTA
departures, fetched from the [MBTA v3 API](https://api-v3.mbta.com) by
PebbleKit JS (`src/pkjs/index.js`). It shows three rows, each one any
subway, light rail or commuter rail line + station + direction. The defaults:

- **North Station → Outbound** (Newburyport/Rockport Line)
- **Salem → Inbound** to North Station
- **Central → Southbound** Red Line

Commuter rail rows show departure times. Subway rows show minutes until each
train, tagged with the first letter of its destination when trains in that
direction go to different places (e.g. `A` = Ashmont, `B` = Braintree).

The Green Line is offered as a single line covering the B, C, D and E branches.
Its stations are grouped into the shared central stops and each branch's own
stops, and departures are tagged with their branch letter (e.g. `12B 15D`)
wherever more than one branch stops.

### Bluebikes docks

Up to five [Bluebikes](https://bluebikes.com/system-data) docks can be shown as
small circles along the top left, in the order they are set. Each circle fills
like a pie with the share of the dock's spaces that hold a rentable bike (an
empty ring means no bikes), and a small dot beside it means an e-bike is
available. Dock status comes from the Bluebikes GBFS feed and refreshes every
minute; the feed has no per-station query, so each refresh downloads the whole
system's status (about 235 KB).

### Settings

Open the settings page (via [Clay](https://github.com/pebble-dev/clay), defined
in `src/pkjs/config.js`) from the Pebble app, or in the emulator with
`pebble emu-app-config --emulator diorite`.

- **Departures shown per row** (1–5, default 3). Each row's departures stay on
  one line; with 4 or 5 the text shrinks to fit and the clock gets the
  freed-up space.
- **Line → Station → Direction** per row. The page loads these lists live from
  the MBTA API (`src/pkjs/config-page.js`): picking a line fills in its
  stations and directions. If the MBTA can't be reached, the saved choices are
  still shown.
- **Label on watch** per row, filled in automatically (e.g. `SALEM > IN`) when
  you change the station or direction, and editable. If it doesn't fit next to
  the countdown, the watch shortens the station part.
- **Show service alerts** per row (off by default): when the MBTA has an alert
  in effect for that line, station and direction, its text scrolls across the
  row's label once a minute and `!` is shown before the label in between.
  Minor notices (severity below 3) are left out. If the watch ever resets
  during a scroll, alerts stay off until the settings are saved again.
- **Bluebikes docks**: five slots, each with a search box. Type part of a
  dock's name and pick it from the list beneath.
- **Departure warning** per row: while a departure is inside its range it
  blinks (inverted), blinking faster as the end of the range approaches — a
  cue that it's time to leave. Defaults: row 1 on, 30 → 25 min; row 2
  15 → 10 and row 3 10 → 5, both off.

Commuter rail times are scheduled times, replaced by live predictions when
the MBTA has them (cancelled trips are hidden). The watch requests fresh data
every 2 minutes; set `MBTA_API_KEY` in `src/pkjs/index.js` for a higher
rate limit.

## Building & running

`make` lists shortcuts for the commands below (`make emu`, `make shot`,
`make watch`, ...).

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
