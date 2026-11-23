#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <Adafruit_GFX.h>
#include <atomic>
#include <optional>
#include <vector>
#include "backend.h"
#include "config.h"
#include "printer.h"

static Config config;
static Printer printer;
static Backend backend;
static WiFiMulti wifi;
static uint8_t wifiReason = 0;  // last disconnect reason, 0 = none yet

// On-board RGB LED state, shown by ledTask() (see README).
enum class Led : uint8_t { Unreachable, Idle, Busy, Done, Printer, Error };
static std::atomic<Led> led = Led::Unreachable;

// LED for a failed backend request: -1 = no connection, else an HTTP error.
static Led httpLed(int code) { return code < 0 ? Led::Unreachable : Led::Error; }

// Final job result not yet accepted by the backend. No new job is fetched until it is delivered.
struct Result {
  String jobId;
  const char *error;  // nullptr = printed
};
static std::optional<Result> pending;

static void handle(const Job &job) {
  Serial.printf("Job %s: %u bytes\n", job.id.c_str(), job.len);
  if (!job.data) {
    pending = Result{job.id, "JOB_TOO_LARGE"};
    led = Led::Error;
    return;
  }
  Printer::Status st = printer.status();
  if (!st.ready) {
    pending = Result{job.id, st.error};
    led = Led::Printer;
    return;
  }
  // Claim first: the backend refuses (e.g. 409) jobs that were cancelled or already handed out.
  int code = backend.report(job.id, "printing");
  if (code != 200) {
    Serial.printf("Job %s not claimed (HTTP %d)\n", job.id.c_str(), code);
    led = httpLed(code);
    return;
  }
  const char *error = printer.print(job.data.get(), job.len);
  pending = Result{job.id, error};
  led = error ? Led::Printer : Led::Done;
}

// Wi-Fi diagnostics for the test label: why there is no connection, known networks and a fresh scan.
static std::vector<String> wifiDebug(const char *configError) {
  std::vector<String> lines = {"WiFi: not connected"};
  if (configError) lines.push_back("Config: " + String(configError));
  else if (wifiReason) lines.push_back("Reason: " + String(WiFi.disconnectReasonName(wifi_err_reason_t(wifiReason))));
  else lines.push_back("Reason: no known network in scan, or timeout");
  String known = "Known:";
  for (const auto &w : config.wifi) known += " '" + w.ssid + "'";
  lines.push_back(known);
  lines.push_back("MAC: " + WiFi.macAddress());
  int n = std::max<int16_t>(WiFi.scanNetworks(), 0);
  lines.push_back("Scan: " + String(n) + " networks, * = known");
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  for (int i : order) {
    String ssid = WiFi.SSID(i);
    bool isKnown = std::any_of(config.wifi.begin(), config.wifi.end(), [&](const Config::Wifi &w) { return w.ssid == ssid; });
    lines.push_back(String(WiFi.RSSI(i)) + " dBm ch" + WiFi.channel(i) + " " + (ssid.isEmpty() ? "(hidden)" : ssid) + (isKnown ? " *" : ""));
  }
  WiFi.scanDelete();
  return lines;
}

