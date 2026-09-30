#pragma once
#include <Arduino.h>
#include <usb/usb_host.h>

// Brother PT-P700 on the ESP32-S3 USB OTG port (host mode).
// Not thread-safe: all calls must come from the same task.
class Printer {
public:
  struct Status {
    bool connected = false;
    bool ready = false;
    uint8_t tapeWidthMm = 0;
    const char *error = nullptr;  // machine-readable code, nullptr when ready
  };

  void begin();
  // Handles plug/unplug events. Cheap; call often.
  void poll();
  // Queries the printer (ESC i S). Prints nothing.
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
  uint8_t newAddr = 0;  // set by the client callback, handled in poll()
  bool gone = false;
  bool done = false;  // current transfer finished

  static void onClientEvent(const usb_host_client_event_msg_t *msg, void *arg);
  static void onTransferDone(usb_transfer_t *t);
  void pump(uint32_t ms);
  void open(uint8_t addr);
  void close();
  bool transfer(usb_transfer_t *t, uint32_t timeoutMs);
  bool write(const uint8_t *data, size_t len);
  bool readStatus(uint8_t *s, uint32_t timeoutMs);
};
