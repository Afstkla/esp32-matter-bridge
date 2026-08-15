# Genie firmware — agent instructions

ESP32-S3 Matter bridge on a Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2). The
device ("Genie") is LIVE in Apple Home — read docs/plans/idf-port.md Global
Constraints before touching hardware or committing.

## Knowledge base — read first, write back once

Hard-won hardware/stack lore lives in exactly one place per topic. Before
debugging ANY peripheral, read its file. When you learn something new about
a peripheral (a register quirk, a timing fact, an errata, a disproven
theory), record it in that file IN THE SAME task — a finding that only
lives in a report or chat is a finding the next agent pays for again.

- docs/hardware/<peripheral>.md — one file per chip/subsystem (index in
  docs/hardware/README.md)
- docs/display-notes.md — display DMA/flush pipeline + underflow semantics
- docs/matter-notes.md — Matter serve-path mechanisms (three of them; the
  store is NOT the truth for registry-served clusters)
- components/esp_lcd/PATCH.md — why the vendored esp_lcd override exists
- ../NOTES.md (private, outside the repo, never committed) — credentials,
  MACs, IPs, board history; the private counterpart to docs/hardware/

Disproven theories are documented WITH their refutation — this repo's
history is littered with plausible-but-wrong stories that cost days.

## Privacy — before every commit

Repo files must never contain the Wi-Fi SSID, any LAN IP, device MACs, or
the Matter pairing code. Run the privacy grep from docs/plans/idf-port.md
Global Constraints (docs/plans/ is gitignored and holds the patterns).

## Conventions

- Commits: git -c user.name=Afstkla -c user.email=9075380+Afstkla@users.noreply.github.com
- Single-task panel ownership: only the ui task touches panel/rail/expander;
  console/Matter effects route through ui-tick intent flags (app_main.cpp).
- Sensor values reach Matter ONLY via bridgeUpdateValue() (bridge.h).
- esp_pm usb lock: single writer (ui task) via intent flags.