// Temporary: prints a framed label with the Wi-Fi state and a stripe pattern if the printer shows up at boot.
static void testPrint(const char *configError) {
  constexpr uint32_t WAIT_MS = 5000;
  constexpr int FRAME = 3, MARGIN = 10, STRIPES = 60;  // dots (180 dpi)
  // Text first: the Wi-Fi scan in wifiDebug() can knock the printer off USB, so nothing may run between status and print.
  const String title = "FUNDUS \x80" "A TOURNE";  // \x80: CP437 capital C cedilla
  const bool online = WiFi.status() == WL_CONNECTED;
  std::vector<String> body;
  if (online) {
    body = {"WiFi: " + WiFi.SSID(), "Signal: " + String(WiFi.RSSI()) + " dBm, ch " + WiFi.channel(),
            "IP: " + WiFi.localIP().toString()};
  } else {
    body = wifiDebug(configError);
    for (const auto &b : body) Serial.println(b);
  }
  Printer::Status st;
  for (uint32_t start = millis(); !(st = printer.status()).connected && millis() - start < WAIT_MS;) delay(100);
  // Printable pins of the 128-pin head per tape width (Brother raster command reference).
  int dots = 0, offset = 0;
  switch (st.tapeWidthMm) {
    case 4: dots = 24, offset = 52; break;  // 3.5 mm
    case 6: dots = 32, offset = 48; break;
    case 9: dots = 50, offset = 39; break;
    case 12: dots = 70, offset = 29; break;
    case 18: dots = 112, offset = 8; break;
    case 24: dots = 128, offset = 0; break;
  }
  if (!st.ready || !dots) {
    Serial.printf("Test print skipped: %s, tape %u mm\n", st.error ? st.error : "ready", st.tapeWidthMm);
    return;
  }

  // Built-in 5x7 font in 6x8 cells, scaled to fill the tape width. Debug lines stay small; cut off what does not fit.
  const int avail = dots - 2 * FRAME - 4;
  const int bodyScale = online ? std::clamp(avail / int(8 * (body.size() + 1)), 1, 3) : 1;
  if (!online) body.resize(std::clamp((avail - 16) / 8, 1, int(body.size())));
  const int titleScale = online ? bodyScale : std::clamp((avail - 8 * int(body.size())) / 8, 1, 2);
  int textLen = 6 * titleScale * title.length();
  for (const auto &b : body) textLen = std::max(textLen, 6 * bodyScale * int(b.length()));
  const int len = FRAME + MARGIN + textLen + MARGIN + STRIPES + FRAME;

  // Canvas x runs along the tape (one raster line per column), y across it.
  GFXcanvas1 canvas(len, dots);
  canvas.cp437(true);
  canvas.setTextWrap(false);
  canvas.setCursor(FRAME + MARGIN, std::max(0, (dots - 8 * titleScale - 8 * bodyScale * int(body.size())) / 2));
  canvas.setTextSize(titleScale);
  canvas.println(title);
  canvas.setTextSize(bodyScale);
  for (const auto &b : body) {
    canvas.setCursor(FRAME + MARGIN, canvas.getCursorY());
    canvas.println(b);
  }
  for (int x = len - FRAME - STRIPES; x < len; x++)
    for (int y = 0; y < dots; y++)
      if ((x + y) % 24 < 3) canvas.drawPixel(x, y, 1);
  for (int i = 0; i < FRAME; i++) canvas.drawRect(i, i, len - 2 * i, dots - 2 * i, 1);

  std::vector<uint8_t> job(100, 0x00);  // invalidate
  const uint8_t header[] = {
    0x1B, 0x40,              // initialize
    0x1B, 0x69, 0x61, 0x01,  // raster mode
    0x1B, 0x69, 0x7A, 0x84, 0x00, st.tapeWidthMm, 0x00,  // print info: width, then raster line count (LE32) ...
    uint8_t(len), uint8_t(len >> 8), 0x00, 0x00, 0x00, 0x00,  // ... starting page, 0
    0x1B, 0x69, 0x4D, 0x40,        // auto cut
    0x1B, 0x69, 0x4B, 0x08,        // no chain printing: feed and cut after this label
    0x1B, 0x69, 0x64, 0x0E, 0x00,  // 14 dots margin
    0x4D, 0x02,                    // TIFF (PackBits) compression
  };
  job.insert(job.end(), std::begin(header), std::end(header));
  for (int x = 0; x < len; x++) {
    uint8_t line[3 + 1 + 16] = {'G', 17, 0, 15};  // one literal run of 16 bytes
    for (int y = 0; y < dots; y++)
      if (canvas.getPixel(x, y)) line[4 + (offset + y) / 8] |= 0x80 >> ((offset + y) % 8);
    job.insert(job.end(), std::begin(line), std::end(line));
  }
  job.push_back(0x1A);  // print with feeding

  const char *error = printer.print(job.data(), job.size());
  Serial.printf("Test print: %s\n", error ? error : "printed");
}

