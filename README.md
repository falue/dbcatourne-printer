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
pio run -t uploadfs                        # flash config
pio run -t upload                          # flash firmware
pio device monitor
```

The ESP reads its own flashed copy, so run `pio run -t uploadfs` after **every** change to `data/config.json` (a config-only change needs no firmware upload). `config.example.json` is just the template and never reaches the ESP. Close the serial monitor first, as the upload needs the same port.

If `pio` is not found, use `~/.platformio/penv/bin/pio` or add that folder to your `PATH`.

`wifi` is a list of `{ssid, password}`; the strongest known network in range is used, and the ESP reconnects to whichever is available.
> Be aware that this ESP can only connect to 2.4GHZ networks! No 5G allowed!

`token` is the device secret that the backend issues for this `printerId`.

The loader takes any `fs::FS`. To read the config from an SD card instead, replace `LittleFS` in `setup()` with `SD` (plus `SD.begin(csPin)`).

## Testing without the backend

`tools/mock_backend.py` (Python 3, no dependencies) stands in for the backend on your LAN: it logs every heartbeat (printer connected/ready, tape, error) and job report, so you see the printer state over Wi-Fi without a serial cable.

```sh
python3 tools/mock_backend.py   # prints the URL to put into "server" in data/config.json, then uploadfs
```

Press Enter to queue a test label, or drop a `.bin` PT-P700 command stream into `tools/jobs/`. Finished jobs move to `tools/jobs/done/` with the result in the file name. No auth, no TLS: for testing only.

## Wiring

```text
 AC-DC 230v to 12V
        │ barrell jack
        ▼

Barrel jack in case
        │ wires to usb c
        ▼
    POWER BANK
  INIU P41 USB-C
        │ usb-c to usb-c
        ▼
Joy-it COM-ZY12PDN
    set to 12 V
        │
        │      wires to 
        │     barrel jack
        ├─────────────────────▶ Brother PT-P700
        │                       Draws up to 12 V / 2 A
        │                               ▲
        │                               │ USB A to USB mini
        └───▶ 12→5V buck ────▶ ESP32 ───┘
                          with USB A port
```

The OTG port is the S3's internal USB PHY on fixed pins. Without the DevKit's USB socket, wire a USB-A female socket directly:

| USB-A pin (wire color) | ESP32-S3 |
|---|---|
| 1 VBUS (red) | 5 V |
| 2 D− (white) | GPIO19 |
| 3 D+ (green) | GPIO20 |
| 4 GND (black) | GND |

## Hardware notes

- **Board requirement:** GPIO19/20 must be free for the printer. Use a board whose native USB goes straight to its own port, e.g. the Espressif **ESP32-S3-DevKitC-1** (two USB-C: "USB" = GPIO19/20, "UART" = serial chip). Boards with a single USB-C and an onboard USB hub (e.g. Waveshare ESP32-S3-DEV-KIT with CH334F + CH343) wire GPIO19/20 to that hub: USB host then fails, with the printer endlessly reconnecting while the USB-C is plugged in and never detected without it.
- On-board RGB LED: GPIO48 on DevKitC-1 v1.0, GPIO38 on v1.1 (the firmware uses `RGB_BUILTIN` = 48).

- Plug the printer into the DevKit's **USB** (OTG) port. Serial logs are on the **UART** port.
- The USB port must supply 5 V on VBUS to the printer.
- PT-P700: set the P-Lite/EL switch **off**. In P-Lite mode the printer appears as a different USB device (`UNSUPPORTED_DEVICE`). Also disable auto power-off, otherwise the printer eventually disappears (`NOT_CONNECTED`).
