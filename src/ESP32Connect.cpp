// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2023-2024 Mathieu Carbou, 2025 Robert Wendlandt
 */
#include "ESP32Connect.h"

#include <cstdio>
#include <string>

#ifndef ESPCONNECT_NO_MDNS
  #include <ESPmDNS.h>
#endif
#include <esp_mac.h>

#include <Preferences.h>
#include <functional>

#include "./espconnect_webpage.h"

#ifdef ESPCONNECT_DEBUG
  #ifdef MYCILA_LOGGER_SUPPORT
    #include <MycilaLogger.h>
extern Mycila::Logger logger;
    #define LOGD(tag, format, ...) logger.debug(tag, format, ##__VA_ARGS__)
    #define LOGI(tag, format, ...) logger.info(tag, format, ##__VA_ARGS__)
    #define LOGW(tag, format, ...) logger.warn(tag, format, ##__VA_ARGS__)
    #define LOGE(tag, format, ...) logger.error(tag, format, ##__VA_ARGS__)
  #else
    #define LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
    #define LOGI(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
    #define LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
    #define LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
  #endif
#else
  #define LOGD(tag, format, ...)
  #define LOGI(tag, format, ...)
  #define LOGW(tag, format, ...)
  #define LOGE(tag, format, ...)
#endif

#define TAG "ESPCONNECT"

static const char* NetworkStateNames[] = {
  "NETWORK_DISABLED",
  "NETWORK_ENABLED",
  "NETWORK_CONNECTING",
  "NETWORK_TIMEOUT",
  "NETWORK_CONNECTED",
  "NETWORK_DISCONNECTED",
  "NETWORK_RECONNECTING",
  "AP_STARTING",
  "AP_STARTED",
  "PORTAL_STARTING",
  "PORTAL_STARTED",
  "PORTAL_COMPLETE",
  "PORTAL_TIMEOUT",
};

const char* Soylent::ESP32Connect::getStateName() const {
  return NetworkStateNames[static_cast<int>(_state)];
}

const char* Soylent::ESP32Connect::getStateName(Soylent::ESP32Connect::State state) const {
  return NetworkStateNames[static_cast<int>(state)];
}

Soylent::ESP32Connect::Mode Soylent::ESP32Connect::getMode() const {
  switch (_state) {
    case Soylent::ESP32Connect::State::AP_STARTED:
    case Soylent::ESP32Connect::State::PORTAL_STARTED:
      return Soylent::ESP32Connect::Mode::AP;
      break;
    case Soylent::ESP32Connect::State::NETWORK_CONNECTED:
    case Soylent::ESP32Connect::State::NETWORK_DISCONNECTED:
    case Soylent::ESP32Connect::State::NETWORK_RECONNECTING:
      if (WiFi.localIP()[0] != 0)
        return Soylent::ESP32Connect::Mode::STA;
      return Soylent::ESP32Connect::Mode::NONE;
    default:
      return Soylent::ESP32Connect::Mode::NONE;
  }
}

std::string Soylent::ESP32Connect::getMACAddress(Soylent::ESP32Connect::Mode mode) const {
  std::string mac;

  switch (mode) {
    case Soylent::ESP32Connect::Mode::AP:
      mac = WiFi.softAPmacAddress().c_str();
      break;
    case Soylent::ESP32Connect::Mode::STA:
      mac = WiFi.macAddress().c_str();
      break;
    default:
      break;
  }

  if (!mac.empty() && mac != "00:00:00:00:00:00")
    return mac;

  // ESP_MAC_IEEE802154 is used to mean "no MAC address" in this context
  esp_mac_type_t type = esp_mac_type_t::ESP_MAC_IEEE802154;

  switch (mode) {
    case Soylent::ESP32Connect::Mode::AP:
      type = ESP_MAC_WIFI_SOFTAP;
      break;
    case Soylent::ESP32Connect::Mode::STA:
      type = ESP_MAC_WIFI_STA;
      break;
    default:
      break;
  }

  if (type == esp_mac_type_t::ESP_MAC_IEEE802154)
    return "";

  uint8_t bytes[6] = {0, 0, 0, 0, 0, 0};
  if (esp_read_mac(bytes, type) != ESP_OK)
    return "";

  char buffer[18] = {0};
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return buffer;
}

IPAddress Soylent::ESP32Connect::getIPAddress(Soylent::ESP32Connect::Mode mode) const {
  const wifi_mode_t wifiMode = WiFi.getMode();
  switch (mode) {
    case Soylent::ESP32Connect::Mode::AP:
      return wifiMode == WIFI_MODE_AP || wifiMode == WIFI_MODE_APSTA ? WiFi.softAPIP() : IPAddress();
    case Soylent::ESP32Connect::Mode::STA:
      return wifiMode == WIFI_MODE_STA ? WiFi.localIP() : IPAddress();
    default:
      return IPAddress();
  }
}

