#include "netcfg.h"

#include <Preferences.h>
#include <WiFi.h>

static Preferences s_prefs;

static Preferences &prefs() {
  static bool opened = false;
  if (!opened) {
    s_prefs.begin("net", false);
    opened = true;
  }
  return s_prefs;
}

String netcfgSsid() {
  return prefs().getString("ssid", "");
}

bool netcfgJoin(uint32_t timeoutMs) {
  String ssid = netcfgSsid();
  if (!ssid.length()) {
    Serial.println("WIFI none stored - send: ssid <name> then pass <key>");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.enableIPv6(true);
  WiFi.begin(ssid.c_str(), prefs().getString("pass", "").c_str());

  // Waiting here holds up the whole boot, and nothing downstream needs the link
  // to be up: the Matter stack drives its own reconnection and republishes mDNS
  // when the interface arrives. The screen reports the state as it changes.
  if (timeoutMs == 0) {
    Serial.printf("WIFI joining '%s' in the background\n", ssid.c_str());
    return false;
  }

  uint32_t until = millis() + timeoutMs;
  while (millis() < until && WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WIFI join failed for '%s' (2.4GHz only) status=%d\n", ssid.c_str(),
                  (int)WiFi.status());
    return false;
  }
  Serial.printf("WIFI joined '%s' ip=%s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
  return true;
}

// Set in two steps rather than one "wifi <ssid> <key>" command because both an
// SSID and a passphrase may legitimately contain spaces.
void netcfgSetSsid(const String &ssid) {
  prefs().putString("ssid", ssid);
  Serial.printf("WIFI ssid='%s' - now send: pass <key>\n", ssid.c_str());
}

void netcfgSetPassword(const String &password) {
  prefs().putString("pass", password);
  Serial.printf("WIFI key stored (%u chars), restarting\n", password.length());
  delay(200);
  ESP.restart();
}

void netcfgForget() {
  prefs().remove("ssid");
  prefs().remove("pass");
  Serial.println("WIFI forgotten");
}
