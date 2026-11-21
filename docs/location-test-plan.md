# Location log: one week test plan

For version 0.5.0. The plan covers five commuting days: a commuter rail trip
in the morning and a Red Line trip in the evening. It collects the numbers that
`docs/location-logging-spec.md` asks for (questions Q1 to Q6 there).

No station is named here, and none should be added. The summary you copy out
each day holds no positions and no stop names. The trace does show which
stations you visited: keep it in a private note.

## What this week can and cannot answer

| Question | This week |
|---|---|
| Q1 Accuracy above ground, in stations, in tunnels | Yes, from the marker days (1, 3 and 5). |
| Q2 Does the phone report speed and heading? | Yes, every day. |
| Q3 How often is a request unanswered, and for how long? | Yes. Days 2 and 4 are the honest figure, because the phone is never touched. |
| Q4 Watch battery cost of asking more often | No. The watch still asks once a minute. The battery notes below give the baseline to compare with later. |
| Q5 How long until the board lists the station you arrived at | Partly: by eye on day 5, and later from the traces. |
| Q6 Which option would have shown the right departures soonest | Not yet. It needs the replay script, which is written against the traces this week produces. |

## Why the days differ

Pressing a marker means opening the Pebble app, and an open app may get better
and quicker positions than one in the background. So the days alternate:

- **Marker days (1, 3, 5):** you press the buttons. We learn exactly where you
  were for each record, at the cost of the phone being awake at those moments.
- **Pocket days (2, 4):** the phone stays locked in your pocket for the whole
  trip. We learn how the phone behaves when left alone, which is how the
  watchface is really used.

If the two kinds of day give the same figures, markers are harmless and the
marker days can be trusted as they are. If they differ, that is a finding too.

## Before the first day (10 minutes, at home)

1. Install version 0.5.0 and open the watchface settings in the Pebble app.
2. Set **Layout** to "Departure board: nearby" and leave **Nearby radius** at
   400 meters. Keep both the same all week so the days can be compared.
3. At the bottom, under **Location log**, turn on "Record location requests"
   and save. The log now runs for seven days and then turns itself off.
   Turning it off and on again starts a new log and removes the old one, so
   leave the switch alone until the week is over.
4. Wait five minutes and open the settings again. "Mark where you are" is now
   at the top. Press "Back at street level", then Save.
5. Open the settings again and check, under **Location log**:
   - the number of records held has gone up by about one a minute;
   - the summary box has text, and **Copy** puts it on the clipboard (paste it
     into a note to see). If the button says it could not copy, select the
     text in the box by hand.
6. Set **Show the trace** to "Last 3 hours", save, and open the settings once
   more. A trace box appears at the bottom, once. Copy it into a private note.

Tell me how steps 5 and 6 went. They answer three of the spec's open
questions (how much the phone will store, whether copying works, whether the
page opens with a trace in it) before any commuting day depends on them.

## Every day

Morning, before leaving:

- Note the watch battery percent and the phone battery percent, with the time.
- Leave the Pebble app running in the background, as you normally would.

Evening, after the trip home:

1. Note the two battery percents again, with the time.
2. Open the settings, go to **Location log**, copy the **summary** into a note
   headed with the day number. This is the part to send me.
3. Set **Show the trace** to "Last 24 hours", save, open the settings again,
   and copy the **trace** into a private note headed the same way. If the page
   does not open with the trace in it, open the settings again (the trace is
   only put in the page once) and use "Last 3 hours" instead, once after each
   trip.
4. Write one line about anything odd: the board saying NO FIX, departures for
   a station you had already left, the watch losing the phone.

The log holds about two days of records. Missing one evening loses nothing;
missing two in a row loses the oldest trip.

## The markers

| Button | Press it when |
|---|---|
| Entered station | You pass the doors or the fare gates. |
| On platform | You stop walking and wait for the train. |
| Boarded | The train doors close behind you. |
| Got off | You step onto the platform at the other end. |
| Back at street level | You are outside under open sky. |

Morning, commuter rail: most of its stations are a platform in the open air.
Press "On platform" when you reach it (skip "Entered station" if there is no
building), "Boarded", "Got off" at the terminal, and "Back at street level"
when you leave the building. If you go straight down to the subway instead,
press "Entered station" at the fare gates.

Evening, Red Line: all five, in order.

You can press several buttons before saving; each keeps the time of its own
press. A marker is only kept once you press Save, so save before the phone
goes back in your pocket. If you miss one, do not press it late: write the
time in your note instead.

## Day by day

**Day 1: marker day.** Both trips with markers, phone otherwise in your
pocket. The aim is to get the routine right. In the evening do the full export
and send me the summary, so a problem shows up after one day and not after
five.

**Day 2: pocket day.** Do not open the Pebble app from leaving home until you
are at your desk, or from leaving work until you are home. Look at the watch
as much as you like. Afterwards, write the times from memory to the nearest
minute: on the platform, boarded, got off, outside.

**Day 3: marker day.** As day 1. With day 1 it gives the two days of marked
figures the spec asks for.

**Day 4: pocket day.** As day 2.

**Day 5: marker day, watching the board.** As day 1, and at each arrival (the
morning terminal, your Red Line station in the evening, and the station where
you get off) look at the watch and note two times: when you stepped onto the
platform or through the gates, and when the board first listed departures
from that station. The difference is what "look ahead" would have to beat.

**Days 6 and 7:** nothing to do. Leave the log on; it turns itself off seven
days after you switched it on and keeps what it holds. Do one last export
after it has stopped.

## Sending the results

Send the five summaries, the battery notes and the odd-things lines. Keep the
traces; I will ask for them when the report and replay scripts exist, and you
decide then whether to share them or run the scripts yourself.

From the summaries we decide, per the spec:

- station lock and riding mode stay alive only if accuracy in stations and on
  trains is good enough (tens of meters, not hundreds);
- look ahead needs speed and heading, reported or worked out;
- how much the board may depend on a fresh position follows from the longest
  gaps on the pocket days.

## A short week

The week of November 23, 2026 has Thanksgiving on Thursday. If you commute
only Monday to Wednesday, run days 1, 2 and 3 and skip the rest: the spec
needs two or three commuting days, and day 5's board watching can be done on
any later trip while the log is still on. For the full five days, switch the
log on the morning of Monday, November 30 instead.
