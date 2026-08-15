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

## The panel cannot scroll itself

Worth knowing before designing any transition: the CO5300 command set runs
0x30 `PTLAR`, 0x31 `PTLAC`, 0x34 `TEAROFF` — there is no 0x33 `VSCRDEF` or
0x37 `VSCSAD`. Unlike most ILI/ST controllers it has no scroll register, so it
cannot shift its own frame memory in any direction. Every pixel that appears at
a new position has to be written there.

That makes a horizontal slide the worst case: every pixel on screen moves every
frame, so the delta *is* the whole screen. Pre-rendering both pages into one
double-width buffer and pushing a moving window does not help either, because a
window out of a wider buffer is strided — one SPI transaction per row instead of
one for the frame. Measured on this board:

```
whole frame, 448 rows, 322 KB, contiguous   18.8 ms
tile band,   330 rows, 243 KB, row by row   20.3 ms
```

Three quarters of the data costs more, because the ~43 us per-transaction
overhead against 330 rows outweighs the 6 ms the smaller payload saves. Fewer,
larger transfers win; the payload size barely matters.

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
endpoint::set_parent_endpoint(endpoint, aggregator);
generic_switch::add(endpoint, &switchConfig);   // or dimmable_light::add
endpoint::enable(endpoint);                     // required after startup
```

Traps:

- The `NodeLabel` buffer must outlive the call — the attribute keeps
  referencing it, so a local will dangle. Make it a member. That also means an
  array of accessories cannot be compacted while the stack runs: the endpoint's
  `priv_data` points at the object too.
- Use `attribute::create()` rather than `create_node_label()` — see the memory
  section above.
- **A switch click is an event, not an attribute.** Declaring the momentary
  feature in the config brings `NumberOfPositions`, `CurrentPosition` and the
  `InitialPress` event with it; send from
  `SystemLayer().ScheduleLambda(send_initial_press(...))`.
- `endpoint::destroy()` takes the stack lock itself, but as a
  `ScopedChipStackLock`, which is a no-op when the calling thread already holds
  it (`CHIP_STACK_LOCK_TRACKING_ENABLED` is on in these libs). Calling it from
  inside your own lock is safe; without that tracking it would deadlock.

## `<type>::add()` does not create the Descriptor cluster

`<type>::create()` is a wrapper: `endpoint::create()`, then
`descriptor::create()`, then `<type>::add()`. Only the wrapper adds the
Descriptor — `add()` never does, and neither does `endpoint::create()` or
`endpoint::resume()`, which allocate an endpoint with no clusters at all.
`bridged_node::resume()` re-creates the descriptor by hand for exactly this
reason.

So an endpoint built as `endpoint::create()` + `<type>::add()` — the shape you
need when the endpoint id has to be reclaimed from NVS — serves **no
Descriptor**. That is not cosmetic. Every endpoint shall have one (core spec
§9.5); it carries `DeviceTypeList`, `ServerList` and `PartsList`, the provider
builds `ServerClusters()` straight from the cluster list, and the Descriptor
cluster's server object is registered from that cluster's init callback. Without
it a controller walking a composed device's `PartsList` cannot read what the
parts are, and Apple Home marks the whole group unsupported — which is what it
did, from baac5d0 until 2778efe.

Create it by hand alongside `<type>::add()`:

```cpp
cluster::descriptor::config_t descriptor;
cluster::descriptor::create(endpoint, &descriptor, CLUSTER_FLAG_SERVER);
```

The symptom is a cluster count one short of the same device type built with
`<type>::create()`. This was misread for a year as a missing **Identify**
cluster; `add()` does create Identify, which is why adding it by hand appeared
to do nothing. `walk <endpoint>` prints the cluster list and settles it.

## `attribute::update()` is not what a controller reads

esp_matter keeps an attribute store. CHIP now also has a *cluster object* per
cluster — everything with a directory under
`esp_matter/data_model_provider/clusters` — and `provider::ReadAttribute()`
checks the registry first:

```cpp
if (auto *cluster = mRegistry.Get(request.path); cluster != nullptr) {
    return cluster->ReadAttribute(request, encoder);   // the store is never consulted
}
```

Those objects hold their own state. `TemperatureMeasurementCluster` starts with
`mMeasuredValue{}` — null — and esp_matter's init callback seeds only min, max
and tolerance from the store, never the value. So `attribute::update()` writes
the store, marks the path dirty, the controller re-reads, and gets null forever.
Nothing logs a failure; esp_matter's own `W :` line prints the value it just
stored, which reads like success.

Affected here: TemperatureMeasurement, RelativeHumidityMeasurement,
BooleanState, OccupancySensing. **Not** affected: OnOff and LevelControl, which
have no cluster object and are served from the store — which is exactly why the
lamp worked from day one while every sensor read empty.

The value has to go to the object:

```cpp
auto *cluster = static_cast<TemperatureMeasurementCluster *>(
    esp_matter::data_model::provider::get_instance().registry().Get(
        chip::app::ConcreteClusterPath(endpointId, TemperatureMeasurement::Id)));
