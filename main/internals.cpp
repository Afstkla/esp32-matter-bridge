#include "internals.h"

#include <cstdio>
#include <cstring>

#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bridge.h"
#include "keys.h"
#include "panel.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "internals";

static const char *INTERNALS_NAME = "Genie";
static const uint32_t POLL_INTERVAL_MS = 30000;

// The panel is far brighter than a Matter level implies at the bottom of the
// range, and zero would look like a dead device rather than a dim one.
static const uint8_t MIN_BRIGHTNESS = 8;

namespace {

// The display, as a dimmable light. This one is not a simulation: a level from
// Home reaches the panel, so it needs to hear about writes, which means the
// endpoint's priv_data has to be a MatterEndPoint the bridge callback can call.
//
// The callback records the wanted state and returns; apply() does the work.
// attributeChanged() runs on the CHIP task, and driving the panel from there
// would put a QSPI command in the middle of the DMA transfer the ui task is
// running on the same bus.
class Backlight : public MatterEndPoint {
public:
  void attributeChanged(uint32_t clusterId, uint32_t attributeId,
                        esp_matter_attr_val_t *val) override {
    if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
      _on = val->val.b;
    } else if (clusterId == LevelControl::Id &&
               attributeId == LevelControl::Attributes::CurrentLevel::Id) {
      _level = val->val.u8;
    } else {
      return;
    }
    _wanted = true;
  }

  void apply() {
    if (!_wanted || panelAsleep()) {
      return;
    }
    _wanted = false;
    uint8_t level = _level;
    panelBrightness(_on ? (level > MIN_BRIGHTNESS ? level : MIN_BRIGHTNESS) : 0);
    printf("internals: backlight on=%d level=%u\n", _on ? 1 : 0, level);
  }

private:
  volatile bool _wanted = false;
  volatile bool _on = true;
  volatile uint8_t _level = 255;
};

Backlight s_backlight;
}  // namespace

// Endpoint ids are persisted for the same reason the accessories persist
// theirs: an id that moves is a new accessory to a controller, so a reboot
// would leave the old one behind in Home and add a fresh one beside it.
struct Ids {
  uint16_t parent;
  uint16_t temperature;
  uint16_t battery;
  uint16_t backlight;
};

static const char *IDS_NAMESPACE = "internals";
static const char *IDS_KEY = "eps";

static endpoint_t *s_parent = nullptr;
static Ids s_ids = {0, 0, 0, 0};
static nvs_handle_t s_nvs = 0;
static char s_label[33] = {0};

static void persistIds() {
  nvs_set_blob(s_nvs, IDS_KEY, &s_ids, sizeof(s_ids));
  nvs_commit(s_nvs);
}

static void loadIds() {
  size_t stored = sizeof(s_ids);
  if (nvs_get_blob(s_nvs, IDS_KEY, &s_ids, &stored) != ESP_OK || stored != sizeof(s_ids)) {
    s_ids = {0, 0, 0, 0};
  }
}

// Reclaims the id if there is one to reclaim. Same rule as bridge.cpp: resume()
// only works once esp_matter::start() has restored its id counter from NVS.
static endpoint_t *makeChild(uint16_t wanted, void *priv) {
  endpoint_t *endpoint = nullptr;
  if (wanted != 0) {
    endpoint = endpoint::resume(node::get(), ENDPOINT_FLAG_NONE, wanted, priv);
  }
  return endpoint != nullptr ? endpoint : endpoint::create(node::get(), ENDPOINT_FLAG_NONE, priv);
}

static void report(const char *what, endpoint_t *endpoint) {
  uint16_t clusters = 0, attributes = 0;
  bridgeCount(endpoint, &clusters, &attributes);
  ESP_LOGI(TAG, "%s on endpoint %u (%u clusters, %u attributes)", what,
           endpoint::get_id(endpoint), clusters, attributes);
}

