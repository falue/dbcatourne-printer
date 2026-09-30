#include "config.h"
#include <ArduinoJson.h>

const char *Config::load(fs::FS &fs, const char *path) {
  File file = fs.open(path);
  if (!file) return "config.json not found";
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, file)) return err.c_str();
  for (JsonObject w : doc["wifi"].as<JsonArray>())
    if (String ssid = w["ssid"] | ""; !ssid.isEmpty()) wifi.push_back({ssid, w["password"] | ""});
  server = doc["server"] | "";
  printerId = doc["printerId"] | "";
  token = doc["token"] | "";
  if (wifi.empty() || server.isEmpty() || printerId.isEmpty() || token.isEmpty())
    return "wifi (list of {ssid, password}), server, printerId and token are required";
  return nullptr;
}
