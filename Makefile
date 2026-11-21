# Shortcuts for common Pebble commands. Run `make` for the list.
#   make emu PLATFORM=diorite     pick the emulator platform (default flint)
SHELL := /bin/bash
export PATH := $(HOME)/.local/bin:$(PATH)
export PYTHONWARNINGS := ignore

PLATFORM ?= flint
SHOT ?= screenshot.png
# No window server (cloud VM, CI): the emulator needs --vnc.
VNC := $(if $(DISPLAY),,--vnc)
EMU := --emulator $(PLATFORM) $(VNC)
LOG := build/make.log
# Emulator commands start daemons that keep stdout open, so their output goes
# to a file instead of the terminal.
QUIET := >$(LOG) 2>&1 </dev/null

.PHONY: help setup build clean emu emu-test dump test shot config logs kill wipe login status watch watch-retry watch-logs watch-capture bump version

help: ## List the shortcuts
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F ':.*## ' '{printf "  make %-12s %s\n", $$1, $$2}'

setup: ## Install the Pebble tool and SDK in a fresh container
	scripts/setup-pebble.sh

build: ## Build the watchface
	pebble build

clean: ## Remove build output
	pebble clean

emu: build ## Build and run in the emulator (PLATFORM=flint|diorite)
	@pebble install $(EMU) $(QUIET) || pebble install $(EMU) $(QUIET) || { tail -5 $(LOG); exit 1; }
	@echo "Running on the $(PLATFORM) emulator."

# A public landmark (Park Street station), so no real position is ever needed.
LAYOUT ?= 3
ROWS ?= 6
SHOW ?= 0
LAT ?= 42.35639
LON ?= -71.06242
emu-test: ## Emulator with a forced layout and a test position (LAYOUT=3 ROWS=6 SHOW=0 LAT= LON=)
	@scripts/emu-test.py apply $(LAYOUT) $(ROWS) $(SHOW) $(LAT) $(LON)
	@$(MAKE) --no-print-directory emu; status=$$?; scripts/emu-test.py restore; exit $$status

DUMP ?= build/screen.png
dump: ## Save the emulator screen straight from QEMU to DUMP=build/screen.png
	@scripts/dump.py $(DUMP)

test: ## Run the phone-side tests (TESTS=all also drives the settings page in Chromium)
	@for t in tests/*.test.js; do node $$t >/dev/null || { echo "FAILED $$t"; exit 1; }; echo "ok $$t"; done
	@if [ "$(TESTS)" = all ]; then node tests/settings-page.js; fi

shot: ## Screenshot the emulator to SHOT=screenshot.png
	@pebble screenshot $(EMU) --scale 6 --no-open $(SHOT) $(QUIET) || { tail -5 $(LOG); exit 1; }
	@echo "Saved $(SHOT)"

config: ## Open the settings page for the emulator
	pebble emu-app-config $(EMU)

logs: ## Stream logs from the emulator (Ctrl-C to stop)
	pebble logs $(EMU)

kill: ## Stop the emulator
	-@pebble kill >/dev/null 2>&1; pkill -f "[w]ebsockify"; true

wipe: kill ## Stop the emulator and reset its stored data
	@pebble wipe >/dev/null 2>&1; echo "Emulator data wiped."

login: ## Sign in to your Pebble account (prints a link)
	pebble login --no-open-browser

status: ## Show whether the Pebble account is signed in
	@pebble login --status

watch: build ## Install on the real watch (waits 5 min for the Pebble app on your phone)
	@timeout 300 pebble install --cloudpebble $(QUIET) || { tail -5 $(LOG); exit 1; }
	@tail -1 $(LOG)

watch-retry: build ## Like watch, but keeps trying for 30 minutes while the phone connects
	@for i in 1 2 3 4 5 6; do \
	  timeout 300 pebble install --cloudpebble $(QUIET); \
	  grep -q "App install succeeded" $(LOG) && break; \
	done; tail -1 $(LOG); grep -q "App install succeeded" $(LOG)

watch-logs: ## Stream logs from the real watch and phone (Ctrl-C to stop)
	pebble logs --cloudpebble

bump: ## Raise the patch version in package.json (0.1.3 -> 0.1.4)
	@node -e 'const fs=require("fs"),f="package.json",s=fs.readFileSync(f,"utf8");const m=/"version": "(\d+)\.(\d+)\.(\d+)"/.exec(s);if(!m)throw new Error("no version");const v=m[1]+"."+m[2]+"."+(+m[3]+1);fs.writeFileSync(f,s.replace(m[0],"\"version\": \""+v+"\""));console.log(m[1]+"."+m[2]+"."+m[3]+" -> "+v)'

version: ## Show the version in package.json
	@node -p 'require("./package.json").version'

watch-capture: ## Capture watch logs to build/watch-logs.txt, reconnecting (MINUTES=110)
	scripts/watch-logs.sh $(or $(MINUTES),110) build/watch-logs.txt
