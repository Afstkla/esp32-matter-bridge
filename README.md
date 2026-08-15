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

## Why ESP-IDF, not Arduino

This firmware used to build on arduino-esp32/pioarduino. It moved to native
ESP-IDF because the prebuilt Arduino libraries hard-code two `sdkconfig`
switches this project needs and cannot change:

- `CONFIG_PM_ENABLE` — automatic light sleep. The prebuilt libs ship without
  it, so the Arduino build could only do WiFi modem sleep plus manual CPU
  frequency scaling; the ESP-IDF build runs `esp_pm_configure()` with real DFS
  and light sleep on battery.
- `CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL` — the Matter data model
  allocates from PSRAM instead of internal DRAM. The prebuilt libs pin
  `esp_matter_mem_calloc()` to `MALLOC_CAP_INTERNAL`, so every cluster and
  attribute the stack builds at runtime ate the one pool that is scarce, and
  three accessories were enough to starve WiFi's own allocations.

Measured effect: with six accessories plus the board's own internals
accessory, free internal DRAM is **~46.6 KB** on this build versus **~19 KB**
on the old Arduino one — the six accessories cost 2.1 KB of internal DRAM and
13.9 KB of PSRAM combined, instead of internal DRAM alone.

## Quick start

```sh
. ~/esp/esp-idf/export.sh
idf.py build
```

Pinned toolchain: **ESP-IDF 5.5.5** at `~/esp/esp-idf` (the version
`espressif/esp_matter` ^1.6.0 recommends). See the [official Espressif get-started guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/) for installing ESP-IDF v5.5.5. Managed components
(`main/idf_component.yml`, pulled on first build): `espressif/esp_matter`
^1.6.0 and `espressif/esp_lcd_co5300` ^2.1.0.

The board is the sole `/dev/cu.usbmodem*` device (USB-Serial-JTAG) — resolve
it with `ls /dev/cu.usbmodem*`; the number changes when the board moves USB
ports.

```sh
idf.py -p /dev/cu.usbmodem21401 erase-flash flash
```

The **first** flash on a board must include `erase-flash` — Arduino-era NVS
content is not compatible with this firmware's layout. Later flashes just need
`flash`. Only one serial reader may hold the port at a time.

A fresh board has no network and does not need one. It advertises over BLE,
and the controller hands over WiFi credentials as part of pairing: Matter
stores them itself and rejoins on every boot, so nothing here keeps a second
copy and no credential ever enters the source tree.

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
  second or two; no restart. A fresh device starts empty and holds six, which
  is what the board carries alongside its own internals accessory. Eight types are on offer: `Button`, `Light`,
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

The firmware runs an `esp_console` REPL over USB-Serial-JTAG (prompt
`genie> `), which echoes input and prints its own prompt — unlike the old
line protocol. `tools/mctl.py` drives it:

```sh
uv run --with pyserial python tools/mctl.py --no-reset diag slots
uv run --with pyserial python tools/mctl.py --no-reset 'click 0' 'listen 20'
```

It resets the board on connect by default; pass `--no-reset` to attach to a
running device without disturbing it. Only one serial reader may hold the
port at a time.

| command | what it does |
| --- | --- |
| `help` | list every command, built in |
| `ping` | reply `PONG` |
| `heap` | free internal and PSRAM heap |
| `nvs` | NVS entry usage |
| `diag` | commissioning state, WiFi, IPv6, fabric count |
| `state` | whether the device holds a fabric |
| `slots` | list accessories |
| `types` | list the device types on offer |
| `add <type>` | add an accessory, e.g. `add Dimmer` |
| `remove N` | delete an accessory |
| `click N` | fire a switch press |
| `on N 0\|1` | drive a level accessory's on/off state |
| `level N 0-255` | drive a level accessory's brightness |
| `flag N` | flip a contact or motion sensor |
| `value N up\|down` | move a temperature or humidity reading |
| `swipe left\|right` | change page, animation included |
| `tap X Y` | inject a tap at a coordinate |
| `bright 0-255` | set panel brightness |
| `sleep` / `wake` | blank / unblank and redraw the screen |
| `qr` | toggle the pairing screen |
| `idle N` | blank the screen after N seconds, 0 to never |
| `reset slots` | remove every accessory (restarts) |
| `wifi` | which network the driver actually joined |
| `pairing` | manual pairing code and QR payload |
| `window` | reopen a 3-minute commissioning window without unpairing |
| `decommission` | erase all fabrics and restart |
| `touchdump <ms>` | stream touch register changes |
| `screendump` | stream the framebuffer as base64 (see below) |
| `power [locks\|on\|off\|80\|160\|240]` | power management tuning — **debug surface** |
| `ps none\|min\|max` | set WiFi power save mode |
| `battlog` | dump the battery drain log, oldest first |
| `bt` | backtrace every task — **debug surface**, names the line a wedged task is parked on |
| `passwd <secret>\|clear` | set or remove the remote console secret (see below) — USB only |