std::string Soylent::ESP32Connect::getWiFiSSID() const {
  switch (WiFi.getMode()) {
    case WIFI_MODE_AP:
    case WIFI_MODE_APSTA:
      return _apSSID;
    case WIFI_MODE_STA:
      return _config.wifiSSID;
    default:
      return {};
  }
}

std::string Soylent::ESP32Connect::getWiFiBSSID() const {
  switch (WiFi.getMode()) {
    case WIFI_MODE_AP:
    case WIFI_MODE_APSTA:
      return WiFi.softAPmacAddress().c_str();
    case WIFI_MODE_STA:
      return WiFi.BSSIDstr().c_str();
    default:
      return {};
  }
}

int8_t Soylent::ESP32Connect::getWiFiRSSI() const {
  return WiFi.getMode() == WIFI_MODE_STA ? WiFi.RSSI() : 0;
}

int8_t Soylent::ESP32Connect::getWiFiSignalQuality() const {
  return WiFi.getMode() == WIFI_MODE_STA ? _wifiSignalQuality(WiFi.RSSI()) : 0;
}

int8_t Soylent::ESP32Connect::_wifiSignalQuality(int32_t rssi) {
  int32_t s = map(rssi, -90, -30, 0, 100);
  return s > 100 ? 100 : (s < 0 ? 0 : s);
}

void Soylent::ESP32Connect::begin(const char* hostname, const char* apSSID, const char* apPassword) {
  if (_state != Soylent::ESP32Connect::State::NETWORK_DISABLED)
    return;

  _autoSave = true;

  LOGD(TAG, "Loading config...");
  Preferences preferences;
  preferences.begin("ESP32Connect", true);
  std::string ssid;
  std::string password;
  if (preferences.isKey("ssid"))
    ssid = preferences.getString("ssid").c_str();
  if (preferences.isKey("password"))
    password = preferences.getString("password").c_str();
  bool ap = preferences.isKey("ap") ? preferences.getBool("ap", false) : false;
  preferences.end();
  LOGD(TAG, " - AP: %d", ap);
  LOGD(TAG, " - SSID: %s", ssid.c_str());

  begin(hostname, apSSID, apPassword, {ssid, password, ap});
}

void Soylent::ESP32Connect::begin(const char* hostname, const char* apSSID, const char* apPassword, const Soylent::ESP32Connect::Config& config) {
  if (_state != Soylent::ESP32Connect::State::NETWORK_DISABLED)
    return;

  _hostname = hostname;
  _apSSID = apSSID;
  _apPassword = apPassword;
  _config = config; // copy values

  // TODO(soylentOrange): Change std::bind to lambda
  _wifiEventListenerId = WiFi.onEvent(std::bind(&ESP32Connect::_onWiFiEvent, this, std::placeholders::_1));

  _state = Soylent::ESP32Connect::State::NETWORK_ENABLED;

  // blocks like the old behaviour
  if (_blocking) {
    LOGI(TAG, "Starting ESP32Connect in blocking mode...");
    while (_state != Soylent::ESP32Connect::State::AP_STARTED && _state != Soylent::ESP32Connect::State::NETWORK_CONNECTED) {
      loop();
      delay(100);
    }
  } else {
    LOGI(TAG, "Starting ESP32Connect in non-blocking mode...");
  }
}

void Soylent::ESP32Connect::end() {
  if (_state == Soylent::ESP32Connect::State::NETWORK_DISABLED)
    return;
  LOGI(TAG, "Stopping ESP32Connect...");
  _lastTime = -1;
  _autoSave = false;
  _setState(Soylent::ESP32Connect::State::NETWORK_DISABLED);
  WiFi.removeEvent(_wifiEventListenerId);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  _stopAP();
  _httpd = nullptr;
}

