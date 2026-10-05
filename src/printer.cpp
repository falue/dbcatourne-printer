#include "printer.h"
#include <esp_intr_alloc.h>
#include <usb/usb_helpers.h>

// 32-byte status message (Brother raster command reference, "Status information").
enum : uint8_t {
  S_ERROR1 = 8, S_ERROR2 = 9, S_MEDIA_WIDTH = 10, S_MEDIA_TYPE = 11, S_TYPE = 18, S_PHASE = 19,
  S_TAPE_COLOR = 24, S_TEXT_COLOR = 25,
};
enum : uint8_t { TYPE_REPLY = 0x00, TYPE_COMPLETED = 0x01, TYPE_ERROR = 0x02, TYPE_OFF = 0x04 };

static const char *errorCode(const uint8_t *s) {
  static constexpr struct {
    uint8_t byte, mask;
    const char *code;
  } ERRORS[] = {
    {S_ERROR1, 0x01, "NO_MEDIA"},      {S_ERROR1, 0x04, "CUTTER_JAM"},   {S_ERROR1, 0x08, "WEAK_BATTERY"},
    {S_ERROR1, 0x40, "WRONG_ADAPTER"}, {S_ERROR2, 0x01, "REPLACE_MEDIA"}, {S_ERROR2, 0x10, "COVER_OPEN"},
    {S_ERROR2, 0x20, "OVERHEATING"},
  };
  for (const auto &e : ERRORS)
    if (s[e.byte] & e.mask) return e.code;
  return s[S_ERROR1] | s[S_ERROR2] ? "PRINTER_ERROR" : nullptr;
}

// Code tables (4), (8) and (9) of the PT-H500/P700/E500 raster command reference v1.11.
struct Name {
  uint8_t code;
  const char *name;
};
static constexpr Name MEDIA_TYPES[] = {
  {0x00, "no media"}, {0x01, "laminated"}, {0x03, "non-laminated"}, {0x11, "heat-shrink tube HS 2:1"},
  {0x17, "heat-shrink tube HS 3:1"}, {0xFF, "incompatible"},
};
static constexpr Name TAPE_COLORS[] = {
  {0x01, "white"}, {0x02, "other"}, {0x03, "clear"}, {0x04, "red"}, {0x05, "blue"}, {0x06, "yellow"},
  {0x07, "green"}, {0x08, "black"}, {0x09, "clear (white text)"}, {0x20, "matte white"}, {0x21, "matte clear"},
  {0x22, "matte silver"}, {0x23, "satin gold"}, {0x24, "satin silver"}, {0x30, "blue (D)"}, {0x31, "red (D)"},
  {0x40, "fluorescent orange"}, {0x41, "fluorescent yellow"}, {0x50, "berry pink (S)"}, {0x51, "light gray (S)"},
  {0x52, "lime green (S)"}, {0x60, "yellow (F)"}, {0x61, "pink (F)"}, {0x62, "blue (F)"},
  {0x70, "white (heat-shrink tube)"}, {0x90, "white (flexible ID)"}, {0x91, "yellow (flexible ID)"},
  {0xF0, "cleaning"}, {0xF1, "stencil"}, {0xFF, "incompatible"},
};
static constexpr Name TEXT_COLORS[] = {
  {0x01, "white"}, {0x02, "other"}, {0x04, "red"}, {0x05, "blue"}, {0x08, "black"}, {0x0A, "gold"},
  {0x62, "blue (F)"}, {0xF0, "cleaning"}, {0xF1, "stencil"}, {0xFF, "incompatible"},
};

template <size_t N>
static const char *nameOf(const Name (&table)[N], uint8_t code) {
  for (const auto &e : table)
    if (e.code == code) return e.name;
  return "unknown";
}

static void logTape(const uint8_t *s) {
  Serial.printf("Tape: %s mm, %s, %s tape, %s text [type %02X, tape %02X, text %02X]\n",
                s[S_MEDIA_WIDTH] == 4 ? "3.5" : String(s[S_MEDIA_WIDTH]).c_str(), nameOf(MEDIA_TYPES, s[S_MEDIA_TYPE]),
                nameOf(TAPE_COLORS, s[S_TAPE_COLOR]), nameOf(TEXT_COLORS, s[S_TEXT_COLOR]), s[S_MEDIA_TYPE],
                s[S_TAPE_COLOR], s[S_TEXT_COLOR]);
}

