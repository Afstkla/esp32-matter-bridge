# Matter on the Arduino ESP32 core — what bit us

Notes from building this firmware against arduino-esp32 3.3.11 / pioarduino
55.03.311 on an ESP32-S3, and from the 3.1.3 era before it. Each of these cost
real debugging time and none of them are obvious from the documentation.

## A bridged accessory costs 53 KB of internal RAM

The most expensive bug in the project, because nothing points at the cause.

Naming a bridged accessory is one line:

```cpp
cluster::bridged_device_basic_information::attribute::create_node_label(info, _name, 32);
```

On esp_matter 1.5 that call allocates **53,316 bytes of internal DRAM**. The
32-byte name is not the reason. The helper hardcodes
`ATTRIBUTE_FLAG_NONVOLATILE`, and calling `attribute::create()` directly with an
identical value, identical `max_val_size`, and everything else the same but
*without* that flag costs **116 bytes**:

```cpp
esp_matter::attribute::create(info, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
                              ATTRIBUTE_FLAG_WRITABLE, esp_matter_char_str(_name, 32), 32);
```

Two independent faults have to line up for this to be fatal, which is why it is
so confusing:

1. The library spends 53 KB persisting a 32-byte string.
2. The prebuilt Arduino libs set `CONFIG_ESP_MATTER_MEM_ALLOC_MODE_INTERNAL=y`,
   so `esp_matter_mem_calloc()` is pinned to `MALLOC_CAP_INTERNAL` and **cannot
   use PSRAM at all**. With 8 MB of PSRAM sitting idle, the waste lands in the
   one pool that is scarce.

The failure is silent and looks like something else entirely. Three accessories
drain internal DRAM; allocations for the rest fail quietly and cost ~1.6 KB each
instead of 53 KB; then WiFi cannot create its task:

```
E wifi:create wifi task: failed to create task
E [WiFiGeneric.cpp] wifiLowLevelInit(): esp_wifi_init 0x101: ESP_ERR_NO_MEM
E bridge: esp_matter start failed (-1)
```

