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

class BridgedAccessory : public MatterEndPoint {
public:
  bool beginSwitch(const char *name);
  bool beginLight(const char *name, bool on, uint8_t level);

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
  esp_matter::endpoint_t *createNode(const char *name);

  char _name[33] = {0};
  bool _started = false;
  bool _on = false;
  uint8_t _level = 128;
  OnOffCB _onOffCB = nullptr;
  LevelCB _levelCB = nullptr;
};
