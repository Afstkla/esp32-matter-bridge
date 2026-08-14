# Matter accessory builder

A Matter **bridge** on an ESP32-S3 with a round AMOLED touchscreen. It presents
itself to Apple Home (or any Matter controller) as a hub, and the accessories
behind it are configured on the device's own screen — no app, no recompile.

Pick a device type on the touchscreen, give it a name, and it appears in Home
under that name. Press the on-screen button and a HomeKit automation fires.

## Why a bridge

A plain multi-endpoint Matter device gets named by its controller after the
device *type* — "Programmable Switch 1", "Light 2" — and nothing the device
sends changes that. `NodeLabel` on the **Bridged Device Basic Information**
cluster is the only per-accessory name an ecosystem reads, and that cluster
only exists on endpoints under an **Aggregator**.

The same arrangement is what lets accessories be added at runtime. So naming
and "add another accessory" turn out to be one feature, not two.

## Hardware

- **Waveshare ESP32-S3-Touch-AMOLED-1.8** — this code targets the **V2** board
  (CO5300 display controller, CST816 touch at I²C `0x15`). The V1 board uses
  different parts and will not work unmodified.
- 368×448 AMOLED over QSPI, capacitive touch, AXP2101 PMU, 8 MB PSRAM.

Nothing else is required. Buttons use the two keys already on the board:
**BOOT** (top) steps the active level up, **PWR** (bottom) steps it down.

## Quick start

```sh
pio run -e matter-amoled-1-8 -t upload
```

A fresh board has no network and does not need one. It advertises over BLE, and
the controller hands over WiFi credentials as part of pairing: Matter stores
them itself and rejoins on every boot, so nothing here keeps a second copy and
no credential ever enters the source tree.

**2.4 GHz only** — the ESP32-S3 has no 5 GHz radio, and a 5 GHz-only SSID fails
silently.

The screen shows a pairing QR at first boot. Scan it in Apple Home. You
need a home hub (HomePod or Apple TV) for Matter, and Home will warn that the
accessory is uncertified — this firmware uses the Matter **test** vendor ID
`0xFFF1`, which Apple accepts and Google does not.

## Using it

- **QR screen** — top right corner of the tile grid, and only there. A hotspot
  that outlives its screen silently eats taps meant for whatever replaced it.
- **Tile grid** — one tile per accessory, six to a page. **Swipe left or right**
  to change page; the pages slide across (~140 ms) and the dots at the bottom
  show where you are.
- **`+` tile** — pick a device type, then a name. It appears in Home within a
  second or two; no restart. Eight types are on offer: `Button`, `Light`,
  `Dimmer`, `Outlet`, `Contact`, `Motion`, `Temp`, `Humidity`. The first four
  you operate; the last four report a reading you set from the screen.
- **Tap an accessory** — press it, toggle it, nudge its level ±10%, or move a
  sensor's reading. What the screen offers follows from the device type.
- **Tap the name** — opens the same name picker: the same six-to-a-page grid of
  tiles, swipe to page through it. Only free names are offered — duplicates are
  legal in Matter but miserable to tell apart in the Home app. The chosen name
  is pushed to the accessory's `NodeLabel`, so it reaches Home too.
- **remove** — top right of an accessory's screen, then tap again to confirm.
- The screen sleeps after a minute and wakes on touch.

Taps register on release, which is what makes a swipe distinguishable from a
tap. A press that drifts more than ~24 px is treated as neither.

Accessories are built *after* `esp_matter::start()`, not before it, and each one
remembers the endpoint ID it was given. That ordering is the whole trick: the
stack restores its endpoint-ID counter from NVS as it starts, and
`bridged_node::resume()` will only reclaim an ID below that counter — so before
`start()` it can never succeed, and after it always can. Adding an accessory is
then just another `create()`, and it keeps its identity across reboots without
anything restarting. See [docs/matter-notes.md](docs/matter-notes.md).

Removing one destroys its endpoint and leaves the slot empty rather than
shifting the others down — every live endpoint holds a pointer into its
accessory object, so nothing may move while the stack is running. The next
accessory added fills the gap.

## Serial console

`tools/mctl.py` drives the board over USB. It resets on connect by default;
pass `--no-reset` to attach to a running device without disturbing it.

```sh
uv run --with pyserial python tools/mctl.py --no-reset diag slots
uv run --with pyserial python tools/mctl.py --no-reset 'click 0' 'listen 20'
```

| command | what it does |
| --- | --- |
| `diag` | commissioning state, WiFi, IPv6, fabric count |
| `slots` | list accessories |
| `types` | list the device types on offer |
| `add <type>` | add an accessory, e.g. `add Dimmer` |
| `flag N` | flip a contact or motion sensor |
| `value N up\|down` | move a temperature or humidity reading |
| `remove N` | delete an accessory |
| `swipe left` / `swipe right` | change page, animation included |
| `reset slots` | back to the default six (restarts) |
| `click N` | fire a switch press |
| `on N 0\|1`, `level N 0-255` | drive a level accessory |
| `wifi` | which network the driver actually joined |
| `pairing`, `state`, `decommission` | Matter commissioning |
| `window` | reopen a 3-minute commissioning window without unpairing |
| `sleep`, `wake`, `idle N` | screen |
| `nvs`, `power`, `touchdump` | diagnostics |

`listen N` and `sleep N` are client-side helpers for scripting a session.

## What works, and what does not

Established on real hardware against Apple Home:

| | |
| --- | --- |
| Six-plus accessories as separate Home entries | yes |
| Device-set names via the bridge | yes |
| Device-initiated switch press triggers an automation | yes, ~2 s |
| Device-initiated on/off triggers an automation | yes |
| Device-initiated **brightness** triggers an automation | **no** |

Home offers no "brightness changed" automation trigger — only on/off. A
dimmable light therefore cannot act as a volume knob that drives automations.
Levels are still useful as state, and as *conditions* inside an automation
triggered by something else, but if a level must drive automation the honest
encoding is a switch press per step.

## Gotchas worth knowing

Notes for anyone doing Matter on the Arduino ESP32 core — each of these cost
real debugging time. Longer write-up in [docs/matter-notes.md](docs/matter-notes.md).

- **A bridged accessory costs 53 KB of internal RAM on esp_matter 1.5** if you
  name it with `create_node_label()`. That helper forces
  `ATTRIBUTE_FLAG_NONVOLATILE`, and the flag — not the 32-byte name — is what
  allocates. Three accessories exhaust internal DRAM, after which WiFi cannot
  start and `esp_matter::start()` fails. Create the attribute directly instead.
- **BLE commissioning depends on the core version.** arduino-esp32 3.1.x ships
  its libs without `CONFIG_ENABLE_CHIPOBLE`, so the "phone hands the device its
  WiFi credentials" flow does not exist there at all. 3.3.x enables it on
  NimBLE out of the box — see [docs/matter-notes.md](docs/matter-notes.md).
- **Do not let a data partition cover `0xE000`.** The upload tool writes
  `boot_app0.bin` to that fixed offset regardless of the partition CSV.
- **`Matter.begin()` cannot start a bridge.** It requires a private flag that
  only the library's own endpoint classes can set.
- **Progress logs do not exist.** The prebuilt libs are compiled at log level
  ERROR, so `ESP_LOGI` inside CHIP is compiled out. Errors still appear.

## Licence

MIT — see [LICENSE](LICENSE).