// A child is a plain endpoint whose parent is the bridged node. That parentage
// is the whole mechanism: it puts the child in the parent's PartsList, which is
// how a controller learns the two are one accessory.
//
// Built as endpoint::create() plus <type>::add() rather than <type>::create(),
// so the id can be reclaimed. That combination leaves out the Identify cluster
// the all-in-one call brings: a child comes up with 2 clusters where the same
// device type as a standalone accessory has 3. Calling identify::create() by
// hand did not put it back and it is not yet understood why. Nothing here uses
// Identify, so this is a conformance gap rather than a broken feature.
static endpoint_t *addChild(endpoint_t *endpoint, const char *what) {
  if (endpoint == nullptr) {
    ESP_LOGE(TAG, "%s create failed", what);
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

  ESP_ERROR_CHECK(nvs_open(IDS_NAMESPACE, NVS_READWRITE, &s_nvs));
  loadIds();

  bridged_node::config_t config;
  if (s_ids.parent != 0) {
    s_parent =
        bridged_node::resume(node::get(), &config, ENDPOINT_FLAG_BRIDGE, s_ids.parent, nullptr);
  }
  if (s_parent == nullptr) {
    s_parent = bridged_node::create(node::get(), &config, ENDPOINT_FLAG_BRIDGE, nullptr);
  }
  if (s_parent == nullptr) {
    ESP_LOGE(TAG, "bridged node create failed");
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

  s_ids.parent = endpoint::get_id(s_parent);
  PmuStatus pmu = pmuStatus();

  temperature_sensor::config_t temperature;
  temperature.temperature_measurement.measured_value =
      nullable<int16_t>((int16_t)(pmu.celsius * 100));
  endpoint_t *child = makeChild(s_ids.temperature, nullptr);
  if (child != nullptr && temperature_sensor::add(child, &temperature) == ESP_OK) {
    child = addChild(child, "die temperature");
  } else {
    child = nullptr;
  }
  s_ids.temperature = child != nullptr ? endpoint::get_id(child) : 0;

  // Battery as a humidity reading is a lie, but Matter's Power Source cluster
  // is not a device type Home draws on its own, and a percentage needs
  // somewhere to live. Revisit if Home turns out to read Power Source.
  humidity_sensor::config_t battery;
  battery.relative_humidity_measurement.measured_value =
      nullable<uint16_t>((uint16_t)(pmu.percent * 100));
  child = makeChild(s_ids.battery, nullptr);
  if (child != nullptr && humidity_sensor::add(child, &battery) == ESP_OK) {
    child = addChild(child, "battery percent");
  } else {
    child = nullptr;
  }
  s_ids.battery = child != nullptr ? endpoint::get_id(child) : 0;

  dimmable_light::config_t backlight;
  backlight.on_off.on_off = true;
  backlight.on_off_lighting.start_up_on_off = nullptr;
  backlight.level_control.current_level = 255;
  backlight.level_control_lighting.start_up_current_level = nullptr;
  child = makeChild(s_ids.backlight, (void *)&s_backlight);
  if (child != nullptr && dimmable_light::add(child, &backlight) == ESP_OK) {
    child = addChild(child, "display brightness");
  } else {
    child = nullptr;
  }
  s_ids.backlight = child != nullptr ? endpoint::get_id(child) : 0;
  if (child != nullptr) {
    s_backlight.setEndPointId(s_ids.backlight);
  }
  persistIds();
}

// Called from the ui task, so the stack lock is this end's job:
// attribute::update() walks the data model, and the CHIP task is walking it too.
static void publishValue(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId,
                         esp_matter_attr_val_t val) {
  if (endpointId == 0) {
    return;
  }
  StackLock lock;
  attribute::update(endpointId, clusterId, attributeId, &val);
}

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void internalsPoll() {
  static uint32_t lastAt = 0;
  if (s_parent == nullptr) {
    return;
  }
  s_backlight.apply();
  if (lastAt != 0 && nowMs() - lastAt < POLL_INTERVAL_MS) {
    return;
  }
  lastAt = nowMs();

  PmuStatus pmu = pmuStatus();
  if (!pmu.present) {
    return;
  }
  publishValue(s_ids.temperature, TemperatureMeasurement::Id,
               TemperatureMeasurement::Attributes::MeasuredValue::Id,
               esp_matter_nullable_int16((int16_t)(pmu.celsius * 100)));
  publishValue(s_ids.battery, RelativeHumidityMeasurement::Id,
               RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
               esp_matter_nullable_uint16((uint16_t)(pmu.percent * 100)));
}
