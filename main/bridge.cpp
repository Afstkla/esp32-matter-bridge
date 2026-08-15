#include "bridge.h"

#include <cstdio>
#include <cstring>

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "console.h"
#include "net.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "bridge";

static endpoint_t *s_aggregator = nullptr;

// The node is created with one attribute callback for the whole data model, so
// the endpoint's priv_data is the only thing that says which object a write
// belongs to. POST_UPDATE rather than PRE_UPDATE: nothing here vetoes a write,
// and reading back after the stack has stored it is one less way to disagree
// with it.
static esp_err_t attributeCb(attribute::callback_type_t type, uint16_t endpoint_id,
                             uint32_t cluster_id, uint32_t attribute_id,
                             esp_matter_attr_val_t *val, void *priv_data) {
  if (type == attribute::POST_UPDATE && priv_data != nullptr) {
    ((MatterEndPoint *)priv_data)->attributeChanged(cluster_id, attribute_id, val);
  }
  return ESP_OK;
}

static esp_err_t identificationCb(identification::callback_type_t type, uint16_t endpoint_id,
                                  uint8_t effect_id, uint8_t effect_variant, void *priv_data) {
  return ESP_OK;
}

// Commissioning crosses four hand-offs (BLE discovery, credential transfer,
// operational IPv6/mDNS, fabric join) and none of them log a failure, so the
// stack's own state is the only way to see which hand-off an attempt reached.
struct CommissioningStage {
  bool bleAdvertising;
  bool windowOpen;
  bool wifiProvisioned;
  bool wifiConnected;
  uint8_t fabrics;
};

// What bridgeCommissioned() and bridgePairingWindowOpen() answer from: written
// by the CHIP task, read by the ui task on every redraw, so volatile like every
// other flag this firmware passes between tasks. s_stage itself never leaves
// the CHIP task — it is only the baseline the event callback diffs against.
static CommissioningStage s_stage{};
static volatile bool s_commissioned = false;
static volatile bool s_windowOpen = false;

static CommissioningStage readStage() {
  CommissioningStage s{};
  s.bleAdvertising = chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertisingEnabled();
  s.windowOpen =
      chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
  s.wifiProvisioned = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationProvisioned();
  s.wifiConnected = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected();
  s.fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
  return s;
}

static void printStage(const CommissioningStage &s) {
  printf("DIAG t=%us ble_adv=%d window=%d wifi_prov=%d wifi_conn=%d fabrics=%u\n",
         (unsigned)(esp_timer_get_time() / 1000000), s.bleAdvertising, s.windowOpen,
         s.wifiProvisioned, s.wifiConnected, s.fabrics);
  NetStatus net = netStatus();
  printf("DIAG ssid=%s rssi=%d ch=%u ip=%s ll6=%s\n", net.ssid, net.rssi, net.channel, net.ip,
         net.linkLocal);
}

// The Arduino build polled this from loop() every 500 ms because it had a loop
// to poll from. Every hand-off worth watching raises a stack event, so the
// callback that was empty there does the same job here for nothing.
static void eventCb(const ChipDeviceEvent *event, intptr_t arg) {
  CommissioningStage now = readStage();
  if (memcmp(&now, &s_stage, sizeof(now)) == 0) {
    return;
  }
  s_stage = now;
  s_commissioned = now.fabrics > 0;
  s_windowOpen = now.windowOpen;
  printStage(now);
}

// What the bridge calls itself. Only the node label is ours to set here: the
// vendor and product strings come from the compiled-in test providers.
static const char *DEVICE_NAME = "Genie";

bool bridgeBegin() {
  node::config_t node_config;
  strlcpy(node_config.root_node.basic_information.node_label, DEVICE_NAME,
          sizeof(node_config.root_node.basic_information.node_label));
  node_t *node = node::create(&node_config, attributeCb, identificationCb);
  if (node == nullptr) {
    ESP_LOGE(TAG, "node create failed");
    return false;
  }

  aggregator::config_t config;
  s_aggregator = aggregator::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
  if (s_aggregator == nullptr) {
    ESP_LOGE(TAG, "aggregator create failed");
    return false;
  }
  ESP_LOGI(TAG, "aggregator on endpoint %u", endpoint::get_id(s_aggregator));
  return true;
}

static const uint32_t PAIRING_WINDOW_SECONDS = 180;

