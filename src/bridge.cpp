#include "bridge.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static endpoint_t *s_aggregator = nullptr;

// ArduinoMatter::_init() creates the node, but it is protected with a fixed
// friend list of the library's own endpoint classes, so a bridge that uses
// none of them has to create the node itself. That also means Matter.begin()
// is unusable (it refuses to run unless _init set its private flag) and
// esp_matter::start() is called directly instead. The getters on the Matter
// object — commissioned state, pairing code, QR — do not check that flag and
// keep working.
static esp_err_t attributeCb(attribute::callback_type_t type, uint16_t endpoint_id,
                             uint32_t cluster_id, uint32_t attribute_id,
                             esp_matter_attr_val_t *val, void *priv_data) {
  if (type == attribute::PRE_UPDATE && priv_data != nullptr) {
    MatterEndPoint *ep = (MatterEndPoint *)priv_data;
    return ep->attributeChangeCB(endpoint_id, cluster_id, attribute_id, val) ? ESP_OK : ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t identificationCb(identification::callback_type_t type, uint16_t endpoint_id,
                                  uint8_t effect_id, uint8_t effect_variant, void *priv_data) {
  return ESP_OK;
}

static void eventCb(const ChipDeviceEvent *event, intptr_t arg) {}

// What the bridge calls itself. Only the node label is ours to set here: the
// vendor and product strings are compiled into the prebuilt CHIP libraries.
static const char *DEVICE_NAME = "Genie";

bool bridgeBegin() {
  node::config_t node_config;
  strlcpy(node_config.root_node.basic_information.node_label, DEVICE_NAME,
          sizeof(node_config.root_node.basic_information.node_label));
  node_t *node = node::create(&node_config, attributeCb, identificationCb);
  if (node == nullptr) {
    Serial.println("bridge: node create failed");
    return false;
  }

  aggregator::config_t config;
  s_aggregator = aggregator::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
  if (s_aggregator == nullptr) {
    Serial.println("bridge: aggregator create failed");
    return false;
  }
  Serial.printf("bridge: aggregator on endpoint %u\n", endpoint::get_id(s_aggregator));
  return true;
}

bool bridgeStart() {
  esp_err_t err = esp_matter::start(eventCb);
  if (err != ESP_OK) {
    Serial.printf("bridge: esp_matter start failed (%d)\n", err);
    return false;
  }
  return true;
}

// Endpoints built before esp_matter::start() are registered by the stack as it
// comes up. One built afterwards has to be enabled by hand, and every
// data-model write while the stack runs has to hold its lock.
namespace {
class LiveEdit {
public:
  LiveEdit() : _live(esp_matter::is_started()) {
    if (_live) {
      chip::DeviceLayer::PlatformMgr().LockChipStack();
    }
  }
  ~LiveEdit() {
    if (_live) {
      chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }
  }
  bool live() const {
    return _live;
  }

private:
  bool _live;
};
}  // namespace

static bool publish(esp_matter::endpoint_t *endpoint, const LiveEdit &edit) {
  if (!edit.live()) {
    return true;
  }
  esp_err_t err = endpoint::enable(endpoint);
  if (err != ESP_OK) {
    Serial.printf("bridge: endpoint enable failed (%d)\n", err);
    return false;
  }
  return true;
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
    Serial.println("bridge: begin the aggregator first");
    return nullptr;
  }
  strlcpy(_name, name, sizeof(_name));

  bridged_node::config_t config;
  const uint8_t flags = ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE;
  endpoint_t *endpoint = nullptr;
  if (endpointId != 0) {
    endpoint = bridged_node::resume(node::get(), &config, flags, endpointId, (void *)this);
    if (endpoint == nullptr) {
      Serial.printf("bridge: endpoint %u could not be resumed, taking a new one\n", endpointId);
    }
  }
  if (endpoint == nullptr) {
    endpoint = bridged_node::create(node::get(), &config, flags, (void *)this);
  }
  if (endpoint == nullptr) {
    Serial.println("bridge: bridged node create failed");
    return nullptr;
  }
  endpoint::set_parent_endpoint(endpoint, s_aggregator);

  // Deliberately not create_node_label(): it forces ATTRIBUTE_FLAG_NONVOLATILE,
  // which costs 53 KB of internal DRAM per endpoint on esp_matter 1.5 and
  // starves the WiFi driver after three accessories. The names are persisted in
  // Preferences and reapplied on boot, so Matter does not need to store them.
  cluster_t *info = cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
  esp_matter::attribute::create(info, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
                                ATTRIBUTE_FLAG_WRITABLE, esp_matter_char_str(_name, sizeof(_name) - 1),
                                sizeof(_name) - 1);
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
  return TYPES[type < ACC_TYPE_COUNT ? type : ACC_BUTTON];
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
static void countContents(endpoint_t *endpoint, uint16_t *clusters, uint16_t *attributes) {
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
    Serial.printf("bridge: unknown device type %u\n", type);
    return false;
  }
  LiveEdit edit;
  endpoint_t *endpoint = createNode(name, endpointId);
  if (endpoint == nullptr) {
    return false;
  }

  _type = type;
  if (accessoryUi(type) == UI_VALUE && _value == 0) {
    _value = type == ACC_HUMIDITY ? 5000 : 2100;
  }
  if (addDeviceType(type, endpoint, _on, _level, _flag, _value) != ESP_OK) {
    Serial.printf("bridge: %s add failed\n", accessoryTypeName(type));
    return false;
  }
  if (!publish(endpoint, edit)) {
    return false;
  }

  setEndPointId(endpoint::get_id(endpoint));
  _started = true;
  uint16_t clusters = 0, attributes = 0;
  countContents(endpoint, &clusters, &attributes);
  Serial.printf("bridge: %s '%s' on endpoint %u (%u clusters, %u attributes)\n",
                accessoryTypeName(type), _name, getEndPointId(), clusters, attributes);
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
  LiveEdit edit;
  endpoint_t *endpoint = endpoint::get(node::get(), getEndPointId());
  if (endpoint == nullptr) {
    Serial.printf("bridge: endpoint %u not found\n", getEndPointId());
    return false;
  }
  esp_err_t err = endpoint::destroy(node::get(), endpoint);
  if (err != ESP_OK) {
    Serial.printf("bridge: endpoint destroy failed (%d)\n", err);
    return false;
  }
  Serial.printf("bridge: removed '%s' from endpoint %u\n", _name, getEndPointId());
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
  esp_matter_attr_val_t val = esp_matter_uint8(level);
  return updateAttributeVal(LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);
}

bool BridgedAccessory::attributeChangeCB(uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val) {
  if (endpoint_id != getEndPointId()) {
    return true;
  }
  if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
    _on = val->val.b;
    if (_onOffCB) {
      _onOffCB(_on);
    }
  } else if (cluster_id == LevelControl::Id &&
             attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
    _level = val->val.u8;
    if (_levelCB) {
      _levelCB(_level);
    }
  }
  return true;
}
