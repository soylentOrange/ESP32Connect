# ESP32Connect

Simple & Easy Network Manager for ESP32 with WiFi, Ethernet and Captive Portal support

<!-- [![](https://mathieu.carbou.me/MycilaESPConnect/screenshot.png)](https://mathieu.carbou.me/MycilaESPConnect/screenshot.png) -->

This library is based on [MycilaESPConnect](https://github.com/mathieucarbou/MycilaESPConnect), which I highly recommend. 
This fork is intended as a playground for changing the web frontend (and also remove ethernet and eps8266 support).

- [Usage](#usage)
  - [API](#api)
  - [Blocking mode](#blocking-mode)
  - [Non-blocking mode](#non-blocking-mode)
  - [Use an external configuration system](#use-an-external-configuration-system)
  - [Logo](#logo)
  - [mDNS](#mdns)

## Usage

### API

- `espConnect.setAutoRestart(bool)`: will automatically restart the ESP after the captive portal times out, or after the captive portal has been answered by te user
- `espConnect.setBlocking(bool)`: will block the execution of the program in the begin code to handle the connect. If false, the setup code will continue in the background and the network setup will be done in the background from the main loop.
- `espConnect.listen()`: register a callback for all ESPConnect events

2 flavors of `begin()` methods:

1. `espConnect.begin("hostname", "ssid", "password")` / `espConnect.begin("hostname", "ssid")`
2. `espConnect.begin("hostname", "ssid", "password", Mycila::ESPConnect::Config)` where config is `{.wifiSSID = ..., .wifiPassword = ..., .apMode = ...}`

The first flavors will automatically handle the persistance of user choices and reload them at startup.

The second choice let the user handle the load/save of the configuration.

Please have a look at the self-documented API for the other methods and teh examples.

### Blocking mode

```cpp
  AsyncWebServer server(80);
  Soylent::ESPConnect espConnect(server);

  espConnect.listen([](__unused Soylent::ESPConnect::State previous, __unused Soylent::ESPConnect::State state) {
    // ...
  });

  espConnect.setAutoRestart(true);
  espConnect.setBlocking(true);
  espConnect.begin("arduino", "Captive Portal SSID");
  Serial.println("ESPConnect completed!");
```

### Non-blocking mode

```cpp
void setup() {
  AsyncWebServer server(80);
  Soylent::ESPConnect espConnect(server);

  espConnect.listen([](__unused Soylent::ESPConnect::State previous, __unused Soylent::ESPConnect::State state) {
    // ...
  });

  espConnect.setAutoRestart(true);
  espConnect.setBlocking(false);
  espConnect.begin("arduino", "Captive Portal SSID");
  Serial.println("ESPConnect started!");
}

void loop() {
  espConnect.loop();
}
```

### Set static IP

```cpp
Soylent::ESPConnect::IPConfig ipConfig;

ipConfig.ip.fromString("192.168.125.99");
ipConfig.gateway.fromString("192.168.125.1");
ipConfig.subnet.fromString("255.255.255.0");
ipConfig.dns.fromString("192.168.125.1");

espConnect.setIPConfig(ipConfig);
```

### Use an external configuration system

```cpp
  AsyncWebServer server(80);
  Soylent::ESPConnect espConnect(server);

  espConnect.listen([](__unused Soylent::ESPConnect::State previous, __unused Soylent::ESPConnect::State state) {
    switch (state) {
      case Soylent::ESPConnect::State::PORTAL_COMPLETE:
        bool apMode = espConnect.hasConfiguredAPMode();
        std::string wifiSSID = espConnect.getConfiguredWiFiSSID();
        std::string wifiPassword = espConnect.getConfiguredWiFiPassword();
        if (apMode) {
          Serial.println("====> Captive Portal: Access Point configured");
        } else {
          Serial.println("====> Captive Portal: WiFi configured");
        }
        saveConfig(wifiSSID, wifiPassword, apMode);
        break;

      default:
        break;
    }
  });

  espConnect.setAutoRestart(true);
  espConnect.setBlocking(true);

  // load config from external system
  Soylent::ESPConnect::Config config = {
    .wifiSSID = ...,
    .wifiPassword = ...,
    .apMode = ...
  };

  espConnect.begin("arduino", "Captive Portal SSID", "", config);
```

Known **compatibilities**:

| **Board**                                                                                                                        | **Compile** | **Tested** |
| :------------------------------------------------------------------------------------------------------------------------------- | :---------: | :--------: |
| [Waveshare ESP32-S3-Tiny](https://www.waveshare.com/wiki/ESP32-S3-Tiny) (esp32s3)                          |     ✅      |     ✅     |
| [WEMOS LOLIN S2 Mini](https://www.wemos.cc/en/latest/s2/s2_mini.html) (esp32s2)                              |     ✅      |     ✅     |

### Logo

You can customize the logo by providing a web handler for `/logo`:

```c++
  server.on("/logo", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "image/png", logo_png_gz_start, logo_png_gz_end - logo_png_gz_start);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=900");
    request->send(response);
  });
```

If not provided, the logo won't appear in the Captive Portal.

### mDNS

mDNS takes quite a lot of space in flash (about 25KB).
You can disable it by setting `-D ESPCONNECT_NO_MDNS`.