Nothing mentions attributes or memory limits. Instrument with
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` around each
endpoint and the cliff is obvious in one boot.

Dropping `NONVOLATILE` means Matter no longer persists the label. That is fine
here because the names live in `Preferences` and are reapplied on boot, but a
label written by a controller will not survive a restart.

## BLE commissioning depends on the core version

On arduino-esp32 **3.1.x** the prebuilt libs are compiled *without*
`CONFIG_ENABLE_CHIPOBLE`. The CHIPoBLE transport is compiled out, not merely
disabled — no build flag brings it back:

```console
$ grep CHIPOBLE .../framework-arduinoespressif32-libs/esp32s3/sdkconfig
# CONFIG_ENABLE_CHIPOBLE is not set
```

Commissioning there is **on-network only**: the device must already hold WiFi
credentials, join, and publish `_matterc._udp` for a commissioner to find it.
This is why every Arduino Matter example calls `WiFi.begin()` and blocks until
connected *before* `Matter.begin()`. Symptom if you miss it: pairing times out
with no useful error at either end.

On **3.3.x** it is enabled out of the box, on NimBLE rather than Bluedroid:

```console
$ grep -E "CHIPOBLE|NIMBLE" .../esp32s3/sdkconfig
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_ENABLE_CHIPOBLE=y
```

So the fix for "no BLE commissioning" is to upgrade the core, not to rebuild the
libraries. Rebuilding them with `CONFIG_ENABLE_CHIPOBLE=y` on 3.1.x does work,
but Bluedroid's CHIPoBLE path is unmaintained and needs three source fixes
before it compiles and advertises — an upgrade avoids all of them, because the
NimBLE path is the one Espressif actually build and test.

Do not trust `ble_adv` in `diag` either way — it reports
`IsBLEAdvertisingEnabled()`, the stack's intent, and reads 1 even when the
controller refused the parameters. Verify with a BLE scan for service data under
UUID `0xFFF6`; the payload carries the discriminator and the vendor and product
IDs, so you can match it against the `_matterc._udp` record.

## Moving from esp_matter 1.3.0 to 1.5

Four changes, all in endpoint construction:

- `on_off.lighting` and `level_control.lighting` moved out of the cluster
  configs into sibling members `on_off_lighting` and `level_control_lighting`.
  `= nullptr` still compiles but changed meaning: it used to say "do not create
  this attribute", now it says "the attribute exists and its value is null".
  The new default is `0`, which forces the light off at boot.
- The switch cluster validates features at create time
  (`VALIDATE_FEATURES_EXACT_ONE`), so the momentary feature must be declared in
  the config rather than added afterwards. It aborts the cluster otherwise, and
  the device reboots in a loop:

  ```
  E esp_matter_cluster: Exactly one of the feature(s) must be supported from (Latching Switch,Momentary Switch)
  assert failed: ABORT_CLUSTER_CREATE
  ```

  ```cpp
  config.switch_cluster.feature_flags = cluster::switch_cluster::feature::momentary_switch::get_id();
  ```
- Declaring that feature also creates `NumberOfPositions`, `CurrentPosition` and
  the `InitialPress` event, so the explicit calls for those become duplicates.
- `create_node_label()` — see the memory section above.

Do not expect to bump esp_matter on its own. Its version is pinned by the
Arduino core: 3.1.x pins `esp_insights` to exactly `1.0.1` (to match Matter of
that era), and esp_matter 1.3.1+ requires `1.2.2`, so nothing above 1.3.0 will
resolve. 3.2.x bumps Insights and 3.3.x ships esp_matter 1.5. The unit of
upgrade is the core, not the component.

## Progress logging does not exist

```console
$ grep CONFIG_LOG_MAXIMUM_LEVEL .../esp32s3/sdkconfig
CONFIG_LOG_MAXIMUM_LEVEL=1
```

Every `ESP_LOGI`/`ESP_LOGD` inside the CHIP stack is compiled out. Raising
`CORE_DEBUG_LEVEL` achieves nothing and `esp_log_level_set()` cannot restore
what was never emitted. **Errors do still appear** (`E (31306) chip[DL]: ...`),
so silence means nothing failed rather than that logging is off.

Since progress logs are unavailable, this firmware polls the stack's own state
instead and prints on change — see the `diag` command. Reading the four
commissioning hand-offs in order tells you exactly where a failure happened:

```
DIAG t=1s ble_adv=0 window=1 wifi_prov=0 wifi_conn=0 fabrics=0
```

- `ble_adv` / `window` — is the device discoverable at all
- `wifi_prov` / `wifi_conn` — did credentials arrive and work
- `ll6` — Matter's operational traffic is IPv6-only; `::` means unreachable
- `fabrics` — did a controller actually join

`ble_adv=0` is permanent and expected on a 3.1.x core, per the section above.

## A data partition must not cover 0xE000

The nastiest bug in the project, because it presented as flaky NVS.

Stored WiFi credentials would vanish after reflashing but survive plain
reboots — intermittently. The cause is visible in any upload:

```
Flash will be erased from 0x0000e000 to 0x0000ffff...
```

**The upload tool writes `boot_app0.bin` to the hardcoded offset `0xE000`
regardless of what the partition CSV says.** A table with
`nvs, data, nvs, 0x9000, 0x7000` spans `0x9000`–`0xFFFF`, so `0xE000` lands
*inside NVS* and erases two of its pages on every single flash. Whether
anything broke depended on which page happened to hold the key.

Fix: put `otadata` where the tool already writes, and start NVS after it.

```
otadata,  data, ota,  0xE000,  0x2000,
nvs,      data, nvs,  0x10000, 0x10000,
app0,     app,  ota_0, 0x20000, 0x600000,
```

Matter is NVS-hungry (~300 entries with two fabrics), so the larger partition
is worth having anyway.

## `Matter.begin()` cannot start a bridge

`ArduinoMatter::_init()` creates the Matter node, but it is `protected` behind
a hardcoded friend list of the library's own endpoint classes
(`MatterGenericSwitch`, `MatterDimmableLight`, …). A bridge uses none of them,
so `_init()` never runs, and `Matter.begin()` then refuses to start because its
private `_matter_has_started` flag was never set.

The way through is to create the node yourself and start the stack directly:

```cpp
node::config_t config;
node_t *node = node::create(&config, attributeCb, identificationCb);
// ... create endpoints ...
esp_matter::start(eventCb);
```

`attributeCb` is a reimplementation of the library's: cast `priv_data` to
`MatterEndPoint *` and forward to `attributeChangeCB` on `PRE_UPDATE`. Keeping
accessories derived from `MatterEndPoint` means the library's helpers still
work, and the getters on the `Matter` object (commissioned state, pairing code,
QR) keep working too — none of them check that flag.

## Building a bridged accessory

```cpp
bridged_node::create(node, &config, ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE, this);
generic_switch::add(endpoint, &switchConfig);   // or dimmable_light::add
endpoint::set_parent_endpoint(endpoint, aggregator);
cluster::bridged_device_basic_information::attribute::create_node_label(info, name, len);
```

Two traps:

- The `NodeLabel` buffer must outlive the call — the attribute keeps
  referencing it, so a local will dangle. Make it a member.
- **A switch click is an event, not an attribute.** `generic_switch::add()`
  does not bring it. You must also add `momentary_switch::add()`,
  `create_initial_press()`, `create_current_position()` and
  `create_number_of_positions()`, then send from
  `SystemLayer().ScheduleLambda(send_initial_press(...))`.

## Endpoint IDs and `resume()`

Endpoints created at boot are numbered sequentially in creation order, so an
append-only accessory list yields stable IDs. An endpoint created *while the
stack runs* takes the next free ID instead, which will not match what a cold
boot assigns it — so it changes identity on restart and controllers re-add it.

`bridged_node::resume()` looks like the answer but fails with:

```
E esp_matter_core: The endpoint_id of the resumed endpoint should have been used
```

It validates against esp_matter's own allocation counter and cannot claim an ID
the current session has not yet reached. The workable approach is to persist
the accessory list and restart after adding, so everything is recreated in
order.

## Verifying commissioning from a Mac

Faster than guessing at the phone. Before scanning anything:

```console
$ dns-sd -B _matterc._udp local.
$ dns-sd -L <instance> _matterc._udp local.
  ... reached at <mac>.local.:5540   VP=65521+32768 D=3840 CM=1
