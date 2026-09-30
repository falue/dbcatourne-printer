#include "printer.h"
#include <esp_intr_alloc.h>
#include <usb/usb_helpers.h>

// 32-byte status message (Brother raster command reference, "Status information").
enum : uint8_t { S_ERROR1 = 8, S_ERROR2 = 9, S_MEDIA_WIDTH = 10, S_TYPE = 18, S_PHASE = 19 };
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

static void libTask(void *) {
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
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
  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &xferIn));  // one full-speed bulk packet
  ESP_ERROR_CHECK(usb_host_transfer_alloc(OUT_CHUNK, 0, &xferOut));
  for (auto *t : {xferIn, xferOut}) {
    t->callback = onTransferDone;
    t->context = this;
  }
}

void Printer::onClientEvent(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<Printer *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) self->newAddr = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) self->gone = true;
}

void Printer::onTransferDone(usb_transfer_t *t) {
  static_cast<Printer *>(t->context)->done = true;
}

void Printer::pump(uint32_t ms) {
  usb_host_client_handle_events(client, pdMS_TO_TICKS(ms));
}

void Printer::poll() {
  pump(0);
  if (gone) close();
  if (newAddr && !dev) open(newAddr);
  newAddr = 0;
}

void Printer::open(uint8_t addr) {
  if (usb_host_device_open(client, addr, &dev) != ESP_OK) {
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
      Serial.println("USB: PT-P700 connected");
    }
    return;
  }
}

void Printer::close() {
  if (dev) {
    if (iface >= 0) usb_host_interface_release(client, dev, iface);
    usb_host_device_close(client, dev);
    Serial.println("USB: device removed");
  }
  dev = nullptr;
  iface = -1;
  epIn = epOut = 0;
  gone = false;
}

// The host stack has no transfer timeouts: on expiry the endpoint is halted and flushed to cancel.
bool Printer::transfer(usb_transfer_t *t, uint32_t timeoutMs) {
  done = false;
  t->device_handle = dev;
  if (usb_host_transfer_submit(t) != ESP_OK) return false;
  for (uint32_t start = millis(); !done && millis() - start < timeoutMs;) pump(10);
  if (!done) {
    usb_host_endpoint_halt(dev, t->bEndpointAddress);
    usb_host_endpoint_flush(dev, t->bEndpointAddress);
    while (!done) pump(10);
    usb_host_endpoint_clear(dev, t->bEndpointAddress);
    return false;
  }
  return t->status == USB_TRANSFER_STATUS_COMPLETED;
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
    pump(20);
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
    Status st = {.connected = true, .tapeWidthMm = s[S_MEDIA_WIDTH], .error = errorCode(s)};
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
