#include "internals.h"

#include <Matter.h>
#include <MatterEndPoint.h>

#include "bridge.h"
#include "keys.h"
#include "panel.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *INTERNALS_NAME = "Genie";
static const uint32_t POLL_INTERVAL_MS = 30000;

// The panel is far brighter than a Matter level implies at the bottom of the
// range, and zero would look like a dead device rather than a dim one.
static const uint8_t MIN_BRIGHTNESS = 8;

namespace {

// The display, as a dimmable light. This one is not a simulation: a level from
// Home reaches the panel, so it needs to hear about writes, which means the
// endpoint's priv_data has to be a MatterEndPoint the bridge callback can call.
class Backlight : public MatterEndPoint {
public:
  bool attributeChangeCB(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                         esp_matter_attr_val_t *val) override {
    if (endpoint_id != getEndPointId()) {
      return true;
    }
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
      _on = val->val.b;
    } else if (cluster_id == LevelControl::Id &&
               attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
      _level = val->val.u8;
    } else {
      return true;
    }
    panelBrightness(_on ? (uint8_t)max(_level, MIN_BRIGHTNESS) : 0);
    Serial.printf("internals: backlight on=%d level=%u\n", _on ? 1 : 0, _level);
    return true;
  }

private:
  bool _on = true;
  uint8_t _level = 255;
};

Backlight s_backlight;
}  // namespace

static endpoint_t *s_parent = nullptr;
static uint16_t s_temperatureId = 0;
static uint16_t s_batteryId = 0;
static char s_label[33] = {0};

static void report(const char *what, endpoint_t *endpoint) {
  uint16_t clusters = 0, attributes = 0;
  bridgeCount(endpoint, &clusters, &attributes);
  Serial.printf("internals: %s on endpoint %u (%u clusters, %u attributes)\n", what,
                endpoint::get_id(endpoint), clusters, attributes);
}

// A child is a plain endpoint whose parent is the bridged node. That parentage
// is the whole mechanism: it puts the child in the parent's PartsList, which is
// how a controller learns the two are one accessory.
static endpoint_t *addChild(endpoint_t *endpoint, const char *what) {
  if (endpoint == nullptr) {
    Serial.printf("internals: %s create failed\n", what);
    return nullptr;
  }
  endpoint::set_parent_endpoint(endpoint, s_parent);
  if (!bridgePublish(endpoint)) {
    return nullptr;
  }
  report(what, endpoint);
  return endpoint;
}

void internalsBegin() {
  if (bridgeAggregator() == nullptr) {
    return;
  }

  bridged_node::config_t config;
  s_parent = bridged_node::create(node::get(), &config, ENDPOINT_FLAG_BRIDGE, nullptr);
  if (s_parent == nullptr) {
    Serial.println("internals: bridged node create failed");
    return;
  }
  endpoint::set_parent_endpoint(s_parent, bridgeAggregator());

  // Same reason as in bridge.cpp: create_node_label() would force
  // ATTRIBUTE_FLAG_NONVOLATILE and cost 53 KB of internal DRAM.
  strlcpy(s_label, INTERNALS_NAME, sizeof(s_label));
  cluster_t *info = cluster::get(s_parent, BridgedDeviceBasicInformation::Id);
  esp_matter::attribute::create(info, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
                                ATTRIBUTE_FLAG_WRITABLE,
                                esp_matter_char_str(s_label, sizeof(s_label) - 1),
                                sizeof(s_label) - 1);
  if (!bridgePublish(s_parent)) {
    return;
  }
  report("parent", s_parent);

  PmuStatus pmu = pmuStatus();

  temperature_sensor::config_t temperature;
  temperature.temperature_measurement.measured_value =
      nullable<int16_t>((int16_t)(pmu.celsius * 100));
  endpoint_t *child =
      addChild(temperature_sensor::create(node::get(), &temperature, ENDPOINT_FLAG_NONE, nullptr),
               "die temperature");
  s_temperatureId = child != nullptr ? endpoint::get_id(child) : 0;

  // Battery as a humidity reading is a lie, but Matter's Power Source cluster
  // is not a device type Home draws on its own, and a percentage needs
  // somewhere to live. Revisit if Home turns out to read Power Source.
  humidity_sensor::config_t battery;
  battery.relative_humidity_measurement.measured_value =
      nullable<uint16_t>((uint16_t)(pmu.percent * 100));
  child = addChild(humidity_sensor::create(node::get(), &battery, ENDPOINT_FLAG_NONE, nullptr),
                   "battery percent");
  s_batteryId = child != nullptr ? endpoint::get_id(child) : 0;

  dimmable_light::config_t backlight;
  backlight.on_off.on_off = true;
  backlight.on_off_lighting.start_up_on_off = nullptr;
  backlight.level_control.current_level = 255;
  backlight.level_control_lighting.start_up_current_level = nullptr;
  child = addChild(
      dimmable_light::create(node::get(), &backlight, ENDPOINT_FLAG_NONE, (void *)&s_backlight),
      "display brightness");
  if (child != nullptr) {
    s_backlight.setEndPointId(endpoint::get_id(child));
  }
}

static void publishValue(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId,
                         esp_matter_attr_val_t val) {
  if (endpointId == 0) {
    return;
  }
  attribute::update(endpointId, clusterId, attributeId, &val);
}

void internalsPoll() {
  static uint32_t lastAt = 0;
  if (s_parent == nullptr || (lastAt != 0 && millis() - lastAt < POLL_INTERVAL_MS)) {
    return;
  }
  lastAt = millis();

  PmuStatus pmu = pmuStatus();
  if (!pmu.present) {
    return;
  }
  publishValue(s_temperatureId, TemperatureMeasurement::Id,
               TemperatureMeasurement::Attributes::MeasuredValue::Id,
               esp_matter_nullable_int16((int16_t)(pmu.celsius * 100)));
  publishValue(s_batteryId, RelativeHumidityMeasurement::Id,
               RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
               esp_matter_nullable_uint16((uint16_t)(pmu.percent * 100)));
}
