#include "internals.h"

#include <cstdio>
#include <cstring>

#include <app-common/zap-generated/cluster-enums.h>
#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bridge.h"
#include "keys.h"
#include "panel.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "internals";

// Not const: the attribute helpers take a char *, and the strings Home shows as
// the accessory's manufacturer and model live here rather than in the root
// node's Basic Information, which only ever answers for the bridge itself.
static char INTERNALS_NAME[] = "Genie";
static char VENDOR_NAME[] = "Afstkla";
static const char *UNIQUE_ID = "genie-internals";
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
  uint16_t backlight;
};

// What the blob looked like while the battery was a humidity child. The device
// is commissioned and cannot be reset, so its stored blob is still this shape
// on the first boot after the change, and the backlight id has to come out of
// it unchanged — a backlight that renumbers is a new accessory to Home.
struct IdsV1 {
  uint16_t parent;
  uint16_t temperature;
  uint16_t battery;
  uint16_t backlight;
};

static const char *IDS_NAMESPACE = "internals";
static const char *IDS_KEY = "eps";

static endpoint_t *s_parent = nullptr;
static Ids s_ids = {0, 0, 0};
static nvs_handle_t s_nvs = 0;
static char s_label[33] = {0};
static char s_serial[13] = {0};

static void persistIds() {
  nvs_set_blob(s_nvs, IDS_KEY, &s_ids, sizeof(s_ids));
  nvs_commit(s_nvs);
}

static void loadIds() {
  s_ids = {0, 0, 0};
  size_t stored = 0;
  if (nvs_get_blob(s_nvs, IDS_KEY, nullptr, &stored) != ESP_OK) {
    return;
  }
  if (stored == sizeof(Ids)) {
    nvs_get_blob(s_nvs, IDS_KEY, &s_ids, &stored);
    return;
  }
  IdsV1 v1 = {0, 0, 0, 0};
  if (stored == sizeof(v1) && nvs_get_blob(s_nvs, IDS_KEY, &v1, &stored) == ESP_OK) {
    s_ids = {v1.parent, v1.temperature, v1.backlight};
  }
}

// Reclaims the id if there is one to reclaim. Same rule as bridge.cpp: resume()
// only works once esp_matter::start() has restored its id counter from NVS.
//
// A bare endpoint has no Descriptor cluster: <type>::create() is what adds one,
// and this builds the endpoint by hand so that the id can be reclaimed. Every
// endpoint shall have a Descriptor (Matter core spec 9.5), and without it a
// controller walking the parent's PartsList cannot read what the parts are.
static endpoint_t *makeChild(uint16_t wanted, void *priv) {
  endpoint_t *endpoint = nullptr;
  if (wanted != 0) {
    endpoint = endpoint::resume(node::get(), ENDPOINT_FLAG_NONE, wanted, priv);
  }
  if (endpoint == nullptr) {
    endpoint = endpoint::create(node::get(), ENDPOINT_FLAG_NONE, priv);
  }
  if (endpoint == nullptr) {
    return nullptr;
  }
  cluster::descriptor::config_t descriptor;
  if (cluster::descriptor::create(endpoint, &descriptor, CLUSTER_FLAG_SERVER) == nullptr) {
    ESP_LOGE(TAG, "descriptor create failed on endpoint %u", endpoint::get_id(endpoint));
    return nullptr;
  }
  return endpoint;
}

// BatPercentRemaining is in half-percents, so a full cell reads 200.
static uint8_t batteryHalfPercent(uint8_t percent) {
  return percent >= 100 ? 200 : (uint8_t)(percent * 2);
}

// Cuts on the OCV-derived percent, which reads high on USB and low under load
// (see the battery section of the README), so they sit well clear of the knee
// where a LiPo's voltage falls off. Anything but Ok makes Home shout, and a
// pack that only looks empty because the panel is drawing should not do that.
static uint8_t batteryChargeLevel(uint8_t percent) {
  if (percent <= 7) {
    return (uint8_t)PowerSource::BatChargeLevelEnum::kCritical;
  }
  if (percent <= 20) {
    return (uint8_t)PowerSource::BatChargeLevelEnum::kWarning;
  }
  return (uint8_t)PowerSource::BatChargeLevelEnum::kOk;
}

// The battery lives on the parent, not on a child of its own: Bridged Node
// carries Power Source as an optional server cluster, which is how a bridge
// says "this accessory runs on a battery" rather than inventing an endpoint.
//
// The Battery feature is what makes BatChargeLevel, BatReplacementNeeded and
// BatReplaceability mandatory; with Status, Order, Description and
// EndpointList — all four created by cluster::power_source::create — that is
// the cluster's complete mandatory set for this feature map, and a missing one
// is how an accessory ends up unsupported. BatPercentRemaining is optional and
// is the only number Home actually draws, so it is added by hand.
static void addBattery(endpoint_t *parent, const PmuStatus &pmu) {
  cluster::power_source::config_t config;
  config.feature_flags = cluster::power_source::feature::battery::get_id();
  config.status = (uint8_t)PowerSource::PowerSourceStatusEnum::kActive;
  strlcpy(config.description, "Battery", sizeof(config.description));
  config.features.battery.bat_charge_level = batteryChargeLevel(pmu.percent);
  // Not UserReplaceable: the Replaceable feature would come with it and bring
  // its own mandatory attributes describing a cell nobody is meant to swap.
  config.features.battery.bat_replaceability =
      (uint8_t)PowerSource::BatReplaceabilityEnum::kNotReplaceable;

  cluster_t *cluster = cluster::power_source::create(parent, &config, CLUSTER_FLAG_SERVER);
  if (cluster == nullptr) {
    ESP_LOGE(TAG, "power source create failed");
    return;
  }
  cluster::power_source::attribute::create_bat_percent_remaining(
      cluster, nullable<uint8_t>(batteryHalfPercent(pmu.percent)), nullable<uint8_t>(0),
      nullable<uint8_t>(200));
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
  strlcpy(config.bridged_device_basic_information.unique_id, UNIQUE_ID,
          sizeof(config.bridged_device_basic_information.unique_id));
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

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  namespace identity = esp_matter::cluster::bridged_device_basic_information::attribute;
  identity::create_vendor_name(info, VENDOR_NAME, strlen(VENDOR_NAME));
  identity::create_product_name(info, INTERNALS_NAME, strlen(INTERNALS_NAME));
  identity::create_serial_number(info, s_serial, strlen(s_serial));

  PmuStatus pmu = pmuStatus();
  addBattery(s_parent, pmu);
  if (!bridgePublish(s_parent)) {
    return;
  }
  report("parent", s_parent);

  s_ids.parent = endpoint::get_id(s_parent);

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

static void publishValue(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId,
                         esp_matter_attr_val_t val) {
  if (endpointId == 0) {
    return;
  }
  bridgeUpdateValue(endpointId, clusterId, attributeId, &val);
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
  publishValue(s_ids.parent, PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id,
               esp_matter_nullable_uint8(batteryHalfPercent(pmu.percent)));
  publishValue(s_ids.parent, PowerSource::Id, PowerSource::Attributes::BatChargeLevel::Id,
               esp_matter_enum8(batteryChargeLevel(pmu.percent)));
}
