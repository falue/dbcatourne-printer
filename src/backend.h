#pragma once
#include "config.h"
#include "printer.h"
#include <esp_http_client.h>  // after Arduino headers (INADDR_NONE clash)
#include <memory>

struct Job {
  String id;
  std::unique_ptr<uint8_t[]> data;  // nullptr if the job exceeded MAX_JOB_BYTES
  size_t len = 0;
};

// HTTPS client for the printing backend (see PROTOCOL.md).
// Methods return the HTTP status code, or -1 on network/protocol errors.
class Backend {
public:
  void begin(const Config &cfg);
  int heartbeat(const Printer::Status &st);
  // Long-polls for the next job: 200 fills job, 204 means none.
  int nextJob(Job &job);
  // Sets a job's status: "printing" (claim), "printed" or "failed" (with error code).
  int report(const String &jobId, const char *status, const char *error = nullptr);

private:
  static constexpr const char *FIRMWARE = "0.2.0";
  static constexpr int POLL_WAIT_S = 10;  // server may hold /jobs/next this long
  static constexpr size_t MAX_JOB_BYTES = 128 * 1024;

  esp_http_client_handle_t http = nullptr;
  String base, printerId, jobId;  // jobId: X-Job-Id of the last response

  static esp_err_t onEvent(esp_http_client_event_t *e);
  int post(const String &path, const String &json);
};
