#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <Adafruit_GFX.h>
#include <optional>
#include <vector>
#include "backend.h"
#include "config.h"
#include "printer.h"

static Config config;
static Printer printer;
static Backend backend;
static WiFiMulti wifi;

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
    return;
  }
  Printer::Status st = printer.status();
  if (!st.ready) {
    pending = Result{job.id, st.error};
    return;
  }
  // Claim first: the backend refuses (e.g. 409) jobs that were cancelled or already handed out.
  int code = backend.report(job.id, "printing");
  if (code != 200) {
    Serial.printf("Job %s not claimed (HTTP %d)\n", job.id.c_str(), code);
    return;
  }
  pending = Result{job.id, printer.print(job.data.get(), job.len)};
}

// Temporary: prints a framed label with the Wi-Fi state and a stripe pattern if the printer shows up at boot.
static void testPrint() {
  constexpr uint32_t WAIT_MS = 5000;
  constexpr int FRAME = 3, MARGIN = 10, STRIPES = 60;  // dots (180 dpi)
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

  std::vector<String> text = {"FUNDUS \x80" "A TOURNE"};  // \x80: CP437 capital C cedilla
  if (WiFi.status() == WL_CONNECTED) {
    text.push_back("WiFi: " + WiFi.SSID());
    text.push_back("Signal: " + String(WiFi.RSSI()) + " dBm, ch " + WiFi.channel());
    text.push_back("IP: " + WiFi.localIP().toString());
  } else {
    text.push_back("WiFi: not connected");
  }
  size_t chars = 0;
  for (const auto &t : text) chars = std::max(chars, t.length());
  // Built-in 5x7 font in 6x8 cells, scaled to fill the tape width.
  const int scale = std::clamp((dots - 2 * FRAME - 4) / int(8 * text.size()), 1, 3);
  const int lineH = 8 * scale, top = (dots - lineH * int(text.size())) / 2;
  const int len = FRAME + MARGIN + 6 * scale * int(chars) + MARGIN + STRIPES + FRAME;

  // Canvas x runs along the tape (one raster line per column), y across it.
  GFXcanvas1 canvas(len, dots);
  canvas.cp437(true);
  canvas.setTextWrap(false);
  canvas.setTextSize(scale);
  for (size_t i = 0; i < text.size(); i++) {
    canvas.setCursor(FRAME + MARGIN, top + lineH * i);
    canvas.print(text[i]);
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

// On-board RGB LED: blue while Wi-Fi is connected, else orange SOS. Own task, as loop() blocks for seconds.
static void ledTask(void *) {
  constexpr uint32_t UNIT_MS = 150;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      rgbLedWrite(RGB_BUILTIN, 0, 0, 64);
      delay(UNIT_MS);
      continue;
    }
    for (const char *p = "... --- ..."; *p; p++) {
      if (*p != ' ') {
        rgbLedWrite(RGB_BUILTIN, 64, 16, 0);
        delay((*p == '.' ? 1 : 3) * UNIT_MS);
        rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      }
      delay((*p == ' ' ? 2 : 1) * UNIT_MS);  // gap 1 within a letter, 3 between letters
    }
    delay(6 * UNIT_MS);  // gap 7 between words
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreate(ledTask, "led", 4096, nullptr, 1, nullptr);
  printer.begin();
  bool configured = LittleFS.begin() && config.load(LittleFS);
  WiFi.mode(WIFI_STA);
  for (const auto &w : config.wifi) wifi.addAP(w.ssid.c_str(), w.password.c_str());
  if (configured) wifi.run();  // first connection attempt, so the test print can show it
  testPrint();
  if (!configured) {
    Serial.println("No valid /config.json: upload it with `pio run -t uploadfs`");
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
    Serial.println(online ? "WiFi: " + WiFi.localIP().toString() : String("WiFi: disconnected"));
  }
  if (!online) {
    delay(200);
    return;
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    int code = backend.heartbeat(printer.status());
    if (code != 200) Serial.printf("Heartbeat: HTTP %d\n", code);
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
  if (code == 200) handle(job);
  else if (code != 204) {
    Serial.printf("Next job: HTTP %d\n", code);
    delay(RETRY_MS);
  }
}
