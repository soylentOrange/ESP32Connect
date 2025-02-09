#include <ESP32Connect.h>

AsyncWebServer server(80);
Soylent::ESP32Connect espConnect(server);
uint32_t lastLog = 0;
uint32_t lastChange = 0;
const char* hostname = "arduino-1";

void setup() {
  Serial.begin(115200);
  while (!Serial)
    continue;

  server.on("/clear", HTTP_GET, [&](AsyncWebServerRequest* request) {
    Serial.println("Clearing configuration...");
    espConnect.clearConfiguration();
    request->send(200);
    ESP.restart();
  });

  // network state listener is required here in async mode
  espConnect.listen([](__unused Soylent::ESP32Connect::State previous, Soylent::ESP32Connect::State state) {
    JsonDocument doc;
    espConnect.toJson(doc.to<JsonObject>());
    serializeJson(doc, Serial);
    Serial.println();

    switch (state) {
      case Soylent::ESP32Connect::State::NETWORK_CONNECTED:
      case Soylent::ESP32Connect::State::AP_STARTED:
        // serve your home page here
        server.on("/", HTTP_GET, [&](AsyncWebServerRequest* request) {
                return request->send(200, "text/plain", "Hello World!");
              })
          .setFilter([](__unused AsyncWebServerRequest* request) { return espConnect.getState() != Soylent::ESP32Connect::State::PORTAL_STARTED; });
        server.begin();
        break;

      case Soylent::ESP32Connect::State::NETWORK_DISCONNECTED:
        server.end();
        break;

      default:
        break;
    }
  });

  espConnect.setAutoRestart(true);
  espConnect.setBlocking(false);

  Serial.println("====> Trying to connect to saved WiFi or will start portal in the background...");

  espConnect.begin(hostname, "Captive Portal SSID");

  Serial.println("====> setup() completed...");
}

void loop() {
  espConnect.loop();

  uint32_t now = millis();

  if (now - lastLog > 3000) {
    JsonDocument doc;
    espConnect.toJson(doc.to<JsonObject>());
    serializeJson(doc, Serial);
    Serial.println();
    lastLog = millis();
  }

  if (now - lastChange > 10000) {
    if (espConnect.getIPConfig().ip == INADDR_NONE) {
      Soylent::ESP32Connect::IPConfig ipConfig;
      ipConfig.ip.fromString("192.168.125.99");
      ipConfig.gateway.fromString("192.168.125.1");
      ipConfig.subnet.fromString("255.255.255.0");
      ipConfig.dns.fromString("192.168.125.1");
      espConnect.setIPConfig(ipConfig);
    } else {
      espConnect.setIPConfig(Soylent::ESP32Connect::IPConfig());
    }
    lastChange = millis();
  }
}