static void libTask(void *) {
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

// Delivers client events and transfer callbacks, independent of how long the caller's loop blocks.
static void clientTask(void *client) {
  for (;;) usb_host_client_handle_events(static_cast<usb_host_client_handle_t>(client), portMAX_DELAY);
}

void Printer::begin() {
  const usb_host_config_t host = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_ERROR_CHECK(usb_host_install(&host));
  xTaskCreate(libTask, "usb_lib", 4096, nullptr, 2, nullptr);
  const usb_host_client_config_t cfg = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {.client_event_callback = onClientEvent, .callback_arg = this},
  };
  ESP_ERROR_CHECK(usb_host_client_register(&cfg, &client));
  done = xSemaphoreCreateBinary();
  xTaskCreate(clientTask, "usb_client", 4096, client, 2, nullptr);
  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &xferIn));  // one full-speed bulk packet
  ESP_ERROR_CHECK(usb_host_transfer_alloc(OUT_CHUNK, 0, &xferOut));
  for (auto *t : {xferIn, xferOut}) {
    t->callback = onTransferDone;
    t->context = this;
  }
}

void Printer::onClientEvent(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<Printer *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    Serial.printf("USB: [%lu ms] device attached (address %u)\n", millis(), msg->new_dev.address);
    self->newAddr = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    Serial.printf("USB: [%lu ms] device detached\n", millis());
    self->gone = true;
  }
}

void Printer::onTransferDone(usb_transfer_t *t) {
  xSemaphoreGive(static_cast<Printer *>(t->context)->done);
}

void Printer::poll() {
  if (gone) close();
  if (uint8_t addr = newAddr.exchange(0); addr && !dev) open(addr);
}

void Printer::open(uint8_t addr) {
  if (usb_host_device_open(client, addr, &dev) != ESP_OK) {
    Serial.printf("USB: [%lu ms] device %u gone before it could be opened\n", millis(), addr);
    dev = nullptr;
    return;
  }
  const usb_device_desc_t *dd;
  usb_host_get_device_descriptor(dev, &dd);
  if (dd->idVendor != VID || dd->idProduct != PID) {
    // Kept open (unclaimed) so status() can report it and DEV_GONE still arrives.
    Serial.printf("USB: %04x:%04x is not a PT-P700 in printer mode (P-Lite/EL switch off?)\n", dd->idVendor, dd->idProduct);
    return;
  }
  const usb_config_desc_t *cfg;
  usb_host_get_active_config_descriptor(dev, &cfg);
  for (uint8_t n = 0; n < cfg->bNumInterfaces; n++) {
    int off = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, n, 0, &off);
    if (!intf || intf->bInterfaceClass != USB_CLASS_PRINTER) continue;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
      int epOff = off;
      const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &epOff);
      if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) continue;
      (USB_EP_DESC_GET_EP_DIR(ep) ? epIn : epOut) = ep->bEndpointAddress;
    }
    if (epIn && epOut && usb_host_interface_claim(client, dev, n, 0) == ESP_OK) {
      iface = n;
      Serial.printf("USB: [%lu ms] PT-P700 connected\n", millis());
    }
    return;
  }
}

void Printer::close() {
  if (dev) {
    if (iface >= 0) usb_host_interface_release(client, dev, iface);
    usb_host_device_close(client, dev);
    Serial.printf("USB: [%lu ms] device closed\n", millis());
  }
  dev = nullptr;
  iface = -1;
  epIn = epOut = 0;
  gone = false;
}