// A device that already holds a fabric advertises nothing — no commissioning
// window, no BLE — so its pairing code is unusable unless something asks for a
// window first.
static void openPairingWindowOnStack(intptr_t) {
  auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
  if (manager.IsCommissioningWindowOpen()) {
    return;
  }
  CHIP_ERROR err =
      manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(PAIRING_WINDOW_SECONDS));
  s_windowOpen = err == CHIP_NO_ERROR;
  printf("WINDOW %s\n", err == CHIP_NO_ERROR ? "open" : "failed");
}

// Handed to the CHIP task, which already holds the stack lock, rather than
// taking it here: the ui task opens a window from a corner tap and
// LockChipStack has no timeout.
void bridgeOpenPairingWindow() {
  chip::DeviceLayer::PlatformMgr().ScheduleWork(openPairingWindowOnStack, 0);
}

bool bridgePairingWindowOpen() {
  return s_windowOpen;
}

bool bridgeCommissioned() {
  return s_commissioned;
}

// The raw "MT:..." payload, which is what Apple Home scans. The URL wrapper the
// Arduino library handed out had to be cut back to this anyway.
bool bridgePairingPayload(char *out, size_t size) {
  chip::MutableCharSpan span(out, size);
  return GetQRCode(span, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE)) ==
         CHIP_NO_ERROR;
}

bool bridgePairingCode(char *out, size_t size) {
  chip::MutableCharSpan span(out, size);
  return GetManualPairingCode(
             span, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE)) ==
         CHIP_NO_ERROR;
}

static void printPairing() {
  // The generator refuses a buffer without room for the check digit as well as
  // the terminator, so the code length is not the buffer size.
  char code[chip::kManualSetupLongCodeCharLength + 2] = {0};
  char payload[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {
      0};
  if (bridgePairingCode(code, sizeof(code))) {
    printf("CODE %s\n", code);
  }
  if (bridgePairingPayload(payload, sizeof(payload))) {
    printf("QR %s\n", payload);
  }
}

static int cmdDiag(int argc, char **argv) {
  CommissioningStage stage;
  {
    StackLock lock;
    stage = readStage();
  }
  printStage(stage);
  printf("DIAG commissioned=%d heap=%u internal=%u psram=%u\n", stage.fabrics > 0 ? 1 : 0,
         (unsigned)esp_get_free_heap_size(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return 0;
}

static int cmdState(int argc, char **argv) {
  printf("COMMISSIONED %d\n", bridgeCommissioned() ? 1 : 0);
  return 0;
}

static int cmdPairing(int argc, char **argv) {
  printPairing();
  return 0;
}

static int cmdWindow(int argc, char **argv) {
  bridgeOpenPairingWindow();
  return 0;
}

static int cmdDecommission(int argc, char **argv) {
  printf("DECOMMISSIONED\n");
  esp_matter::factory_reset();
  return 0;
}

static int cmdTypes(int argc, char **argv) {
  for (uint8_t type = 0; type < ACC_TYPE_COUNT; type++) {
    printf("TYPE %u %s\n", type, accessoryTypeName(type));
  }
  return 0;
}

static int cmdWifi(int argc, char **argv) {
  NetStatus net = netStatus();
  printf("WIFI joined='%s' ip=%s rssi=%d\n", net.ssid, net.ip, net.rssi);
  return 0;
}

bool bridgeStart() {
  esp_err_t err = esp_matter::start(eventCb);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_matter start failed (%d)", err);
    return false;
  }

  {
    StackLock lock;
    s_stage = readStage();
  }
  s_commissioned = s_stage.fabrics > 0;
  s_windowOpen = s_stage.windowOpen;
  printStage(s_stage);

  consoleRegisterCmd("diag", "Show commissioning stage, network and heap", cmdDiag);
  consoleRegisterCmd("state", "Report whether the device holds a fabric", cmdState);
  consoleRegisterCmd("pairing", "Print the manual pairing code and QR payload", cmdPairing);
  consoleRegisterCmd("window", "Open a commissioning window", cmdWindow);
  consoleRegisterCmd("decommission", "Erase all fabrics and restart", cmdDecommission);
  consoleRegisterCmd("types", "List the accessory device types", cmdTypes);
  consoleRegisterCmd("wifi", "Show the WiFi station the stack joined", cmdWifi);
  return true;
}

bool MatterEndPoint::updateAttributeVal(uint32_t clusterId, uint32_t attributeId,
                                        esp_matter_attr_val_t *val) {
  StackLock lock;
  return esp_matter::attribute::update(_endpointId, clusterId, attributeId, val) == ESP_OK;
}

// Endpoints built before esp_matter::start() are registered by the stack as it
// comes up. One built afterwards has to be enabled by hand.
static bool publish(esp_matter::endpoint_t *endpoint) {
  if (!esp_matter::is_started()) {
    return true;
  }
  esp_err_t err = endpoint::enable(endpoint);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "endpoint enable failed (%d)", err);
    return false;
  }
  return true;
}

