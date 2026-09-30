# ESP32-printer label printer - ESP32 side
Enables a printer to be wireless and work on wifi.

For receiving print jobs from catourne.ch user space and sending it to a **Brother PT-P700**.

Also sends a heartbeat and confirms a probe.

See [GOAL.md](GOAL.md) for the architecture and [PROTOCOL.md](PROTOCOL.md) for the API the backend must provide.

## Firmware

PlatformIO + Arduino-ESP32 3.x (ESP-IDF 5.4), board ESP32-S3-DevKitC-1.

- `src/printer.*` – PT-P700 over the S3's USB OTG port in host mode (ESP-IDF USB host library): hotplug, status request `ESC i S`, sending jobs, waiting for "printing completed".
- `src/backend.*` – HTTPS client (TLS verified against the built-in root CA bundle): heartbeat, long-poll for jobs, claim/report.
- `src/config.*` – loads `/config.json`.
- `src/main.cpp` – main loop: heartbeat every 15 s, then fetch → check printer → claim → print → report.

## Credentials / config

Settings live in `data/config.json` (git-ignored), which is flashed into the LittleFS partition separately from the firmware. You can change Wi-Fi or the token without rebuilding:

```sh
cp config.json data/config.json            # then edit
pio run -t uploadfs                        # flash config
pio run -t upload                          # flash firmware
pio device monitor
```

`wifi` is a list of `{ssid, password}`; the strongest known network in range is used, and the ESP reconnects to whichever is available.

`token` is the device secret that the backend issues for this `printerId`.

The loader takes any `fs::FS`. To read the config from an SD card instead, replace `LittleFS` in `setup()` with `SD` (plus `SD.begin(csPin)`).

## Wiring

```text
12 V battery ──┬──────────────────────► PT-P700 DC in (12 V)
               └─► buck 12 V → 5 V ───► DevKit 5V pin
GND ─────────── common ground: battery, buck, DevKit GND
DevKit USB (OTG) port ──USB cable──► PT-P700 USB
```

The OTG port is the S3's internal USB PHY on fixed pins. Without the DevKit's USB socket, wire a USB-A female socket directly:

| USB-A pin (wire color) | ESP32-S3 |
|---|---|
| 1 VBUS (red) | 5 V |
| 2 D− (white) | GPIO19 |
| 3 D+ (green) | GPIO20 |
| 4 GND (black) | GND |

## Hardware notes

- Plug the printer into the DevKit's **USB** (OTG) port. Serial logs are on the **UART** port.
- The USB port must supply 5 V on VBUS to the printer.
- PT-P700: set the P-Lite/EL switch **off**. In P-Lite mode the printer appears as a different USB device (`UNSUPPORTED_DEVICE`). Also disable auto power-off, otherwise the printer eventually disappears (`NOT_CONNECTED`).