`listen N` and `sleep N` in `mctl.py` are client-side helpers for scripting a
session, not device commands.

## Remote console

The USB console dies exactly when the interesting data exists — on battery,
light-sleeping, with `battlog` filling up. The same commands are therefore
reachable over TCP on port 5323, behind a shared secret set over USB:

```
genie> passwd <your-secret>
PASSWD set, console on port 5323
```

```sh
python tools/genie-console.py <device-ip>     # $GENIE_SECRET skips the prompt
```

`wifi` prints the address to aim at. `passwd` is USB-only: a session that only
exists because of the current secret cannot change it. `passwd clear` removes
the secret and stops the listener within a second, and with no secret stored
nothing is bound at all. One session at a time — a second caller is told `BUSY`
and hung up on — and a session idle for ten minutes is dropped.

The device answers a new connection with `CHALLENGE <32 hex>`; the client
replies with the hex HMAC-SHA256 of that nonce keyed by the secret, compared in
constant time, three answers per connection with three seconds between them.
The secret itself never crosses the wire — but everything after the handshake
does, in the clear, with no integrity check on the session: this is a
home-LAN debugging tool, not something to put in front of the internet.

While a session is up the log stream is teed to it, so a `DIAG` line or a
driver warning appears in the client as it happens. The tee runs on whichever
task logged, so it never blocks: a socket that will not take the bytes loses
log lines instead. The count comes back in the `BYE` line — which a client
that vanishes without closing cleanly never gets to read. A client that stops
reading altogether is dropped after one ten-second send timeout, rather than
being allowed to park the console for as long as it likes.

## Verifying the screen without eyes

`tools/screenshot.py` drives `screendump` and turns the answer into a PNG,
which is how this repo checks rendering changes without a human looking at
the panel:

```sh
uv run --with pyserial,pillow python tools/screenshot.py shot.png swipe left
```

Any arguments after the output path run as console commands first, so the
usual pattern is "do something on screen, then capture it" in one call. The
board is never reset — it photographs whatever state it is already in.
`screendump` streams the framebuffer as base64 lines terminated by
`SCREENDUMP END`; WiFi log lines land in the middle of that stream and are
filtered by alphabet (base64 only), and a short or garbled payload is
retried up to three times rather than decoded into a plausible-but-wrong
picture. The dump runs on the console task while the ui task keeps drawing,
so it can tear if a screen changes mid-dump — screens here are static except
for a ~150 ms swipe animation, so in practice a dump almost always lands on a
still frame.

## The `esp_lcd` override

`components/esp_lcd` is a local, patched copy of ESP-IDF 5.5.5's `esp_lcd`
component — see [its `PATCH.md`](components/esp_lcd/PATCH.md) for the exact
diff. It exists because stock `esp_lcd` leaks an in-flight transaction slot:
a PSRAM-sourced DMA chunk that hits a TX underflow still completes and posts
a result, just flagged `ESP_ERR_INVALID_STATE`, and the stock drain loop in
`esp_lcd_panel_io_spi.c` consumes that result without decrementing
`num_trans_inflight` before bailing. The transaction count then runs one
ahead of reality forever, and the next frame's drain blocks on
`portMAX_DELAY` waiting for a result that will never arrive — the ui task
wedges, both cores go idle, and only the REPL stays alive. The patch routes
the five drain loops through a helper that treats that error as "recycled
with a fault" (log a warning, keep counting) instead of aborting.

The override is pinned to IDF 5.5.5: its `CMakeLists.txt` prints a CMake
warning if built against any other version, because it is a copy of one
version's component, not a fork that tracks upstream. It is an upstream
candidate — the same leak is in all five drain loops in stock `esp_lcd`, on
any IDF version with the same drain shape.

## Power model

Automatic light sleep (`CONFIG_PM_ENABLE`, DFS 80–240 MHz) runs only on
**battery**: an `ESP_PM_NO_LIGHT_SLEEP` lock is held while USB VBUS is
present, so the console stays alive and responsive whenever the board is
plugged in, and is released only on a PMU reading that confirms VBUS is
gone. WiFi power save defaults to `WIFI_PS_MAX_MODEM`, tunable at runtime
with `ps none|min|max` (persisted in NVS).

An **uncommissioned** device will not sleep even on battery: NimBLE holds its
own `NO_LIGHT_SLEEP` lock while advertising, and this firmware advertises
until a fabric is joined. Once commissioned, BLE tears down
(`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING`) and the lock releases with it.

The station is forced to `WIFI_MODE_STA` at startup. `esp_wifi` keeps its mode
in NVS and restores it at init, and CHIP only ever upgrades a stored AP mode to
APSTA — so a SoftAP left behind by any earlier firmware survives every reflash
and beacons forever, pinning the `wifi` `APB_FREQ_MAX` lock and blocking both
modem and light sleep. `power` reports `wifi_mode=`; anything but `sta` is that
bug returning.

