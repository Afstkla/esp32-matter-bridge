#include "internals.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <app-common/zap-generated/cluster-enums.h>
#include <app/clusters/power-source-server/power-source-server.h>
#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"

#include "audio.h"
#include "bridge.h"
#include "console.h"
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

// Long enough to walk the house with, short enough that a session forgotten
// under a cushion stops on its own rather than running the cell down.
static const uint32_t FINDER_TIMEOUT_MS = 10 * 60 * 1000;

// What Home's "identify accessory" button is worth: audible, and over before
// anyone reaches for a way to stop it.
static const uint32_t FINDER_BURST_MS = 5000;

static const uint32_t FINDER_PULSE_MS = 500;

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

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

  // Asks for the level to be written again. The finder borrows the panel to
  // flash with and this is how it gives it back, without having to know what it
  // borrowed it from.
  void refresh() {
    _wanted = true;
  }

private:
  volatile bool _wanted = false;
  volatile bool _on = true;
  volatile uint8_t _level = 255;
};

Backlight s_backlight;

// Beeps until something stops it, so a Genie down the back of the sofa can be
// found. Matter drives it by writing this endpoint's OnOff attribute; every
// other way a session ends — a touch, a button, the timeout — writes that same
// attribute back, so Home's tile is never the only thing that thinks it is on.
//
// attributeChanged() runs on the CHIP task and does nothing but record intent.
// apply() is called from the ui tick, which is the only place the panel may be
// touched and a fine place to call the audio task from.
class Finder : public MatterEndPoint {
public:
  void attributeChanged(uint32_t clusterId, uint32_t attributeId,
                        esp_matter_attr_val_t *val) override {
    if (clusterId != OnOff::Id || attributeId != OnOff::Attributes::OnOff::Id) {
      return;
    }
    _holdMs = FINDER_TIMEOUT_MS;
    _wanted = val->val.b;
  }

  void stop() {
    _wanted = false;
  }

  void burst() {
    _holdMs = FINDER_BURST_MS;
    _wanted = true;
  }

  bool finding() const {
    return _running;
  }

  void apply() {
    uint32_t now = nowMs();
    if (_running && (int32_t)(now - _endsAt) >= 0) {
      _wanted = false;
    }
    if (_wanted != _running) {
      _running = _wanted;
      _endsAt = now + _holdMs;
      audioBeep(_running);
      publish(_running);
      printf("FINDER %s\n", _running ? "on" : "off");
      if (!_running && _bright >= 0) {
        _bright = -1;
        s_backlight.refresh();
      }
    }
    if (_running) {
      pulse(now);
    }
  }

  // Also the boot state: OnOff is a nonvolatile attribute, so without this a
  // reboot mid-session would leave Home showing a tile that is on above a
  // device that is silent.
  void publish(bool on) {
    esp_matter_attr_val_t val = esp_matter_bool(on);
    bridgeUpdateValue(getEndPointId(), OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
  }

private:
  // The screen is the second half of finding the thing: a 2 kHz beep says which
  // room, a flashing AMOLED says which cushion. Only while it is already awake
  // — see internalsFinding() for who wakes it.
  void pulse(uint32_t now) {
    if (panelAsleep()) {
      return;
    }
    int8_t bright = (now / FINDER_PULSE_MS) % 2 == 0 ? 1 : 0;
    if (bright == _bright) {
      return;
    }
    _bright = bright;
    panelBrightness(bright ? 255 : MIN_BRIGHTNESS);
  }

  volatile bool _wanted = false;
  volatile uint32_t _holdMs = FINDER_TIMEOUT_MS;
  bool _running = false;
  uint32_t _endsAt = 0;
  int8_t _bright = -1;
};

Finder s_finder;
}  // namespace

// Endpoint ids are persisted for the same reason the accessories persist
// theirs: an id that moves is a new accessory to a controller, so a reboot
// would leave the old one behind in Home and add a fresh one beside it.
struct Ids {
  uint16_t parent;
  uint16_t temperature;
  uint16_t backlight;
  uint16_t finder;
};

// What the blob looked like before the finder. The device is commissioned and
// cannot be reset, so its stored blob is still this shape on the first boot
// after the change, and every id in it has to come out unchanged — a child that
// renumbers is a new accessory to Home.
//
// The shape before that one (parent, temperature, battery, backlight, from when
// the battery was a child of its own) is deliberately not handled: it is the
// same eight bytes as the struct above, so the two cannot be told apart, and
// reading one as the other would renumber the backlight. That shape has not
// existed since the battery moved onto the parent — the device rewrote its blob
// on the first boot of that firmware, long before this one.
struct IdsV2 {
  uint16_t parent;
  uint16_t temperature;
  uint16_t backlight;
};

