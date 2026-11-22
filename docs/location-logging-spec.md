# Location logging toolkit: spec

Status: step 1 of "Order of work" is built in version 0.5.0 (`src/pkjs/loclog.js`),
with the trace export from step 3. The test plan is in
`docs/location-test-plan.md`. Written at version 0.4.14.

Where 0.5.0 differs from the text below:

- **The privacy rules were relaxed by the owner** (November 2026): this is a
  development tool that only ever holds the owner's own data, and it has to be
  easy to analyze. Records hold the real latitude, longitude and time. The
  rule that survives is that nothing from the log goes into `pebble logs`,
  the repo, tests or docs.
- **The log can upload itself.** With a GitHub repository and a token entered
  in the settings, new records are added to that repository as small CSV files
  under `loclog/`, about every ten minutes and whenever the settings are saved.
  Use a private repository. Copying from the settings page still works.
- The log is CSV, one row per request and one per marker, with the columns
  `t,time,took_ms,result,lat,lon,acc,alt,age,speed,heading,stops,refetch,stop,stop_dist,session,mark`.
  `scripts/loclog.py` loads it into pandas.
- While the log is on, the marker buttons are at the top of the settings page,
  because they are pressed on a platform.
- The trace is shown in the settings page once: choose how much under "Show
  the trace", save, and open the settings again. The page carries the trace in
  its address, and how long an address the Pebble iPhone app accepts is not
  known.
- `stop` is the MBTA's stop number, not its name. While the log is on, the
  stops around the phone are looked up on every layout, not only the nearby
  board, so `stop`, `stop_dist` and `stops` are always filled. They describe
  the last lookup, which is repeated once the phone has moved 150 meters.
