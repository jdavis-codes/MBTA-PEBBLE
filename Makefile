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

.PHONY: help setup build clean emu shot config logs kill wipe login status watch watch-logs

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

watch: build ## Install on the real watch (open the Pebble app on your phone first)
	@timeout 90 pebble install --cloudpebble $(QUIET) || { tail -5 $(LOG); exit 1; }
	@tail -1 $(LOG)

watch-logs: ## Stream logs from the real watch and phone (Ctrl-C to stop)
	pebble logs --cloudpebble