void Soylent::ESP32Connect::loop() {
  if (_dnsServer != nullptr)
    _dnsServer->processNextRequest();

  // first check if we have to enter AP mode
  if (_state == Soylent::ESP32Connect::State::NETWORK_ENABLED && _config.apMode) {
    _startAP();
  }

  // start captive portal when network enabled but not in ap mode and no wifi info
  // portal wil be interrupted when network connected
  if (_state == Soylent::ESP32Connect::State::NETWORK_ENABLED && _config.wifiSSID.empty()) {
    _startAP();
  }

  // otherwise, tries to connect to WiFi
  if (_state == Soylent::ESP32Connect::State::NETWORK_ENABLED) {
    if (!_config.wifiSSID.empty())
      _startSTA();
  }

  // connection to WiFi timed out ?
  if (_state == Soylent::ESP32Connect::State::NETWORK_CONNECTING && _durationPassed(_connectTimeout)) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.config(static_cast<uint32_t>(0x00000000), static_cast<uint32_t>(0x00000000), static_cast<uint32_t>(0x00000000), static_cast<uint32_t>(0x00000000));
      WiFi.disconnect(true, true);
    }
    _setState(Soylent::ESP32Connect::State::NETWORK_TIMEOUT);
  }

  // start captive portal on connect timeout
  if (_state == Soylent::ESP32Connect::State::NETWORK_TIMEOUT) {
    _startAP();
  }

  // timeout portal if we failed to connect to WiFi (we got a SSID) and portal duration is passed
  // in order to restart and try again to connect to the configured WiFi
  if (_state == Soylent::ESP32Connect::State::PORTAL_STARTED && !_config.wifiSSID.empty() && _durationPassed(_portalTimeout)) {
    _setState(Soylent::ESP32Connect::State::PORTAL_TIMEOUT);
  }

  // disconnect from network ? reconnect!
  if (_state == Soylent::ESP32Connect::State::NETWORK_DISCONNECTED) {
    _setState(Soylent::ESP32Connect::State::NETWORK_RECONNECTING);
  }

  if (_state == Soylent::ESP32Connect::State::AP_STARTED || _state == Soylent::ESP32Connect::State::NETWORK_CONNECTED) {
    _disableCaptivePortal();
  }

  if (_state == Soylent::ESP32Connect::State::PORTAL_COMPLETE || _state == Soylent::ESP32Connect::State::PORTAL_TIMEOUT) {
    _stopAP();
    if (_autoRestart) {
      LOGW(TAG, "Auto Restart of ESP...");
      ESP.restart();
    } else {
      _setState(Soylent::ESP32Connect::State::NETWORK_ENABLED);
    }
  }
}

void Soylent::ESP32Connect::clearConfiguration() {
  Preferences preferences;
  preferences.begin("ESP32Connect", false);
  preferences.clear();
  preferences.end();
}

void Soylent::ESP32Connect::toJson(const JsonObject& root) const {
  root["ip_address"] = getIPAddress().toString();
  root["ip_address_ap"] = getIPAddress(Soylent::ESP32Connect::Mode::AP).toString();
  root["ip_address_sta"] = getIPAddress(Soylent::ESP32Connect::Mode::STA).toString();
  root["mac_address"] = getMACAddress();
  root["mac_address_ap"] = getMACAddress(Soylent::ESP32Connect::Mode::AP);
  root["mac_address_sta"] = getMACAddress(Soylent::ESP32Connect::Mode::STA);
  root["mode"] = getMode() == Soylent::ESP32Connect::Mode::AP ? "AP" : (getMode() == Soylent::ESP32Connect::Mode::STA ? "STA" : "NONE");
  root["state"] = getStateName();
  root["wifi_bssid"] = getWiFiBSSID();
  root["wifi_rssi"] = getWiFiRSSI();
  root["wifi_signal"] = getWiFiSignalQuality();
  root["wifi_ssid"] = getWiFiSSID();
}

void Soylent::ESP32Connect::_setState(Soylent::ESP32Connect::State state) {
  if (_state == state)
    return;

  const Soylent::ESP32Connect::State previous = _state;
  _state = state;
  LOGD(TAG, "State: %s => %s", getStateName(previous), getStateName(state));

  // be sure to save anything before auto restart and callback
  if (_autoSave && _state == Soylent::ESP32Connect::State::PORTAL_COMPLETE) {
    LOGD(TAG, "Saving config...");
    LOGD(TAG, " - AP: %d", _config.apMode);
    LOGD(TAG, " - SSID: %s", _config.wifiSSID.c_str());
    Preferences preferences;
    preferences.begin("ESP32Connect", false);
    preferences.putBool("ap", _config.apMode);
    if (!_config.apMode) {
      preferences.putString("ssid", _config.wifiSSID.c_str());
      preferences.putString("password", _config.wifiPassword.c_str());
    }
    preferences.end();
  }

  // make sure callback is called before auto restart
  if (_callback != nullptr)
    _callback(previous, state);
}

