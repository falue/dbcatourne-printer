#pragma once
#include <Arduino.h>
#include <usb/usb_host.h>
#include <atomic>
#include <mutex>

// Brother PT-P700 on the ESP32-S3 USB OTG port (host mode).
// Thread-safe: calls run one at a time, so a print blocks status() until it ends. USB events are handled by an internal task.
class Printer {
public:
  struct Status {
    bool connected = false;
    bool ready = false;
    uint8_t tapeWidthMm = 0;
    int16_t tapeColor = -1;  // Brother colour codes (status bytes 24/25), -1 when unknown
    int16_t textColor = -1;
    const char *error = nullptr;  // machine-readable code, nullptr when ready
    bool operator==(const Status &) const = default;
  };

  void begin();
  // Handles plug/unplug, then queries the printer (ESC i S). Prints nothing.
  Status status();
  // Sends a printer-ready byte stream (one label) and waits for the printer's
  // "printing completed" status. Returns nullptr on success, else an error code.
  const char *print(const uint8_t *data, size_t len);

private:
  static constexpr uint16_t VID = 0x04F9, PID = 0x2061;  // PT-P700 in printer mode
  static constexpr size_t OUT_CHUNK = 4096;
  static constexpr uint32_t WRITE_TIMEOUT_MS = 5000, STATUS_TIMEOUT_MS = 1000, PRINT_TIMEOUT_MS = 60000;

  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t dev = nullptr;
  int iface = -1;  // claimed printer interface, -1 if the device is not a usable PT-P700
  uint8_t epIn = 0, epOut = 0;
  usb_transfer_t *xferIn = nullptr, *xferOut = nullptr;
  std::atomic<uint8_t> newAddr = 0;  // set by the client callback, handled in poll()
  std::atomic<bool> gone = false;
  std::mutex mutex;  // serializes status() and print()
  SemaphoreHandle_t done = nullptr;  // given when the current transfer finished
  uint32_t lastTape = UINT32_MAX;  // width, type and colors from the last status, to log changes

  static void onClientEvent(const usb_host_client_event_msg_t *msg, void *arg);
  static void onTransferDone(usb_transfer_t *t);
  void poll();  // opens/closes the printer after plug/unplug events
  void open(uint8_t addr);
  void close();
  bool transfer(usb_transfer_t *t, uint32_t timeoutMs);
  bool write(const uint8_t *data, size_t len);
  bool readStatus(uint8_t *s, uint32_t timeoutMs);
};