static const char *IDS_NAMESPACE = "internals";
static const char *IDS_KEY = "eps";

static endpoint_t *s_parent = nullptr;
static Ids s_ids = {};
static nvs_handle_t s_nvs = 0;
static char s_label[33] = {0};
static char s_serial[13] = {0};

static void persistIds() {
  nvs_set_blob(s_nvs, IDS_KEY, &s_ids, sizeof(s_ids));
  nvs_commit(s_nvs);
}

// Logged rather than silent because it is a one-way door: the next persistIds()
// overwrites whatever shape was there, and an id that came out wrong renumbers
// a child that Home has already learnt. The byte count is the whole decision —
// reading it in the boot log is the only chance to catch a blob this code did
// not expect before Home resyncs against the result.
static void loadIds() {
  s_ids = {};
  size_t stored = 0;
  if (nvs_get_blob(s_nvs, IDS_KEY, nullptr, &stored) == ESP_OK) {
    if (stored == sizeof(Ids)) {
      nvs_get_blob(s_nvs, IDS_KEY, &s_ids, &stored);
    } else {
      IdsV2 v2 = {};
      if (stored == sizeof(v2) && nvs_get_blob(s_nvs, IDS_KEY, &v2, &stored) == ESP_OK) {
        s_ids = {v2.parent, v2.temperature, v2.backlight, 0};
      }
    }
  }
  ESP_LOGI(TAG, "stored ids: %u bytes -> parent %u, temperature %u, backlight %u, finder %u",
           (unsigned)stored, s_ids.parent, s_ids.temperature, s_ids.backlight, s_ids.finder);
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

// The battery lives on the parent, not on a child of its own. Core spec
// 9.12.2.3: a bridged device whose whole self runs off one source SHALL carry
// the cluster "on the endpoint where the Bridged Node device type is located",
// and every endpoint carrying the cluster "SHALL have the related Power Source
// device type in its DeviceTypeList" — so the parent answers for two device
// types, 0x0013 and 0x0011. endpoint::power_source::add does both halves.
//
// The Battery feature is what makes BatChargeLevel, BatReplacementNeeded and
// BatReplaceability mandatory; with Status, Order, Description and
// EndpointList — all four created by cluster::power_source::create — that is
// the cluster's complete mandatory set for this feature map, and a missing one
// is how an accessory ends up unsupported. BatPercentRemaining is optional and
// is the only number Home actually draws, so it is added by hand.
static void addBattery(endpoint_t *parent, const PmuStatus &pmu) {
  endpoint::power_source::config_t config;
  cluster::power_source::config_t &source = config.power_source;
  source.feature_flags = cluster::power_source::feature::battery::get_id();
  source.status = (uint8_t)PowerSource::PowerSourceStatusEnum::kActive;
  strlcpy(source.description, "Battery", sizeof(source.description));
  source.features.battery.bat_charge_level = batteryChargeLevel(pmu.percent);
  // Not UserReplaceable: the Replaceable feature would come with it and bring
  // its own mandatory attributes describing a cell nobody is meant to swap.
  source.features.battery.bat_replaceability =
      (uint8_t)PowerSource::BatReplaceabilityEnum::kNotReplaceable;

  if (endpoint::power_source::add(parent, &config) != ESP_OK) {
    ESP_LOGE(TAG, "power source add failed");
    return;
  }
  cluster_t *cluster = cluster::get(parent, PowerSource::Id);
  if (cluster == nullptr) {
    ESP_LOGE(TAG, "power source cluster missing after add");
    return;
  }
  cluster::power_source::attribute::create_bat_percent_remaining(
      cluster, nullable<uint8_t>(batteryHalfPercent(pmu.percent)), nullable<uint8_t>(0),
      nullable<uint8_t>(200));
}

// EndpointList is not optional and empty is not neutral: spec 11.7.7.32 says
// an empty list SHALL mean the source powers the entire node, which on a
// bridge would claim this cell also runs every simulated accessory. 9.12.2.3
// wants "all the endpoints constituting the Bridged Device", and 11.7.7.32
// wants the endpoint the cluster sits on to appear in its own list, so the
// parent is in there alongside its children.
//
// Written once, after the children are numbered and the parent is enabled —
// both are preconditions. The set never changes afterwards: these endpoints
// are built at boot from the persisted ids and none is added at runtime.
//
// The list is MANAGED_INTERNALLY, so attribute::update cannot reach it; the
// AAI answers it from PowerSourceServer, which copies the span.
static void linkBatteryEndpoints() {
  chip::EndpointId powered[4] = {};
  size_t count = 0;
  for (uint16_t id : {s_ids.parent, s_ids.temperature, s_ids.backlight, s_ids.finder}) {
    if (id != 0) {
      powered[count++] = id;
    }
  }
  StackLock lock;
  if (PowerSourceServer::Instance().SetEndpointList(
          s_ids.parent, chip::Span<chip::EndpointId>(powered, count)) != CHIP_NO_ERROR) {
    ESP_LOGE(TAG, "power source endpoint list failed");
    return;
  }
  // What a controller is handed, not what esp_matter stored: the AAI answers
  // ClusterRevision from CHIP's own constant, so a submodule bump that moves
  // one and not the other shows up here rather than silently.
  esp_matter_attr_val_t revision = esp_matter_invalid(nullptr);
  attribute_t *attribute = attribute::get(s_ids.parent, PowerSource::Id,
                                          Globals::Attributes::ClusterRevision::Id);
  if (attribute::get_val(attribute, &revision) == ESP_OK) {
    ESP_LOGI(TAG, "power source on endpoint %u serves revision %u, %u powered endpoints",
             s_ids.parent, (unsigned)revision.val.u16, (unsigned)count);
  }
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

// on and off write the attribute rather than calling the object, so the console
// takes the same path a controller does — callback, intent, ui tick — and the
// round trip is testable without a commissioner.
static int cmdFinder(int argc, char **argv) {
  if (argc == 2 && strcasecmp(argv[1], "identify") == 0) {
    s_finder.burst();
  } else if (argc == 2 && strcasecmp(argv[1], "on") == 0) {
    s_finder.publish(true);
  } else if (argc == 2 && strcasecmp(argv[1], "off") == 0) {
    s_finder.publish(false);
  } else if (argc != 1) {
    printf("ERR usage: finder [on|off|identify]\n");
    return 1;
  }
  printf("FINDER endpoint=%u %s\n", s_ids.finder, s_finder.finding() ? "on" : "off");
  return 0;
}

void internalsFinderStop() {
  s_finder.stop();
}

bool internalsFinding() {
  return s_finder.finding();
}

static bool isOurs(uint16_t endpointId) {
  for (uint16_t id : {s_ids.parent, s_ids.temperature, s_ids.backlight, s_ids.finder}) {
    if (id != 0 && id == endpointId) {
      return true;
    }
  }
  return false;
}

void internalsIdentify(uint16_t endpointId) {
  if (isOurs(endpointId)) {
    s_finder.burst();
  }
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

  // bridged_node::add creates only the Bridged Device Basic Information
  // cluster, so the accessory a controller sees has nowhere to send an Identify
  // — and Identify on the endpoint carrying the name is what an ecosystem's
  // "identify this accessory" button aims at. kAudibleBeep says what will
  // happen: internalsIdentify() turns it into a short finder burst.
  cluster::identify::config_t identify;
  identify.identify_type = (uint8_t)Identify::IdentifyTypeEnum::kAudibleBeep;
  cluster_t *identifyCluster = cluster::identify::create(s_parent, &identify, CLUSTER_FLAG_SERVER);
  if (identifyCluster == nullptr) {
    ESP_LOGE(TAG, "identify create failed on the parent");
  }

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

  // A plug-in unit rather than a light: Home draws it as a switch you turn on
  // and off, which is what it is, and nothing about it invites a brightness.
  on_off_plug_in_unit::config_t finder;
  finder.on_off.on_off = false;
  finder.on_off_lighting.start_up_on_off = nullptr;
  finder.identify.identify_type = (uint8_t)Identify::IdentifyTypeEnum::kAudibleBeep;
  child = makeChild(s_ids.finder, (void *)&s_finder);
  if (child != nullptr && on_off_plug_in_unit::add(child, &finder) == ESP_OK) {
    child = addChild(child, "finder");
  } else {
    child = nullptr;
  }
  s_ids.finder = child != nullptr ? endpoint::get_id(child) : 0;
  if (child != nullptr) {
    s_finder.setEndPointId(s_ids.finder);
    s_finder.publish(false);
  }

  persistIds();
  linkBatteryEndpoints();
  consoleRegisterCmd("finder", "Beep until stopped: finder [on|off|identify]", cmdFinder);
}

static void publishValue(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId,
                         esp_matter_attr_val_t val) {
  if (endpointId == 0) {
    return;
  }
  bridgeUpdateValue(endpointId, clusterId, attributeId, &val);
}

void internalsPoll() {
  static uint32_t lastAt = 0;
  if (s_parent == nullptr) {
    return;
  }
  s_backlight.apply();
  s_finder.apply();
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
