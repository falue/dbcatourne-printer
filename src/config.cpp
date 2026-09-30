#include "config.h"
#include <ArduinoJson.h>

bool Config::load(fs::FS &fs, const char *path) {
  File file = fs.open(path);
  if (!file) {
    Serial.printf("Config: %s not found\n", path);
    return false;
  }
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, file)) {
    Serial.printf("Config: %s\n", err.c_str());
    return false;
  }
  for (JsonObject w : doc["wifi"].as<JsonArray>())
    if (String ssid = w["ssid"] | ""; !ssid.isEmpty()) wifi.push_back({ssid, w["password"] | ""});
  server = doc["server"] | "";
  printerId = doc["printerId"] | "";
  token = doc["token"] | "";
  if (wifi.empty() || server.isEmpty() || printerId.isEmpty() || token.isEmpty()) {
    Serial.println("Config: wifi (list of {ssid, password}), server, printerId and token are required");
    return false;
  }
  return true;
}