cluster->SetMeasuredValue(DataModel::MakeNullable(hundredths));
```

Only `relative_humidity_measurement` ships a wrapper for this
(`SetMeasuredValue(endpointId, value)` in its `integration.h`); temperature,
boolean state and occupancy have none, so the registry lookup is the only way
in. `bridgeUpdateValue()` is where this firmware does it, and every write goes
through that one function.

The endpoint config's initial value has the same problem — the object never sees
it — so a restored reading must be pushed once after `endpoint::enable()`.

A directory under `clusters/` is not proof of a cluster object, so check what
the integration actually registers before assuming which path a cluster is on.
There are three:

```cpp
if (auto *cluster = mRegistry.Get(request.path); cluster != nullptr) { ... }   // 1
TryReadViaAccessInterface(path, AttributeAccessInterfaceRegistry::Instance().Get(...), encoder);  // 2
attribute::get_val_internal(attribute, &val);   // 3
```

**Power Source** has a directory and still lands on 3. Its integration
registers no `ServerCluster` at all — only an `AttributeAccessInterface`, and
that one answers `ActiveBatFaults`, `EndpointList` and `ClusterRevision` and
nothing else. An AAI that does not encode returns `std::nullopt` and reading
falls through, so every battery attribute is served from the store and
`attribute::update()` is the correct write. The reverse mistake is as
expensive as the original one: pushing into a registry object that is not
there fails silently too.

One consequence to keep in mind: `ClusterRevision` on this cluster is answered
by the AAI from CHIP's `PowerSource::kRevision`, so the revision esp_matter
stored is never read by anyone. They agree at 3 here, but a future submodule
bump could part them and nothing would say so.

To see what a controller would get, use `walk`: `attribute::get_val()` reads
back through the provider, so it shows the object's value, not the store's.

## Endpoint IDs: build the bridge *after* `esp_matter::start()`

The obvious arrangement — create every endpoint, then start the stack — is the
one that makes runtime changes impossible. Getting it the other way round is
what lets a bridge add an accessory without rebooting.

Endpoint IDs come from a single counter:

```cpp
endpoint->endpoint_id = current_node->min_unused_endpoint_id++;
if (esp_matter::is_started()) {
    node::store_min_unused_endpoint_id();   // NVS, but only once started
}
```

Two consequences that are easy to get wrong:

- **The counter is persisted, but only by endpoints created after the stack
  starts.** Endpoints created before it are numbered 1, 2, 3… from scratch on
  every boot, and never write the counter back.
- **`start()` overwrites the in-RAM counter** with the stored value. So the
  pre-start numbering and the stored counter drift apart, and an accessory added
  at runtime gets an ID far above the pre-start sequence. Ours took 13 where a
  cold boot would have given it 10 — the accessory changes identity on the next
  restart and controllers re-add it.

`bridged_node::resume()` is the fix, but only from the right side of `start()`:

```cpp
VerifyOrReturnError(endpoint_id < current_node->min_unused_endpoint_id, NULL,
                    ESP_LOGE(TAG, "The endpoint_id of the resumed endpoint should have been used"));
```

Called before `start()`, the counter is still climbing from 1 and any real
stored ID fails that check — which is what makes `resume()` look broken. After
`start()` the counter holds the restored value, every stored ID is below it, and
`resume()` works. There is no ordering requirement between resumed endpoints.

So the working shape is:

1. Create the node and aggregator, then `esp_matter::start()`.
2. For each saved accessory, `bridged_node::resume(node, &config, flags, storedId, priv)`,
   falling back to `create()` when there is no stored ID.
3. `endpoint::enable(endpoint)` on each — `resume()` leaves `enabled` false, and
   an endpoint built after startup is not registered with CHIP otherwise.
4. Persist whatever ID came back.

Every data-model write once the stack is up must hold `PlatformMgr().LockChipStack()`.

Adding an accessory then costs nothing but the same four steps, and its ID
survives the next boot. IDs may end up with gaps (ours run 2–10, then 14);
nothing requires them to be contiguous.

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