endpoint_t *bridgeAggregator() {
  return s_aggregator;
}

bool bridgePublish(esp_matter::endpoint_t *endpoint) {
  StackLock lock;
  return publish(endpoint);
}

// The name buffer is a member rather than a local because the attribute keeps
// referring to it, and it is sized once here so later renames always fit.
//
// An accessory reclaims the endpoint id it had last time. That only works after
// esp_matter::start() has restored its min-unused-endpoint-id counter from NVS,
// because resume() refuses any id the counter has not passed yet. Falling back
// to create() covers a first run and a wiped counter; the caller persists
// whichever id came back.
endpoint_t *BridgedAccessory::createNode(const char *name, uint16_t endpointId) {
  if (s_aggregator == nullptr) {
    ESP_LOGE(TAG, "begin the aggregator first");
    return nullptr;
  }
  strlcpy(_name, name, sizeof(_name));

  bridged_node::config_t config;
  const uint8_t flags = ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE;
  endpoint_t *endpoint = nullptr;
  if (endpointId != 0) {
    endpoint = bridged_node::resume(node::get(), &config, flags, endpointId,
                                    static_cast<MatterEndPoint *>(this));
    if (endpoint == nullptr) {
      ESP_LOGW(TAG, "endpoint %u could not be resumed, taking a new one", endpointId);
    }
  }
  if (endpoint == nullptr) {
    endpoint = bridged_node::create(node::get(), &config, flags, static_cast<MatterEndPoint *>(this));
  }
  if (endpoint == nullptr) {
    ESP_LOGE(TAG, "bridged node create failed");
    return nullptr;
  }
  endpoint::set_parent_endpoint(endpoint, s_aggregator);

  // Deliberately not create_node_label(): it forces ATTRIBUTE_FLAG_NONVOLATILE,
  // which costs 53 KB of internal DRAM per endpoint and starves the WiFi driver
  // after three accessories. The names are persisted alongside the slots and
  // reapplied on boot, so Matter does not need to store them.
  cluster_t *info = cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
  esp_matter::attribute::create(info, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
                                ATTRIBUTE_FLAG_WRITABLE,
                                esp_matter_char_str(_name, sizeof(_name) - 1), sizeof(_name) - 1);
  return endpoint;
}

namespace {
struct TypeInfo {
  const char *name;
  AccessoryUi ui;
  const char *unit;
  int16_t step;
  int16_t min;
  int16_t max;
};

// Indexed by AccessoryType. Sensor ranges are the ones Apple will show without
// complaint rather than the full attribute range, which reaches -273 C.
const TypeInfo TYPES[ACC_TYPE_COUNT] = {
    {"Button", UI_PRESS, "", 0, 0, 0},
    {"Light", UI_ONOFF, "", 0, 0, 0},
    {"Dimmer", UI_LEVEL, "", 0, 0, 0},
    {"Outlet", UI_ONOFF, "", 0, 0, 0},
    {"Contact", UI_FLAG, "", 0, 0, 0},
    {"Motion", UI_FLAG, "", 0, 0, 0},
    {"Temp", UI_VALUE, "C", 50, -2000, 6000},
    {"Humidity", UI_VALUE, "%", 100, 0, 10000},
};

const TypeInfo &info(uint8_t type) {
  return TYPES[type < ACC_TYPE_COUNT ? type : (uint8_t)ACC_BUTTON];
}
}  // namespace

AccessoryUi accessoryUi(uint8_t type) {
  return info(type).ui;
}

const char *accessoryTypeName(uint8_t type) {
  return info(type).name;
}

const char *accessoryUnit(uint8_t type) {
  return info(type).unit;
}

