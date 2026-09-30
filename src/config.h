#pragma once
#include <FS.h>
#include <vector>

// Device settings and credentials, read from a JSON file (see config.example.json).
struct Config {
  struct Wifi {
    String ssid, password;
  };
  std::vector<Wifi> wifi;  // the strongest one in range is used
  String server, printerId, token;

  // Returns nullptr, or why the file is missing, invalid or incomplete.
  const char *load(fs::FS &fs, const char *path = "/config.json");
};
