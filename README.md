# Matter accessory builder

A Matter **bridge** on an ESP32-S3 with a round AMOLED touchscreen. It presents
itself to Apple Home (or any Matter controller) as a hub, and the accessories
behind it are configured on the device's own screen — no app, no recompile.

Add a button or a dimmable level from the touchscreen, name it by tapping, and
it appears in Home under that name. Press the on-screen button and a HomeKit
automation fires.

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

Then give it WiFi. Credentials live in NVS and never enter the source tree:

```sh
uv run --with pyserial python tools/mctl.py 'ssid My Network' 'pass my-passphrase'
```

Two separate commands because an SSID and a passphrase may both contain spaces.
The device saves them and restarts. **2.4 GHz only** — the ESP32-S3 has no
5 GHz radio, and a 5 GHz-only SSID fails silently.

Once it is on WiFi the screen shows a pairing QR. Scan it in Apple Home. You
need a home hub (HomePod or Apple TV) for Matter, and Home will warn that the
accessory is uncertified — this firmware uses the Matter **test** vendor ID
`0xFFF1`, which Apple accepts and Google does not.

## Using it

- **Tile grid** — one tile per accessory, paged if there are more than six.
- **`+` tile** — add a button or a level. The device restarts (see below).
- **Tap an accessory** — press it, toggle it, or nudge its level ±10%.
- **Tap the name** — cycles through a list of preset names. The chosen name is
  pushed to the accessory's `NodeLabel`, so it reaches Home too.
- The screen sleeps after a minute and wakes on touch.

Adding an accessory restarts the device on purpose. An endpoint created while
the stack is running takes the next free endpoint ID, which is *not* the ID the
same accessory would get from a cold boot's creation order — so it would change
identity on the next restart and controllers would re-add it as a new
accessory. Recreating everything in boot order keeps IDs stable for good.
(`bridged_node::resume()` looks like the fix but validates against esp_matter's
allocation counter and cannot claim an ID the current session has not reached.)

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
| `add button` / `add level` | append an accessory (restarts) |
| `reset slots` | back to the default six (restarts) |
| `click N` | fire a switch press |
| `on N 0\|1`, `level N 0-255` | drive a level accessory |
| `ssid <name>`, `pass <key>`, `wifi`, `forget` | WiFi credentials |
| `pairing`, `state`, `decommission` | Matter commissioning |
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

- **There is no BLE commissioning.** The prebuilt Arduino libs are built
  without `CONFIG_ENABLE_CHIPOBLE`, so the usual "phone hands the device its
  WiFi credentials" flow does not exist. The device must already be on WiFi.
- **Do not let a data partition cover `0xE000`.** The upload tool writes
  `boot_app0.bin` to that fixed offset regardless of the partition CSV.
- **`Matter.begin()` cannot start a bridge.** It requires a private flag that
  only the library's own endpoint classes can set.
- **Progress logs do not exist.** The prebuilt libs are compiled at log level
  ERROR, so `ESP_LOGI` inside CHIP is compiled out. Errors still appear.

## Licence

MIT — see [LICENSE](LICENSE).