// Own task, as loop() blocks for seconds.
static void ledTask(void *) {
  constexpr uint8_t MAX = 64;
  constexpr uint32_t BREATH_MS = 4000, BLINK_MS = 150, STEP_MS = 20;
  // The LED sits on GPIO48 (DevKitC-1 v1.0) or GPIO38 (v1.1): drive both.
  auto rgb = [](uint8_t r, uint8_t g, uint8_t b) { for (uint8_t pin : {48, 38}) rgbLedWrite(pin, r, g, b); };
  for (;;) {
    switch (Led s = led) {
      case Led::Unreachable: rgb(millis() / BLINK_MS % 2 ? MAX : 0, 0, 0); break;
      case Led::Idle: {
        float f = (1 - cosf(TWO_PI * (millis() % BREATH_MS) / BREATH_MS)) / 2;
        rgb(0, 0, MAX * f * f);  // squared: looks more even to the eye
        break;
      }
      case Led::Busy: rgb(0, MAX, 0); break;
      case Led::Done:
        for (int i = 0; i < 3; i++) {
          rgb(0, 0, 0);
          delay(BLINK_MS);
          rgb(0, MAX, 0);
          delay(BLINK_MS);
        }
        led.compare_exchange_strong(s, Led::Idle);  // unless the state changed meanwhile
        break;
      case Led::Printer: rgb(MAX, MAX / 2, 0); break;
      case Led::Error: rgb(MAX, 0, 0); break;
    }
    delay(STEP_MS);
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreate(ledTask, "led", 4096, nullptr, 1, nullptr);
  printer.begin();
  const char *configError = LittleFS.begin() ? config.load(LittleFS) : "LittleFS mount failed";
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { wifiReason = info.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  for (const auto &w : config.wifi) wifi.addAP(w.ssid.c_str(), w.password.c_str());
  if (!configError) wifi.run();  // first connection attempt, so the test print can show it
  // testPrint(configError);
  if (configError) {
    led = Led::Error;
    Serial.printf("Config: %s. Upload /config.json with `pio run -t uploadfs`\n", configError);
    for (;;) delay(1000);
  }
  backend.begin(config);
}

void loop() {
  constexpr uint32_t HEARTBEAT_MS = 15000, RETRY_MS = 5000;
  static uint32_t lastHeartbeat = millis() - HEARTBEAT_MS;
  static bool online = false;

  printer.poll();
  if (online != (wifi.run() == WL_CONNECTED)) {  // (re)connects to the strongest known network
    online = !online;
    led = online ? Led::Idle : Led::Unreachable;
    Serial.println(online ? "WiFi: " + WiFi.localIP().toString() : String("WiFi: disconnected"));
  }
  if (!online) {
    delay(200);
    return;
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    Printer::Status st = printer.status();
    int code = backend.heartbeat(st);
    if (code != 200) Serial.printf("Heartbeat: HTTP %d\n", code);
    led = code != 200 ? httpLed(code) : st.ready ? Led::Idle : Led::Printer;
  }

  if (pending) {
    const char *error = pending->error;
    int code = backend.report(pending->jobId, error ? "failed" : "printed", error);
    Serial.printf("Job %s %s: HTTP %d\n", pending->jobId.c_str(), error ? error : "printed", code);
    if (code == 200 || code == 404 || code == 409) pending.reset();  // 404/409: the backend will never accept it
    else delay(RETRY_MS);
    return;
  }

  Job job;
  int code = backend.nextJob(job);
  if (code == 200) {
    led = Led::Busy;
    handle(job);
  } else if (code != 204) {
    Serial.printf("Next job: HTTP %d\n", code);
    led = httpLed(code);
    delay(RETRY_MS);
  }
}