int16_t accessoryStep(uint8_t type) {
  return info(type).step;
}

int16_t accessoryMin(uint8_t type) {
  return info(type).min;
}

int16_t accessoryMax(uint8_t type) {
  return info(type).max;
}

// Initial readings are carried in the cluster config rather than written after
// enable, because an endpoint built before esp_matter::start() has no attribute
// storage to write to yet.
static esp_err_t addDeviceType(uint8_t type, endpoint_t *endpoint, bool on, uint8_t level,
                               bool flag, int16_t value) {
  switch (type) {
    case ACC_BUTTON: {
      generic_switch::config_t config;
      // The cluster aborts creation unless exactly one of the latching/momentary
      // features is already declared, so this cannot be added afterwards.
      // Declaring it also brings the position attributes and the InitialPress event.
      config.switch_cluster.feature_flags =
          cluster::switch_cluster::feature::momentary_switch::get_id();
      return generic_switch::add(endpoint, &config);
    }
    case ACC_DIMMER: {
      dimmable_light::config_t config;
      config.on_off.on_off = on;
      config.on_off_lighting.start_up_on_off = nullptr;
      config.level_control.current_level = level;
      config.level_control_lighting.start_up_current_level = nullptr;
      return dimmable_light::add(endpoint, &config);
    }
    case ACC_LIGHT: {
      on_off_light::config_t config;
      config.on_off.on_off = on;
      config.on_off_lighting.start_up_on_off = nullptr;
      return on_off_light::add(endpoint, &config);
    }
    case ACC_OUTLET: {
      on_off_plug_in_unit::config_t config;
      config.on_off.on_off = on;
      config.on_off_lighting.start_up_on_off = nullptr;
      return on_off_plug_in_unit::add(endpoint, &config);
    }
    case ACC_CONTACT: {
      contact_sensor::config_t config;
      config.boolean_state.state_value = flag;
      return contact_sensor::add(endpoint, &config);
    }
    case ACC_MOTION: {
      occupancy_sensor::config_t config;
      config.occupancy_sensing.occupancy = flag ? 1 : 0;
      // Like the switch cluster, this one refuses to be created without a
      // sensing technology declared, and aborts rather than returning an error.
      config.occupancy_sensing.feature_flags =
          cluster::occupancy_sensing::feature::passive_infrared::get_id();
      return occupancy_sensor::add(endpoint, &config);
    }
    case ACC_TEMPERATURE: {
      temperature_sensor::config_t config;
      config.temperature_measurement.measured_value = nullable<int16_t>(value);
      return temperature_sensor::add(endpoint, &config);
    }
    case ACC_HUMIDITY: {
      humidity_sensor::config_t config;
      config.relative_humidity_measurement.measured_value = nullable<uint16_t>((uint16_t)value);
      return humidity_sensor::add(endpoint, &config);
    }
    default:
      return ESP_ERR_INVALID_ARG;
  }
}

// Matter puts ClusterRevision, FeatureMap, AttributeList, AcceptedCommandList
// and GeneratedCommandList on every cluster, so the attribute count runs far
// ahead of the handful an accessory actually uses: a contact sensor carries 18
// records to publish one bool, at roughly 100 bytes of heap apiece. Running out
// of heap is how this board fails, so the count is worth printing.
//
// Deliberately not a heap delta: the stack allocates and frees on its own
// threads while an endpoint is being built, and a measured endpoint came out
// 608 bytes to the good. Use the heap figure from `diag` instead.
void bridgeCount(endpoint_t *endpoint, uint16_t *clusters, uint16_t *attributes) {
  *clusters = 0;
  *attributes = 0;
  for (cluster_t *c = cluster::get_first(endpoint); c != nullptr; c = cluster::get_next(c)) {
    (*clusters)++;
    for (attribute_t *a = attribute::get_first(c); a != nullptr; a = attribute::get_next(a)) {
      (*attributes)++;
    }
  }
}

