#include "backend.h"
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>

void Backend::begin(const Config &cfg) {
  printerId = cfg.printerId;
  base = cfg.server + "/api/printers/" + cfg.printerId;
  esp_http_client_config_t c = {};
  c.url = base.c_str();
  c.timeout_ms = (POLL_WAIT_S + 15) * 1000;
  c.crt_bundle_attach = esp_crt_bundle_attach;  // verify TLS against the built-in root CA bundle
  c.event_handler = onEvent;
  c.user_data = this;
  http = esp_http_client_init(&c);
  esp_http_client_set_header(http, "Authorization", ("Bearer " + cfg.token).c_str());
}

esp_err_t Backend::onEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "X-Job-Id"))
    static_cast<Backend *>(e->user_data)->jobId = e->header_value;
  return ESP_OK;
}

int Backend::post(const String &path, const String &json) {
  esp_http_client_set_url(http, (base + path).c_str());
  esp_http_client_set_method(http, HTTP_METHOD_POST);
  esp_http_client_set_header(http, "Content-Type", "application/json");  // set_post_field(nullptr) deletes it
  esp_http_client_set_post_field(http, json.c_str(), json.length());
  esp_err_t err = esp_http_client_perform(http);
  esp_http_client_set_post_field(http, nullptr, 0);  // the field points into json
  return err == ESP_OK ? esp_http_client_get_status_code(http) : -1;
}

int Backend::heartbeat(const Printer::Status &st) {
  JsonDocument doc;
  doc["printerId"] = printerId;
  doc["espOnline"] = true;
  doc["printerConnected"] = st.connected;
  doc["printerReady"] = st.ready;
  doc["tapeWidthMm"] = st.tapeWidthMm;
  if (st.tapeColor >= 0) doc["tapeColor"] = st.tapeColor; else doc["tapeColor"] = nullptr;
  if (st.textColor >= 0) doc["textColor"] = st.textColor; else doc["textColor"] = nullptr;
  doc["printerError"] = st.error;
  doc["firmware"] = FIRMWARE;
  String json;
  serializeJson(doc, json);
  return post("/heartbeat", json);
}

int Backend::report(const String &id, const char *status, const char *error) {
  JsonDocument doc;
  doc["status"] = status;
  if (error) doc["error"] = error;
  String json;
  serializeJson(doc, json);
  return post("/jobs/" + id, json);
}

int Backend::nextJob(Job &job) {
  jobId = "";
  esp_http_client_set_url(http, (base + "/jobs/next?wait=" + POLL_WAIT_S).c_str());
  esp_http_client_set_method(http, HTTP_METHOD_GET);
  int status = -1;
  if (esp_http_client_open(http, 0) == ESP_OK) {
    int64_t len = esp_http_client_fetch_headers(http);
    status = esp_http_client_get_status_code(http);
    if (status == 200) {
      job.id = jobId;
      job.len = len > 0 ? len : 0;
      if (jobId.isEmpty() || len <= 0) status = -1;  // needs X-Job-Id and Content-Length
      else if (len <= MAX_JOB_BYTES) job.data.reset(new (std::nothrow) uint8_t[len]);
      for (size_t got = 0; status == 200 && job.data && got < job.len;) {
        int n = esp_http_client_read(http, reinterpret_cast<char *>(job.data.get()) + got, job.len - got);
        if (n <= 0) status = -1;
        else got += n;
      }
    }
  }
  esp_http_client_close(http);
  return status;
}
