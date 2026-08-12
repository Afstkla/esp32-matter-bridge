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

bool bridgeBegin() {
  node::config_t node_config;
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

// The name buffer is a member rather than a local because the attribute keeps
// referring to it, and it is sized once here so later renames always fit.
endpoint_t *BridgedAccessory::createNode(const char *name) {
  if (s_aggregator == nullptr) {
    Serial.println("bridge: begin the aggregator first");
    return nullptr;
  }
  strlcpy(_name, name, sizeof(_name));

  bridged_node::config_t config;
  endpoint_t *endpoint = bridged_node::create(node::get(), &config,
                                              ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE,
                                              (void *)this);
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

bool BridgedAccessory::beginSwitch(const char *name) {
  endpoint_t *endpoint = createNode(name);
  if (endpoint == nullptr) {
    return false;
  }

  generic_switch::config_t config;
  // The cluster aborts creation unless exactly one of the latching/momentary
  // features is already declared, so this cannot be added afterwards. Declaring
  // it also brings the position attributes and the InitialPress event.
  config.switch_cluster.feature_flags = cluster::switch_cluster::feature::momentary_switch::get_id();
  if (generic_switch::add(endpoint, &config) != ESP_OK) {
    Serial.println("bridge: generic_switch add failed");
    return false;
  }

  setEndPointId(endpoint::get_id(endpoint));
  _started = true;
  Serial.printf("bridge: switch '%s' on endpoint %u\n", _name, getEndPointId());
  return true;
}

bool BridgedAccessory::beginLight(const char *name, bool on, uint8_t level) {
  endpoint_t *endpoint = createNode(name);
  if (endpoint == nullptr) {
    return false;
  }

  dimmable_light::config_t config;
  config.on_off.on_off = on;
  config.on_off_lighting.start_up_on_off = nullptr;
  config.level_control.current_level = level;
  config.level_control_lighting.start_up_current_level = nullptr;
  if (dimmable_light::add(endpoint, &config) != ESP_OK) {
    Serial.println("bridge: dimmable_light add failed");
    return false;
  }

  _on = on;
  _level = level;
  setEndPointId(endpoint::get_id(endpoint));
  _started = true;
  Serial.printf("bridge: light '%s' on endpoint %u\n", _name, getEndPointId());
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
