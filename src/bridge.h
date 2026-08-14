#pragma once

#include <Matter.h>
#include <MatterEndPoint.h>

#include <functional>

// Accessories are exposed as bridged nodes under a Matter Aggregator rather
// than as plain endpoints on the root node. That is the only arrangement in
// which an ecosystem reads a per-accessory name (NodeLabel on the Bridged
// Device Basic Information cluster) instead of naming everything after its
// device type, and it is also what allows endpoints to come and go at runtime.
bool bridgeBegin();

// Replaces Matter.begin(), which cannot be used once the library's own
// endpoint classes are out of the picture.
bool bridgeStart();

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

  bool attributeChangeCB(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
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