- A marker describes where the owner is for at most two hours ("Back at street
  level" for 20 minutes), so a forgotten marker does not count a day at a desk.
- While the log is on, the phone is asked for its position every time the
  watch asks for anything, on every layout. On the nearby board that is once a
  minute, as before.

## Why we are doing this

The "Departure board: nearby" layout lists every train and bus leaving from
stops within a set distance of the phone. Today it is simple: the watch asks
the phone for its position once a minute, and the departures are fetched again
when the phone has moved more than 150 meters.

We want it to be quicker and smarter when the owner is moving from train to
train. Five options are on the table:

1. **Adaptive polling.** Ask often while moving, rarely while still.
2. **Look ahead.** List stops around where the phone will be in about 90
   seconds, from its speed and direction, instead of where it is.
3. **"Can I make it" times.** Hide departures that leave before the owner could
   reach the stop, and show minutes to spare.
4. **Riding mode.** When moving at vehicle speed, show the station being
   approached and its connections.
5. **Station lock.** Within about 50 meters of a station, show only that station.

Every one of these depends on things we have not measured. We know one thing:
on the owner's iPhone, above ground, with the phone locked in a pocket, the
phone answered every request within about 5 meters in 0.1 seconds. We do not
know:

- how accurate positions are inside stations and in tunnels;
- whether the phone reports speed and heading, or we must work them out;
- how often a request goes unanswered, and where;
- what asking more often than once a minute costs in battery, on the watch and
  on the phone;
- how long it takes for the board to show the right station after arriving.

Other teams' experience says positions underground are poor: the Transit app
locates underground trains from motion sensors and the timetable, not from
position fixes. The MBTA has cell coverage in all underground stations and
tunnels, which is probably why positions in stations seem good, but "good"
could mean 20 meters or 150, and that decides which options are worth building.

The toolkit exists to replace those guesses with numbers from the owner's real
commute, and to let us try the options against recorded trips before putting
any of them on the watch.

## What it must answer

| # | Question | Decides |
|---|---|---|
| Q1 | How accurate are positions above ground, in stations, and in tunnels? | Whether options 4 and 5 are possible; the smallest useful radius |
| Q2 | Does the phone report speed and heading? How do they compare with values worked out from two positions? | How options 2 and 3 get their inputs |
| Q3 | How often is a request unanswered or failed, and how long are the gaps? | How much the board can depend on a fresh position |
| Q4 | What does each polling rate (60, 30, 15 seconds) cost in watch battery per hour? | The rates option 1 may use |
| Q5 | After arriving at a station, how long until the board lists it? | Whether look ahead (option 2) is worth its complexity |
| Q6 | Replayed against recorded trips, which option would have shown the right departures soonest, and how often would each have been wrong? | Which options to build |

## Privacy rules

These are requirements, not preferences.

- The log stays on the phone. Nothing is sent anywhere by the watchface.
- The log never holds a latitude or longitude. Positions are stored as meters
  east and north of a random origin chosen when logging starts. The origin
  itself is not stored.
- Nothing from the log goes into `pebble logs`, the repo, tests or docs.
- The owner exports the log by hand from the settings page and decides who
  sees it. There are two exports (see "Exports"): a summary that is safe to
  paste anywhere, and a trace that reveals which stations were visited.
- Logging is off by default and turns itself off after seven days.

## Parts

### 1. Recorder (phone, `src/pkjs/`)

Replaces the current "Location test". One record is written for each location
request the phone makes, when the request is answered, fails, or is given up
on.

Record fields:

| Field | Meaning |
|---|---|
| `t` | When the request was made, seconds since logging started |
| `took` | Milliseconds until the answer, or until it was given up on |
| `result` | `ok`, `failed`, `denied`, `timeout`, `no answer` |
| `e`, `n` | Meters east and north of the random origin (answered requests only) |
| `acc` | Reported accuracy in meters |
| `age` | Seconds between the phone taking the position and handing it over, when reported |
| `speed`, `heading` | As reported by the phone; empty when it reports none |
| `stop`, `stopDist` | Nearest MBTA stop and its distance, when the nearby stops are known (trace export only) |
| `stops` | How many stops were inside the radius |
| `refetch` | Whether this position caused the departures to be fetched again |
| `mark` | A marker the owner set (see "Markers") |

Limits:

- The log is a ring: at most 3,000 records (about two days at one a minute,
  twelve hours at one every 15 seconds). The oldest are dropped first.
- Each record is stored compactly; the whole log must stay under 200 KB of
  `localStorage`. How much the Pebble iPhone app allows is unknown and must be
  checked before relying on it (see "Open questions").
- Writing the log must never delay or block a refresh. A failure to write is
  ignored.

### 2. Polling rate control (watch and phone)

A setting, shown only for the nearby layout while logging is on: "Ask for the
position every" 60, 30 or 15 seconds.

- The watch has to start every request, because the phone side cannot wake
  itself in the background. At 30 and 15 seconds the watch sends the small
  position request (`REQUEST_POSITION`) from a timer between the minute ticks.
- This changes how often the watch wakes and sends. It is untested on hardware
  and needs the owner's explicit go-ahead before it is installed.
- The location bar keeps its four slashes; each lasts a quarter of the chosen
  interval.

### 3. Watch battery log (watch)

To answer Q4, the watch records its battery level once an hour, with the
layout and polling rate in use, and reports the readings to the phone with the
next request. The phone keeps the last 72 readings beside the location log.

- Battery percent on the Pebble moves in coarse steps, so a rate needs at
  least a full day to give a usable number.
- The phone's battery cannot be read by the watchface. The owner compares it
  by eye, as before.

### 4. Markers

The numbers need ground truth: where the owner really was. Two ways to mark:

- **Settings page:** buttons for "Entered station", "On platform", "Boarded",
  "Got off", "Back at street level". Each writes a marker with the time.
- **Notes afterwards:** the owner writes down times ("boarded 8:14") and they
  are matched to the log by time during analysis.

A wrist-shake marker on the watch was considered and left out: it costs
battery and fires by accident.

### 5. Exports (settings page)

A "Location log" section at the bottom of the settings, replacing "Location
test", with:

- the on/off switch and how many records are held;
- **Summary:** counts and spreads only, no positions and no stop names. Safe
  to paste into a chat or an issue. Contents:
  - requests made, answered, failed, unanswered;
  - accuracy: median, 90th percentile, worst;
  - time to answer: median, 90th percentile;
  - longest gap between answers;
  - share of answers that reported speed, and heading;
  - the same figures again for each marker state (street, station, on a
    train), when markers exist;
  - watch battery percent lost per hour at each polling rate;
- **Trace:** the full records as text the owner can copy. It contains
  relative positions and nearest-stop names, so it shows which stations were
  visited. The page says so above the copy button.

### 6. Report and replay (`scripts/`, run on a computer)

`scripts/location-report.py TRACE.txt` reads an exported trace and prints the
answers to Q1 to Q5 as tables.

`scripts/location-replay.py TRACE.txt` answers Q6. It feeds the recorded
positions, in order, to each candidate strategy and reports what the board
would have shown:

- **Baseline:** today's behavior (fetch again after moving 150 meters).
- **Look ahead:** stops around the position projected 60, 90 and 120 seconds
  on.
- **Station lock:** within 30, 50 and 80 meters of a stop.
- **Adaptive polling:** which requests it would have skipped, and what that
  would have cost in lateness.

For each strategy it reports how long after each "Entered station" marker the
right station was first listed, how often a station was listed that the owner
never went to, and how many requests it made. "Can I make it" (option 3) is
checked the same way: for each departure the owner did or did not catch,
whether the board would have said so.

Both scripts work on trace files that are never committed. `tests/` gets a
small made-up trace (around example stations such as Park Street) so the
scripts themselves are tested.

## Order of work

1. Recorder, summary export and markers, at the current one-minute rate. No
   change to what the watch does, so nothing new to prove on hardware.
2. The owner runs it for two or three commuting days and shares the summary.
   That answers Q1, Q2, Q3 and Q5 and tells us whether options 4 and 5 are
   alive.
3. Report and replay scripts, and the trace export. Answers Q6 for options 1,
   2, 3 and 5.
4. Polling rate control and the battery log, only if steps 2 and 3 show that a
   faster rate would help. Answers Q4.
5. Build the options the numbers support. Remove the toolkit, or leave only
   the summary, before the layout is called finished.

## Done when

- Each of Q1 to Q6 has a number from at least two commuting days, or a stated
  reason it could not be measured.
- For each of the five options there is a written decision: build, drop, or
  hold, with the figure that decided it.

## Not part of this

- Using the phone's motion sensors. The watchface cannot reach them.
- Using the watch's accelerometer to detect riding. It may be worth its own
  investigation if option 4 survives.
- Any change to the other three layouts.
- Sending positions anywhere other than the MBTA stop lookup that the nearby
  board already makes.

## Open questions

- How much `localStorage` does the Pebble iPhone app give a watchface? This
  sets the ring size and must be checked first.
- Does the Pebble iPhone app pass `speed`, `heading` and the position's own
  timestamp through at all? The location test showed "age unknown", which
  suggests the timestamp, at least, is missing.
- Can the settings page copy a long text to the clipboard, or does the trace
  have to be shown in a text box to select by hand?
- Is one request every 15 seconds honored in the background, or does the
  iPhone start dropping them?
