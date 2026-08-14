#pragma once

#include <cstdint>

// Read from the WiFi driver rather than from anything that tracks connections
// it made itself: a commissioner provisions the network over BLE and brings the
// station up underneath us, so the only truthful source is the driver.
//
// Nothing here stores credentials. Matter keeps its own and rejoins with them
// at boot, so a second copy would only be a second thing to get out of step.
struct NetStatus {
  bool connected;
  char ssid[33];
  int8_t rssi;
  uint8_t channel;
  char ip[16];
  char linkLocal[46];
};

NetStatus netStatus();