Battery percent is interpolated from a resting LiPo OCV table, not from the
AXP2101's fuel gauge, which has no battery model programmed and reported 52% at
3.58 V on a cell that died half an hour later. Voltage sags under load, so the
estimate reads low while the device is busy and the raw `mV` stays the number
to trust.

Drain is measured with `battlog`: a 144-entry ring in NVS, sampled every 10
minutes (`mv`/`pct`/`flags` with USB/charging/boot bits), giving a rolling
24-hour window. It survives reboots and dumps oldest-first. Entries older than
this change carry the fuel gauge's numbers and are not comparable.

`power` (debug surface) reports the live PM config, WiFi PS mode, last flush
time and PMU reading; `power locks` dumps the PM lock table; `power
80|160|240` pins the clock for testing and `power off` disables light sleep
entirely, falling back to the fixed 240 MHz clock — the escape hatch if the
panel path ever misbehaves under DFS again.

## Genie's own internals

The board reports itself as one further accessory, and it is the only one here
that is not invented: a **composed device** — a bridged node carrying a child
endpoint per function, whose `PartsList` is what tells a controller they are one
thing. Apple Home draws it as a single tile. Inside are the PMU die
temperature, the battery percentage, and the display brightness, which really
does dim the panel.

Six accessories is the cap because the tile grid is six-to-a-page by design,
not because memory demands it — see "Why ESP-IDF" above for what six actually
cost once the data model moved to PSRAM.

## What works, and what does not

Established on real hardware against Apple Home (Arduino firmware; the port
carries the same Matter data model, so this behaviour is expected to carry
over unchanged):

| | |
| --- | --- |
| Six-plus accessories as separate Home entries | yes |
| Device-set names via the bridge | yes |
| Device-initiated switch press triggers an automation | yes, ~2 s |
| Device-initiated on/off triggers an automation | yes |
| Device-initiated **brightness** triggers an automation | **no** |
| A composed device shown as one grouped tile | yes |

Home offers no "brightness changed" automation trigger — only on/off. A
dimmable light therefore cannot act as a volume knob that drives automations.
Levels are still useful as state, and as *conditions* inside an automation
triggered by something else, but if a level must drive automation the honest
encoding is a switch press per step.

## Gotchas worth knowing

Notes from this port, each of which cost real debugging time. The Arduino-era
write-up (naming, BLE-transport-by-core-version, the old upload tool's
partition trap) is retired along with the toolchain that caused it; what's
still true from it is folded in below. Longer background on the Matter-side
items: [docs/matter-notes.md](docs/matter-notes.md).

- **Only the ui task may touch the panel or the framebuffer.** Console
  commands that affect the screen (`bright`, `sleep`, `wake`, `swipe`, `qr`,
  `tap`) set an intent that the ui task's own tick applies; nothing calls
  `panelFlush` from the REPL task. Two tasks racing to flush left a
  done-semaphore counting frames nobody was waiting on.
- **The QSPI clock is 40 MHz, not 80.** 80 MHz underflows the DMA once the
  framebuffer lives in PSRAM under `esp_lcd` — the Arduino build got away
  with it because its driver's transaction accounting happened to tolerate
  the resulting underflow; this one does not, until the `esp_lcd` override
  above recycles the fault instead of wedging on it.
- **Matter attributes declared `NULLABLE` reject a plain value.** `CurrentLevel`
  and similar attributes need `esp_matter_nullable_uint8` (or the matching
  nullable wrapper for the type), not the plain `esp_matter_uint8` —
  `attribute::update` on the plain form fails silently with
  `ESP_ERR_INVALID_ARG` (258), the local UI still looks right because it reads
  its own cached state, and no controller ever sees the write. This bug was
  latent in the Arduino firmware too.
- **Endpoint IDs and `resume()`.** Endpoint IDs come from one counter that is
  only persisted by endpoints created *after* `esp_matter::start()`, and
  `bridged_node::resume()` only accepts a stored ID below the counter's
  current value — so accessories must be built after `start()`, not before
  it. Still true, still the whole trick behind adding an accessory without a
  reboot.
- **`create_node_label()` forces `ATTRIBUTE_FLAG_NONVOLATILE`.** Use
  `attribute::create()` directly for a bridged accessory's name instead —
  under `CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL` the RAM cost of that flag
  is no longer the crisis it was on Arduino, but dropping `NONVOLATILE` still
  matters for correctness: Matter then relies on this firmware to reapply the
  name from NVS on boot rather than persisting it itself, so a name a
  controller writes directly will not survive a restart.
- **`screendump`/`screenshot.py` is how this repo verifies rendering** — see
  above. There is no other way to confirm a screen change without physically
  looking at the board.

## Licence

MIT — see [LICENSE](LICENSE).
