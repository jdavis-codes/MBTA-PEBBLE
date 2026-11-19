---
alwaysApply: true
---

## Project Overview

This is a Pebble smartwatch application written in C using the Pebble SDK.

## Supported Platforms

The app targets multiple Pebble watch models:
- aplite (Pebble classic)
- basalt (Pebble Time)
- chalk (Pebble Time Round)
- diorite (Pebble 2)
- emery (Pebble Time 2)
- flint (Pebble 2 Duo)

## Commands

```bash
# Build the app for all platforms
pebble build

# Clean build artifacts
pebble clean

# Install the app on specific emulator
pebble install --emulator basalt

# Screenshot the running emulator
pebble screenshot --scale 6 --no-open screenshot.png
```

If you need more information on the `pebble` command or a sub-command, append `--help`.

### Headless Environments

If you're running in an environment without a window server (e.g., headless Linux, Docker, CI), you must add `--vnc` to **all commands that interact with the emulator**. This includes app installs, screenshots, button presses, and any `emu-*` commands:

```bash
pebble install --emulator basalt --vnc
pebble screenshot --vnc --scale 6 --no-open screenshot.png
pebble emu-button --emulator basalt --vnc click select
```

The `--vnc` flag enables a VNC-based display backend that doesn't require X11.

## Project Structure

```
src/c/           - C source files for the watchapp
src/pkjs/        - PebbleKitJS files (currently empty)
worker_src/c/    - Worker source files (optional, not present)
resources/       - Images, fonts, and other resources (not present)
```
## Configuration

By default, this project is initialized as a watchface. To make it an app, replace "watchface": true with "watchface": false in package.json.

## Architecture

The application follows the standard Pebble app architecture:

1. **Main Entry Point**: `src/c` - The `main()` function initializes the app and starts the event loop
2. **Window Management**: Single window app with text layer for displaying button press feedback
3. **Event Handling**: Button click handlers registered via `prv_click_config_provider` for UP, DOWN, and SELECT buttons

## SDK Documentation

The full Pebble SDK documentation is available at https://developer.repebble.com.

An index of every page is at https://developer.repebble.com/llms.txt. Use it to discover what's available. Every page also has a Markdown version: append `.md` to any documentation URL to fetch plain Markdown instead of HTML (e.g. `https://developer.repebble.com/guides/events-and-services/buttons.md`). Prefer the `.md` form when reading docs.

Main Categories:
- Tutorials - Step-by-step learning (C watchface tutorial in 5 parts, advanced topics)
- Developer Guides - Comprehensive reference organized by topic

Key Sections:
- App Resources - Images, fonts, vector graphics, 256 resource limit
- User Interfaces - Layer hierarchy, TextLayer, MenuLayer, round vs rectangular displays
- Events & Services - Buttons, accelerometer, compass, health data, background workers
- Communication - Bluetooth AppMessage, PebbleKit JS/Android/iOS integration
- Graphics & Animations - Drawing APIs, property animations, vector graphics
- Debugging - App logs, GDB, common errors and solutions
- Best Practices - Multi-platform support, battery conservation, modular architecture
- Design & Interaction - Glance-first design, one-click actions, platform guidelines
- App Store Publishing - Submission requirements, assets, analytics

Key Entry Points:
- https://developer.repebble.com/tutorials/watchface-tutorial/part1 - C development start
- https://developer.repebble.com/guides/events-and-services/buttons - Button handling
- https://developer.repebble.com/guides/user-interfaces/layers - UI foundations

## Versioning

The `version` in `package.json` is what the Pebble phone app shows for the watchface, so it identifies the build on the watch.

- Format is `major.minor.patch`, digits only (no `-beta` style suffixes; the build fails). Major and minor must each be 0-255. Patch can be any whole number, but only major and minor are stored in the watch binary, so do not rely on patch on the watch side.
- `main` is `0.0.x`. Each new branch takes the next minor and starts at patch 0. The owner may also call for the next minor for a large change on the same branch (`claude/pebble-dev-tools-setup-ghhvea` went `0.1.x`, `0.2.x` for Bluebikes, `0.3.x` for the layouts).
- Raise the patch (`make bump`) for every build that is installed on the watch for the user to look at, before the install, so each install shows a new number. Say the version in the message that asks the user to open the app.
- After the user confirms a version, tag its commit `v<version>` and push the tag with the branch.

## Documentation Voice

Write the README, the watch guides (`docs/watch-guide.html`, rendered to `docs/watch-guide.png`, `docs/guide-board.png` and `docs/guide-dial.png`; and `docs/guide-bikes.html`, rendered to `docs/guide-bikes.png`) and other user-facing docs the way the owner writes them. The callout text in the watch guide is the reference:

- Explain what the reader sees and what it means for them, not how the code does it: "A hollow square means almost full: one free parking space."
- Name things by what they are on the watch: "train timetable header", "data connection", "end of service", "orbiting dots". Avoid internal names (row label, fetch, flag).
- Give the reason when there is one: "Subway trains depart frequently, so they are shown as minutes to departure."
- Say where a behaviour is controlled: "If enabled in the watchface settings, ...".
- Call out what an example shows: "This example shows commuter rail rows...".
- Full, plain sentences. State lists as `Solid circle: data is up to date.`
- Spell things out rather than give one example (list every destination letter for every line).
- American spelling ("canceled").
- Check every number and behaviour against the code before writing it; do not leave a placeholder.

## Development Best Practices

- Before every commit, install the build on the user's real watch: start `make watch` in the background (it waits up to 5 minutes for the phone), tell the user in the same turn to open the Pebble app on their phone, and commit only after `App install succeeded.` Do not ask whether the app is open and wait for a reply. See the `pebble-vm-dev` skill.
- Do not `git push` a change until it has been installed on the watch and the user has confirmed it looks right there. Commit locally after the install succeeds, then wait for the user's confirmation before pushing. If they ask for adjustments, make them, reinstall, and wait again.
- Changes that do not affect the build (documentation, `CLAUDE.md`, skills, the Makefile) have nothing to install or confirm, so commit and push them directly.

- Whenever making changes, run `pebble screenshot --scale 6` and view the screenshot to make sure it's what the user requested. If not, make more changes until it does what it's supposed to.

## Emulator Button Control

Control emulator buttons programmatically with `pebble emu-button`:

```bash
# Click a button (press and release)
pebble emu-button click select

# Long press (e.g., 2 seconds to exit app)
pebble emu-button click back --duration 2000

# Repeat clicks (e.g., scroll down 5 times)
pebble emu-button click down --repeat 5

# Faster repeat interval
pebble emu-button click up --repeat 3 --interval 100
```

**Actions:**
- `click` - Press then release (use `--duration` for long press)
- `push` - Hold button down (use `release` to let go)
- `release` - Release all buttons

**Buttons:** `back`, `up`, `select`, `down`

**Best Practices:**
- Use `click` for normal navigation and selection
- Use `click --duration 2000` for long press (e.g., back button to exit)
- Use `--repeat` to scroll through menus instead of multiple commands
- After making UI changes, take a screenshot to verify the result

## AI Interaction Guidelines

- When given an image of a watchface to replicate, describe the target watchface in precise detail. Note every visual element present, as well as size, alignment, font weight, spacing, and location.

## AI Code Review Guidelines

- Once you think you've fulfilled the user's request, ask yourself if you see any issues with the current screenshot, and if there are any differences between the screenshot and the reference image or the user's description. If so, fix them.