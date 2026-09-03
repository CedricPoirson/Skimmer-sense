#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define SKIMMERSENSE_HAS_HOME_WIFI 1
#else
#define SKIMMERSENSE_HAS_HOME_WIFI 0
#endif

// service_mode.cpp already implements the validated AP + HTTP + OTA service
// page.  This small proxy keeps that implementation untouched while upgrading
// its Wi-Fi mode to AP+STA when private home-network credentials are present.
// The fallback SkimmerSense-XXXXXX access point therefore remains available
// even when the home WLAN cannot be reached.
class SkmServiceWiFiProxy {
 public:
  bool mode(wifi_mode_t requestedMode) {
#if SKIMMERSENSE_HAS_HOME_WIFI
    (void)requestedMode;
    return WiFi.mode(WIFI_AP_STA);
#else
    return WiFi.mode(requestedMode);
#endif
  }

  bool softAP(const char *ssid, const char *password) {
    const bool apOk = WiFi.softAP(ssid, password);

#if SKIMMERSENSE_HAS_HOME_WIFI
    WiFi.setHostname("skimmersense");
    WiFi.begin(SKIMMERSENSE_WIFI_SSID, SKIMMERSENSE_WIFI_PASSWORD);

    Serial.printf("Home Wi-Fi: connecting to %s", SKIMMERSENSE_WIFI_SSID);
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 12000UL) {
      Serial.print('.');
      delay(250);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      const IPAddress staIp = WiFi.localIP();
      Serial.printf("Home Wi-Fi: connected, IP %s\n", staIp.toString().c_str());
      if (MDNS.begin("skimmersense")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("Home Wi-Fi URL: http://skimmersense.local/");
      } else {
        Serial.println("mDNS: failed; use the home-network IP shown above");
      }
    } else {
      WiFi.disconnect(false, false);
      Serial.println("Home Wi-Fi: connection timeout; maintenance AP remains available");
    }
#else
    Serial.println("Home Wi-Fi: no private wifi_secrets.h; maintenance AP only");
#endif

    return apOk;
  }

  IPAddress softAPIP() const {
    return WiFi.softAPIP();
  }
};

SkmServiceWiFiProxy skmServiceWiFi;

// Compile the existing SERVICE implementation through the proxy above.  The
// original service_mode.cpp remains the single source of truth for HTTP/OTA.
#define WiFi skmServiceWiFi
#include "service_mode.cpp"
#undef WiFi
