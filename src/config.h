#pragma once
#include <FS.h>
#include <vector>

// Device settings and credentials, read from a JSON file (see config.json).
struct Config {
  struct Wifi {
    String ssid, password;
  };
  std::vector<Wifi> wifi;  // the strongest one in range is used
  String server, printerId, token;

  // Returns false (and logs why) when the file is missing, invalid or incomplete.
  bool load(fs::FS &fs, const char *path = "/config.json");
};
