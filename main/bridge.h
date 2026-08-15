#pragma once

#include <esp_matter.h>

#include <cstddef>
#include <cstdint>
#include <functional>

// What Arduino's MatterEndPoint base class was, minus everything a bridge never
// used. An instance of this is the priv_data of any endpoint that wants to hear
// about writes: the node has exactly one attribute callback, and the endpoint's
// priv_data is all it gets to tell one endpoint from another.
class MatterEndPoint {
public:
  virtual ~MatterEndPoint() = default;

  uint16_t getEndPointId() const {
    return _endpointId;
  }
  void setEndPointId(uint16_t id) {
    _endpointId = id;
  }

  // Takes the stack lock when it needs one: attribute::update walks the data
  // model the CHIP task is walking too.
  bool updateAttributeVal(uint32_t clusterId, uint32_t attributeId, esp_matter_attr_val_t *val);

  // Runs on the CHIP task; do not touch the panel or anything else the ui task
  // owns.
  virtual void attributeChanged(uint32_t clusterId, uint32_t attributeId,
                                esp_matter_attr_val_t *val) = 0;

private:
  uint16_t _endpointId = 0;
};

// Accessories are exposed as bridged nodes under a Matter Aggregator rather
// than as plain endpoints on the root node. That is the only arrangement in
// which an ecosystem reads a per-accessory name (NodeLabel on the Bridged
// Device Basic Information cluster) instead of naming everything after its
// device type, and it is also what allows endpoints to come and go at runtime.
bool bridgeBegin();

// Brings the stack up. Endpoints that want to reclaim the id they held last
// time can only be built after this: resume() refuses any id the persisted
// min-unused-endpoint-id counter has not passed yet, and start() is what
// restores that counter from NVS.
bool bridgeStart();

// The aggregator every bridged accessory hangs from.
esp_matter::endpoint_t *bridgeAggregator();

// Registers an endpoint with the running stack. Endpoints built before
// esp_matter::start() are registered as it comes up; one built afterwards has
// to be enabled by hand.
bool bridgePublish(esp_matter::endpoint_t *endpoint);

// Counts what an endpoint is made of. Most of it is bookkeeping Matter puts on
// every cluster rather than anything the accessory uses.
void bridgeCount(esp_matter::endpoint_t *endpoint, uint16_t *clusters, uint16_t *attributes);

// True once the device holds at least one fabric.
bool bridgeCommissioned();

// The pairing screen's material. The payload is the raw "MT:..." string Apple
// Home scans; the code is the 11 digits underneath it.
bool bridgePairingPayload(char *out, size_t size);
bool bridgePairingCode(char *out, size_t size);

bool bridgePairingWindowOpen();
void bridgeOpenPairingWindow();

// The Matter device types this bridge can expose, in the order the picker
// offers them: the things you operate first, then the things that only report.
enum AccessoryType : uint8_t {
  ACC_BUTTON,
  ACC_LIGHT,
  ACC_DIMMER,
  ACC_OUTLET,
  ACC_CONTACT,
  ACC_MOTION,
  ACC_TEMPERATURE,
  ACC_HUMIDITY,
  ACC_TYPE_COUNT
};

// Several device types are the same handful of controls on screen, so the UI
// switches on this rather than on the device type itself.
enum AccessoryUi : uint8_t {
  UI_PRESS,  // stateless button
  UI_ONOFF,  // on and off
  UI_LEVEL,  // on, off and a brightness
  UI_FLAG,   // sensor reading one of two states
  UI_VALUE,  // sensor reading a number
};

AccessoryUi accessoryUi(uint8_t type);
const char *accessoryTypeName(uint8_t type);

// Sensor readings are hundredths, matching Matter's own scaling for both
// temperature (0.01 C) and relative humidity (0.01 %).
const char *accessoryUnit(uint8_t type);
int16_t accessoryStep(uint8_t type);
int16_t accessoryMin(uint8_t type);
int16_t accessoryMax(uint8_t type);

class BridgedAccessory : public MatterEndPoint {
public:
  // endpointId is the id this accessory held last time, or 0 for a new one.
  bool begin(uint8_t type, const char *name, uint16_t endpointId);

  // Destroys the Matter endpoint. The accessory goes inert; the slot it came
  // from is the caller's to free.
  bool remove();

  void setName(const char *name);
  const char *name() const {
    return _name;
  }

  void click();
  bool setOnOff(bool on);
  bool setBrightness(uint8_t level);
  bool getOnOff() const {
    return _on;
  }
  uint8_t getBrightness() const {
    return _level;
  }

  uint8_t type() const {
    return _type;
  }

  // UI_FLAG accessories only.
  bool setFlag(bool active);
  bool flag() const {
    return _flag;
  }

  // UI_VALUE accessories only, in hundredths.
  bool setValue(int16_t hundredths);
  int16_t value() const {
    return _value;
  }

  using OnOffCB = std::function<void(bool)>;
  using LevelCB = std::function<void(uint8_t)>;
  void onChangeOnOff(OnOffCB cb) {
    _onOffCB = cb;
  }
  void onChangeBrightness(LevelCB cb) {
    _levelCB = cb;
  }

  void attributeChanged(uint32_t clusterId, uint32_t attributeId,
                        esp_matter_attr_val_t *val) override;

private:
  esp_matter::endpoint_t *createNode(const char *name, uint16_t endpointId);

  char _name[33] = {0};
  uint8_t _type = ACC_BUTTON;
  bool _started = false;
  bool _on = false;
  uint8_t _level = 128;
  bool _flag = false;
  int16_t _value = 0;
  OnOffCB _onOffCB = nullptr;
  LevelCB _levelCB = nullptr;
};
