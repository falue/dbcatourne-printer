# ESP ↔ backend protocol

What the backend must implement for this firmware. All requests go to `<server>/api/printers/<printerId>/…` over HTTPS, and every request carries `Authorization: Bearer <token>`. `server`, `printerId` and `token` come from the device's `config.json`. The server certificate must chain to a public root CA; the ESP checks it against the ESP-IDF root bundle.

## Heartbeat

`POST /heartbeat` roughly every 15–25 s:

```json
{
  "printerId": "P700-01",
  "espOnline": true,
  "printerConnected": true,
  "printerReady": true,
  "tapeWidthMm": 24,
  "printerError": null,
  "firmware": "0.1.0"
}
```

Reply `200`. The backend records the time and the **source public IP** of the request, which it uses for the same-network check. Consider the ESP offline after **60 s** without a heartbeat.

`printerError` is `null` when ready, or one of:

| Code | Meaning |
|---|---|
| `NOT_CONNECTED` | no printer on USB (unplugged, switched off, auto power-off) |
| `UNSUPPORTED_DEVICE` | a USB device other than a PT-P700 in printer mode, usually the P-Lite/EL switch |
| `NO_STATUS`, `USB_WRITE_FAILED` | the printer did not answer the status request |
| `PRINTER_BUSY` | the printer reports that it is printing |
| `NO_MEDIA`, `REPLACE_MEDIA`, `COVER_OPEN`, `CUTTER_JAM`, `OVERHEATING`, `WEAK_BATTERY`, `WRONG_ADAPTER`, `PRINTER_ERROR` | printer error flags |

The ESP does not know which tape is expected. The backend compares `tapeWidthMm` with its printer configuration and derives `WRONG_TAPE`.

## Next job

`GET /jobs/next?wait=10`

- `204`: no job. The server may hold the request for up to `wait` seconds before answering, which gives near-instant delivery without a request every second.
- `200`: one job, sent as follows.
  - Header `X-Job-Id: <unique id>`.
  - Header `Content-Length` (required, max 128 KiB).
  - Body `application/octet-stream`: the complete PT-P700 command stream for **one label** (invalidate, initialize, raster mode, raster lines, then print with feed `0x1A`). The ESP sends it to the printer byte for byte.

  Only offer jobs in state `queued`, oldest first. Handing out a job must not change its state. The ESP claims it explicitly.

## Job status

`POST /jobs/<jobId>` with a JSON body:

| Body | When | Transition |
|---|---|---|
| `{"status":"printing"}` | claim, sent right before the bytes go to USB | `queued → printing` |
| `{"status":"printed"}` | the printer reported "printing completed" | `printing → printed` |
| `{"status":"failed","error":"<code>"}` | see below | `queued/printing → failed` |

Reply `200` if accepted. Reply `409` if the transition is not allowed (for example a cancelled job, or a job already claimed or final), and `404` for unknown jobs. The ESP only prints after a `200` on the claim. A claim for a job that is already `printing` by this device must also return `409`, so that the job is never printed twice.

A job reaches `failed` for one of these reasons:

- The printer is not ready when the job arrives: `failed` straight from `queued`, with a `printerError` code.
- `JOB_TOO_LARGE`: the job is over 128 KiB or the ESP ran out of memory.
- `USB_WRITE_FAILED`, `PRINTER_OFF`, or a printer error flag during the print.
- `UNCONFIRMED`: all bytes were sent, but no completion status arrived within 60 s. A label may or may not exist. Do not retry automatically.

The ESP fetches no new job until the backend accepts the final `printed`/`failed` report. It retries that report every 5 s and drops it only on `404`/`409`.

## Backend-side timeouts

- A job that stays `printing` for longer than about 2 minutes means the ESP rebooted or lost its result. Mark it `failed` (for example with the error `LOST`). **Never requeue it automatically**, because it may already be printed. A person decides whether to retry.
- If the reply to a claim is lost in transit, the job stays `printing` and ends up `failed` through the timeout above. It is still visible and can be retried, and it is never printed twice.
