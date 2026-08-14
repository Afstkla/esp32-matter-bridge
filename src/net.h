#pragma once

#include <Arduino.h>

// Read from the WiFi driver rather than the Arduino WiFi object, which only
// tracks connections it made itself. A commissioner provisions the network over
// BLE and brings the station up underneath Arduino, leaving WiFi.status()
// disconnected and WiFi.SSID() empty on a device that is demonstrably online.
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