```

`CM=1` means the commissioning window is open, `VP=65521` is the test vendor ID
`0xFFF1`, and port 5540 is Matter. The hostname encodes the MAC, which matches
the device's link-local IPv6 address with the U/L bit flipped — that is how you
confirm a browsed instance is *your* board and not another Matter device on the
network. Several instances resolving to the same host are just stale records
from earlier boots.

## Testing automations without watching the phone

Point the automation back at one of the device's own accessories — "Switch 1
pressed → Lamp 2 on". The device then observes the result itself and the whole
loop is visible on serial:

```
HK slot=5 onoff=0   <- test sets Lamp 2 off
PRESS 0 Lights      <- device-initiated press
HK slot=5 onoff=1   <- automation fired
```

One warning from experience: **make the automation's effect differ from the
starting state.** An automation whose action was "set Lamp 2 to 50%" against a
lamp already at 50% (128/255) fired correctly and changed nothing observable,
which read as a failure and sent us chasing a bug that did not exist.

## Board notes (Waveshare ESP32-S3-Touch-AMOLED-1.8 V2)

- The CST816 touch controller reports nothing until its interrupt mode is
  configured — write `0xFA = 0x10`. Without it the coordinate registers read
  fine but never change. It also needs a few hundred ms after reset before it
  answers at all.
- `Arduino_CO5300::displayOff()` issues `SLPIN`, and the touch controller
  shares the display module's power domain — so sleeping the panel kills touch
  and nothing can wake the device. Blank the framebuffer and set brightness to
  zero instead; on an AMOLED that is the real saving anyway.
- The CO5300 leaves the panel black if a frame arrives as many small writes.
  Push the whole framebuffer in one transaction.
- `Arduino_Canvas::begin()` allocates its framebuffer with `aligned_alloc`,
  which is internal RAM only; 322 KB will not fit beside the Matter stack.
  Pre-seed `_framebuffer` from PSRAM so `begin()` skips the allocation.
- The ESP32-S3 talks USB-Serial-JTAG, which gates its output on DTR. Clearing
  DTR to avoid a reset on connect does not avoid the reset — it just makes the
  device go silent.
- Download mode is sticky across the RTS reset that esptool ends an upload
  with, so the app never starts and the port goes quiet. `esptool --after
  watchdog-reset` leaves download mode properly; without it the only way out is
  a real power-off, and on a board with a battery attached, unplugging USB is
  not one.