bool BridgedAccessory::begin(uint8_t type, const char *name, uint16_t endpointId) {
  if (type >= ACC_TYPE_COUNT) {
    ESP_LOGE(TAG, "unknown device type %u", type);
    return false;
  }
  StackLock lock;
  endpoint_t *endpoint = createNode(name, endpointId);
  if (endpoint == nullptr) {
    return false;
  }

  _type = type;
  if (accessoryUi(type) == UI_VALUE && _value == 0) {
    _value = type == ACC_HUMIDITY ? 5000 : 2100;
  }
  if (addDeviceType(type, endpoint, _on, _level, _flag, _value) != ESP_OK) {
    ESP_LOGE(TAG, "%s add failed", accessoryTypeName(type));
    return false;
  }
  if (!publish(endpoint)) {
    return false;
  }

  setEndPointId(endpoint::get_id(endpoint));
  _started = true;
  uint16_t clusters = 0, attributes = 0;
  bridgeCount(endpoint, &clusters, &attributes);
  ESP_LOGI(TAG, "%s '%s' on endpoint %u (%u clusters, %u attributes)", accessoryTypeName(type),
           _name, getEndPointId(), clusters, attributes);
  return true;
}

bool BridgedAccessory::setFlag(bool active) {
  if (!_started || _flag == active) {
    return _started;
  }
  _flag = active;
  if (_type == ACC_MOTION) {
    esp_matter_attr_val_t val = esp_matter_bitmap8(active ? 1 : 0);
    return updateAttributeVal(OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id,
                              &val);
  }
  esp_matter_attr_val_t val = esp_matter_bool(active);
  return updateAttributeVal(BooleanState::Id, BooleanState::Attributes::StateValue::Id, &val);
}

bool BridgedAccessory::setValue(int16_t hundredths) {
  if (!_started || _value == hundredths) {
    return _started;
  }
  _value = hundredths;
  if (_type == ACC_HUMIDITY) {
    esp_matter_attr_val_t val = esp_matter_nullable_uint16((uint16_t)hundredths);
    return updateAttributeVal(RelativeHumidityMeasurement::Id,
                              RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
  }
  esp_matter_attr_val_t val = esp_matter_nullable_int16(hundredths);
  return updateAttributeVal(TemperatureMeasurement::Id,
                            TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
}

bool BridgedAccessory::remove() {
  if (!_started) {
    return false;
  }
  StackLock lock;
  endpoint_t *endpoint = endpoint::get(node::get(), getEndPointId());
  if (endpoint == nullptr) {
    ESP_LOGE(TAG, "endpoint %u not found", getEndPointId());
    return false;
  }
  esp_err_t err = endpoint::destroy(node::get(), endpoint);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "endpoint destroy failed (%d)", err);
    return false;
  }
  ESP_LOGI(TAG, "removed '%s' from endpoint %u", _name, getEndPointId());
  _started = false;
  _onOffCB = nullptr;
  _levelCB = nullptr;
  return true;
}

void BridgedAccessory::setName(const char *name) {
  strlcpy(_name, name, sizeof(_name));
  if (!_started) {
    return;
  }
  esp_matter_attr_val_t val = esp_matter_char_str(_name, strlen(_name));
  updateAttributeVal(BridgedDeviceBasicInformation::Id,
                     BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &val);
}

void BridgedAccessory::click() {
  if (!_started) {
    return;
  }
  int id = getEndPointId();
  chip::DeviceLayer::SystemLayer().ScheduleLambda(
      [id]() { cluster::switch_cluster::event::send_initial_press(id, 1); });
}

bool BridgedAccessory::setOnOff(bool on) {
  if (!_started || _on == on) {
    return _started;
  }
  _on = on;
  esp_matter_attr_val_t val = esp_matter_bool(on);
  return updateAttributeVal(OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

bool BridgedAccessory::setBrightness(uint8_t level) {
  if (!_started || _level == level) {
    return _started;
  }
  _level = level;
  // CurrentLevel is declared ATTRIBUTE_FLAG_NULLABLE, and attribute::update
  // rejects a plain uint8 against it with ESP_ERR_INVALID_ARG.
  esp_matter_attr_val_t val = esp_matter_nullable_uint8(level);
  return updateAttributeVal(LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);
}

void BridgedAccessory::attributeChanged(uint32_t clusterId, uint32_t attributeId,
                                        esp_matter_attr_val_t *val) {
  if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
    _on = val->val.b;
    if (_onOffCB) {
      _onOffCB(_on);
    }
  } else if (clusterId == LevelControl::Id &&
             attributeId == LevelControl::Attributes::CurrentLevel::Id) {
    _level = val->val.u8;
    if (_levelCB) {
      _levelCB(_level);
    }
  }
}
