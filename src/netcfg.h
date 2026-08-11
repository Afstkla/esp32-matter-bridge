#pragma once

#include <Arduino.h>

// These Arduino libs are built without CONFIG_ENABLE_CHIPOBLE, so there is no
// BLE commissioning transport and a commissioner can only find the device over
// mDNS. It therefore has to be on WiFi *before* the Matter stack starts, which
// means credentials must already be stored rather than handed over during
// pairing. They live in NVS so they never enter the source tree.
bool netcfgJoin(uint32_t timeoutMs);

void netcfgSetSsid(const String &ssid);
void netcfgSetPassword(const String &password);
void netcfgForget();
String netcfgSsid();
