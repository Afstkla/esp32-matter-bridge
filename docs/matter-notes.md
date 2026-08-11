# Matter on the Arduino ESP32 core — what bit us

Notes from building this firmware against arduino-esp32 3.1.3 / pioarduino
53.03.13 on an ESP32-S3. Each of these cost real debugging time and none of
them are obvious from the documentation.

## There is no BLE commissioning

The prebuilt Arduino libs are compiled **without** `CONFIG_ENABLE_CHIPOBLE`:

```console
$ grep CHIPOBLE .../framework-arduinoespressif32-libs/esp32s3/sdkconfig
# CONFIG_ENABLE_CHIPOBLE is not set
```

The CHIPoBLE transport is *compiled out*, not merely disabled — no build flag
brings it back. So the familiar Matter flow, where the phone finds the
accessory over Bluetooth and hands it WiFi credentials, cannot happen.

Commissioning is **on-network only**: the device must already hold WiFi
credentials, join, and publish `_matterc._udp` over mDNS for a commissioner to
find it. This is why every Arduino Matter example calls `WiFi.begin()` and
blocks until connected *before* `Matter.begin()`.

Symptom if you miss it: pairing simply times out, with no useful error at
either end.

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

`ble_adv=0` is permanent and expected here, per the section above.

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
