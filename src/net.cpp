#include "net.h"

#include <esp_netif.h>
#include <esp_wifi.h>

NetStatus netStatus() {
  NetStatus s{};
  strlcpy(s.ip, "0.0.0.0", sizeof(s.ip));
  strlcpy(s.linkLocal, "::", sizeof(s.linkLocal));

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return s;
  }
  s.connected = true;
  strlcpy(s.ssid, (const char *)ap.ssid, sizeof(s.ssid));
  s.rssi = ap.rssi;
  s.channel = ap.primary;

  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) {
    return s;
  }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
    snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&ip.ip));
  }
  esp_ip6_addr_t ip6;
  if (esp_netif_get_ip6_linklocal(netif, &ip6) == ESP_OK) {
    snprintf(s.linkLocal, sizeof(s.linkLocal), IPV6STR, IPV62STR(ip6));
  }
  return s;
}