void Soylent::ESP32Connect::_startSTA() {
  _setState(Soylent::ESP32Connect::State::NETWORK_CONNECTING);

  LOGI(TAG, "Starting WiFi...");

  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname(_hostname.c_str());
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);

  if (_ipConfig.ip) {
    LOGI(TAG, "Set WiFi Static IP Configuration:");
    LOGI(TAG, " - IP: %s", _ipConfig.ip.toString().c_str());
    LOGI(TAG, " - Gateway: %s", _ipConfig.gateway.toString().c_str());
    LOGI(TAG, " - Subnet: %s", _ipConfig.subnet.toString().c_str());
    LOGI(TAG, " - DNS: %s", _ipConfig.dns.toString().c_str());

    WiFi.config(_ipConfig.ip, _ipConfig.gateway, _ipConfig.subnet, _ipConfig.dns);
  }

  LOGD(TAG, "Connecting to SSID: %s...", _config.wifiSSID.c_str());
  WiFi.begin(_config.wifiSSID.c_str(), _config.wifiPassword.c_str());

  _lastTime = millis();

  LOGD(TAG, "WiFi started.");
}

void Soylent::ESP32Connect::_startAP() {
  _setState(_config.apMode ? Soylent::ESP32Connect::State::AP_STARTING : Soylent::ESP32Connect::State::PORTAL_STARTING);

  LOGI(TAG, "Starting Access Point...");

  WiFi.softAPsetHostname(_hostname.c_str());
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname(_hostname.c_str());
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

  WiFi.mode(_config.apMode ? WIFI_AP : WIFI_AP_STA);

  if (_apPassword.empty() || _apPassword.length() < 8) {
    // Disabling invalid Access Point password which must be at least 8 characters long when set
    WiFi.softAP(_apSSID.c_str(), "");
  } else {
    WiFi.softAP(_apSSID.c_str(), _apPassword.c_str());
  }

  if (_dnsServer == nullptr) {
    _dnsServer = new DNSServer();
    _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer->start(53, "*", WiFi.softAPIP());
  }

  LOGD(TAG, "Access Point started.");

  if (!_config.apMode)
    _enableCaptivePortal();
}

void Soylent::ESP32Connect::_stopAP() {
  _disableCaptivePortal();
  LOGI(TAG, "Stopping Access Point...");
  _lastTime = -1;
  WiFi.softAPdisconnect(true);
  if (_dnsServer != nullptr) {
    _dnsServer->stop();
    delete _dnsServer;
    _dnsServer = nullptr;
  }
  LOGD(TAG, "Access Point stopped.");
}

void Soylent::ESP32Connect::_enableCaptivePortal() {
  LOGI(TAG, "Enable Captive Portal...");
  _scan();

  if (_scanHandler == nullptr) {
    _scanHandler = &_httpd->on("/espconnect/scan", HTTP_GET, [&](AsyncWebServerRequest* request) {
      int n = WiFi.scanComplete();

      if (n == WIFI_SCAN_RUNNING) {
        // scan still running ? wait...
        request->send(202);

      } else if (n == WIFI_SCAN_FAILED) {
        // scan error or finished with no result ?
        // re-scan
        _scan();
        request->send(202);

      } else {
        // scan results ?
        AsyncJsonResponse* response = new AsyncJsonResponse(true);
        JsonArray json = response->getRoot();

        // we have some results
        for (int i = 0; i < n; ++i) {
#if ARDUINOJSON_VERSION_MAJOR == 6
          JsonObject entry = json.createNestedObject();
#else
            JsonObject entry = json.add<JsonObject>();
#endif
          entry["name"] = WiFi.SSID(i);
          entry["rssi"] = WiFi.RSSI(i);
          entry["signal"] = _wifiSignalQuality(WiFi.RSSI(i));
          entry["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        }

        WiFi.scanDelete();
        response->setLength();
        request->send(response);
      }
    });
  }

  if (_connectHandler == nullptr) {
    _connectHandler = &_httpd->on("/espconnect/connect", HTTP_POST, [&](AsyncWebServerRequest* request) {
      _config.apMode = request->hasParam("ap_mode", true) && request->getParam("ap_mode", true)->value() == "true";
      if (_config.apMode) {
        request->send(200, "application/json", "{\"message\":\"Configuration Saved.\"}");
        _setState(Soylent::ESP32Connect::State::PORTAL_COMPLETE);
      } else {
        std::string ssid;
        std::string password;
        if (request->hasParam("ssid", true))
          ssid = request->getParam("ssid", true)->value().c_str();
        if (request->hasParam("password", true))
          password = request->getParam("password", true)->value().c_str();
        if (ssid.empty())
          return request->send(400, "application/json", "{\"message\":\"Invalid SSID\"}");
        if (ssid.length() > 32 || password.length() > 64 || (!password.empty() && password.length() < 8))
          return request->send(400, "application/json", "{\"message\":\"Credentials exceed character limit of 32 & 64 respectively, or password lower than 8 characters.\"}");
        _config.wifiSSID = ssid;
        _config.wifiPassword = password;
        request->send(200, "application/json", "{\"message\":\"Configuration Saved.\"}");
        _setState(Soylent::ESP32Connect::State::PORTAL_COMPLETE);
      }
    });
  }

  if (_homeHandler == nullptr) {
    _homeHandler = &_httpd->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
      AsyncWebServerResponse* response = request->beginResponse(200, "text/html", ESPCONNECT_HTML, sizeof(ESPCONNECT_HTML));
      response->addHeader("Content-Encoding", "gzip");
      return request->send(response);
    });
    _homeHandler->setFilter([&](__unused AsyncWebServerRequest* request) {
      return _state == Soylent::ESP32Connect::State::PORTAL_STARTED;
    });
  }

  _httpd->onNotFound([](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "text/html", ESPCONNECT_HTML, sizeof(ESPCONNECT_HTML));
    response->addHeader("Content-Encoding", "gzip");
    return request->send(response);
  });

  _httpd->begin();