// The host stack has no transfer timeouts: on expiry the endpoint is halted and flushed to cancel.
bool Printer::transfer(usb_transfer_t *t, uint32_t timeoutMs) {
  t->device_handle = dev;
  if (esp_err_t err = usb_host_transfer_submit(t)) {
    Serial.printf("USB: [%lu ms] submit on EP %02X failed: %s\n", millis(), t->bEndpointAddress, esp_err_to_name(err));
    return false;
  }
  if (!xSemaphoreTake(done, pdMS_TO_TICKS(timeoutMs))) {
    usb_host_endpoint_halt(dev, t->bEndpointAddress);
    usb_host_endpoint_flush(dev, t->bEndpointAddress);
    xSemaphoreTake(done, portMAX_DELAY);  // the flush completes the transfer as canceled
    usb_host_endpoint_clear(dev, t->bEndpointAddress);
    return false;
  }
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
    static const char *const NAMES[] = {"completed", "error", "timed out", "canceled", "stall", "overflow", "skipped", "no device"};
    Serial.printf("USB: [%lu ms] transfer on EP %02X failed: %s\n", millis(), t->bEndpointAddress,
                  t->status < std::size(NAMES) ? NAMES[t->status] : "unknown");
    return false;
  }
  return true;
}

bool Printer::write(const uint8_t *data, size_t len) {
  xferOut->bEndpointAddress = epOut;
  for (size_t off = 0; off < len;) {
    size_t n = std::min(len - off, OUT_CHUNK);
    memcpy(xferOut->data_buffer, data + off, n);
    xferOut->num_bytes = n;
    if (!transfer(xferOut, WRITE_TIMEOUT_MS)) return false;
    off += n;
  }
  return true;
}

// Reads the next 32-byte status message. The printer may answer IN requests with empty packets meanwhile.
bool Printer::readStatus(uint8_t *s, uint32_t timeoutMs) {
  xferIn->bEndpointAddress = epIn;
  for (uint32_t start = millis(), elapsed; (elapsed = millis() - start) < timeoutMs;) {
    xferIn->num_bytes = xferIn->data_buffer_size;
    if (!transfer(xferIn, timeoutMs - elapsed)) return false;
    const uint8_t *d = xferIn->data_buffer;
    if (xferIn->actual_num_bytes == 32 && d[0] == 0x80 && d[1] == 0x20) {
      memcpy(s, d, 32);
      return true;
    }
    delay(20);
  }
  return false;
}

Printer::Status Printer::status() {
  poll();
  if (!dev) return {.error = "NOT_CONNECTED"};
  if (iface < 0) return {.connected = true, .error = "UNSUPPORTED_DEVICE"};

  uint8_t req[105] = {};  // invalidate (100 x 0x00), initialize (ESC @), status request (ESC i S)
  memcpy(req + 100, "\x1B\x40\x1B\x69\x53", 5);
  if (!write(req, sizeof req)) return {.connected = true, .error = "USB_WRITE_FAILED"};
  uint8_t s[32];
  while (readStatus(s, STATUS_TIMEOUT_MS)) {
    if (s[S_TYPE] != TYPE_REPLY) continue;  // stale notification
    uint32_t tape = s[S_MEDIA_WIDTH] | s[S_MEDIA_TYPE] << 8 | s[S_TAPE_COLOR] << 16 | s[S_TEXT_COLOR] << 24;
    if (tape != lastTape) logTape(s);
    lastTape = tape;
    Status st = {.connected = true, .tapeWidthMm = s[S_MEDIA_WIDTH], .tapeColor = s[S_TAPE_COLOR],
                 .textColor = s[S_TEXT_COLOR], .error = errorCode(s)};
    if (!st.error && s[S_PHASE] != 0) st.error = "PRINTER_BUSY";
    st.ready = !st.error;
    return st;
  }
  return {.connected = true, .error = "NO_STATUS"};
}

const char *Printer::print(const uint8_t *data, size_t len) {
  if (iface < 0) return "NOT_CONNECTED";
  if (!write(data, len)) return "USB_WRITE_FAILED";
  uint8_t s[32];
  while (readStatus(s, PRINT_TIMEOUT_MS)) {
    if (s[S_TYPE] == TYPE_COMPLETED) return nullptr;
    if (s[S_TYPE] == TYPE_ERROR) {
      const char *error = errorCode(s);
      return error ? error : "PRINTER_ERROR";
    }
    if (s[S_TYPE] == TYPE_OFF) return "PRINTER_OFF";
  }
  return "UNCONFIRMED";  // bytes sent, but no completion status: the label may or may not exist
}