#ifndef ESPCONNECT_NO_MDNS
  MDNS.addService("http", "tcp", 80);
#endif
  _lastTime = millis();
}

void Soylent::ESP32Connect::_disableCaptivePortal() {
  if (_homeHandler == nullptr)
    return;

  LOGI(TAG, "Disable Captive Portal...");

  WiFi.scanDelete();

#ifndef ESPCONNECT_NO_MDNS
  mdns_service_remove("_http", "_tcp");
#endif

  _httpd->end();
  _httpd->onNotFound(nullptr);

  if (_connectHandler != nullptr) {
    _httpd->removeHandler(_connectHandler);
    _connectHandler = nullptr;
  }

  if (_scanHandler != nullptr) {
    _httpd->removeHandler(_scanHandler);
    _scanHandler = nullptr;
  }

  if (_homeHandler != nullptr) {
    _httpd->removeHandler(_homeHandler);
    _homeHandler = nullptr;
  }
}

void Soylent::ESP32Connect::_onWiFiEvent(WiFiEvent_t event) {
  if (_state == Soylent::ESP32Connect::State::NETWORK_DISABLED)
    return;

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      if (_state == Soylent::ESP32Connect::State::NETWORK_CONNECTING || _state == Soylent::ESP32Connect::State::NETWORK_RECONNECTING) {
        LOGD(TAG, "[%s] WiFiEvent: ARDUINO_EVENT_WIFI_STA_GOT_IP", getStateName());
        _lastTime = -1;
#ifndef ESPCONNECT_NO_MDNS
        MDNS.begin(_hostname.c_str());
#endif
        _setState(Soylent::ESP32Connect::State::NETWORK_CONNECTED);
      }
      break;

    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        LOGD(TAG, "[%s] WiFiEvent: ARDUINO_EVENT_WIFI_STA_DISCONNECTED", getStateName());
        WiFi.reconnect();
      } else {
        LOGD(TAG, "[%s] WiFiEvent: ARDUINO_EVENT_WIFI_STA_LOST_IP", getStateName());
      }
      if (_state == Soylent::ESP32Connect::State::NETWORK_CONNECTED) {
        _setState(Soylent::ESP32Connect::State::NETWORK_DISCONNECTED);
      }
      break;

    case ARDUINO_EVENT_WIFI_AP_START:
#ifndef ESPCONNECT_NO_MDNS
      MDNS.begin(_hostname.c_str());
#endif
      if (_state == Soylent::ESP32Connect::State::AP_STARTING) {
        LOGD(TAG, "[%s] WiFiEvent: ARDUINO_EVENT_WIFI_AP_START", getStateName());
        _setState(Soylent::ESP32Connect::State::AP_STARTED);
      } else if (_state == Soylent::ESP32Connect::State::PORTAL_STARTING) {
        LOGD(TAG, "[%s] WiFiEvent: ARDUINO_EVENT_WIFI_AP_START", getStateName());
        _setState(Soylent::ESP32Connect::State::PORTAL_STARTED);
      }
      break;

    default:
      break;
  }
}

bool Soylent::ESP32Connect::_durationPassed(uint32_t intervalSec) {
  if (_lastTime >= 0 && millis() - static_cast<uint32_t>(_lastTime) >= intervalSec * 1000) {
    _lastTime = -1;
    return true;
  }
  return false;
}

void Soylent::ESP32Connect::_scan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false, false, 500, 0, nullptr, nullptr);
}
