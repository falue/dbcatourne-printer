# Ça Tourne – PT-P700 Wi-Fi Printing

## Goal

Turn the existing **Brother PT-P700** into a small, battery-powered, network-aware label printer for the Ça Tourne web app.

The printer itself stays unchanged. An **ESP32-S3** acts as the Wi-Fi/USB bridge and receives prepared print jobs from the server.

The important design rule is:

> The ESP should be simple. The server does the rendering, permissions, queueing, and decision-making.

## Hardware

Initial target:

- Brother PT-P700
- ESP32-S3 with Wi-Fi and native USB host support
- one 12 V battery/power bank
- 12 V directly to the PT-P700
- 12 V → 5 V buck converter for the ESP32-S3
- ESP32-S3 USB host → PT-P700 USB
- common ground

The ESP provides USB host VBUS/data to the printer, while the printer's actual operating power comes from 12 V.

## Overall software architecture

```text
Ça Tourne frontend
        |
        | request print / probe printer
        v
Printing backend
        |
        +-- authentication + permissions
        +-- local-network check
        +-- printer state
        +-- print queue
        +-- HTML/CSS label templates
        +-- raster rendering
        +-- Brother PT-P700 command encoding
        |
        v
ESP32-S3
        |
        | USB
        v
Brother PT-P700
```

The web frontend never talks directly to the PT-P700.

## Label templates

Labels should be designed as **HTML/CSS templates** so they are easy to edit and preview in the browser.

Example data inserted into a template:

- prop ID
- prop name
- QR code
- category or other optional fields

The backend renders the HTML/CSS at the printer's exact pixel dimensions, converts it to a monochrome raster, then converts that raster into a **printer-ready Brother PT-P700 byte stream**.

The ESP does not render fonts, HTML, QR codes, PDFs, or BMP files.

It only transports already prepared printer data.

This means changing a label design does **not** require reflashing the ESP.

## Print job flow

1. User presses **Print label** in the authenticated Ça Tourne frontend.
2. Frontend asks the backend to create a print job.
3. Backend checks:
   - user permission
   - printer/ESP online state
   - whether printing is currently allowed
   - whether the requester appears to be on the same local network as the printer
4. Backend renders the HTML/CSS template.
5. Backend creates the PT-P700 raster command stream.
6. Job enters the printer queue.
7. ESP requests the next job.
8. ESP sends the binary stream to the PT-P700 over USB.
9. ESP reports success or failure.
10. Frontend receives/display the resulting status.

Possible job states:

```text
queued
printing
printed
failed
rejected
```

## Local-network restriction

For the initial version, determine local presence **server-side**.

The ESP periodically sends a heartbeat to the backend. The backend records the public IP address from which the heartbeat arrived.

When a browser requests a print job, the backend compares the browser request's public IP with the printer heartbeat's public IP.

If they match, the requester is considered to be on the same network.

This is intentionally a practical check, not cryptographic proof of Wi-Fi membership.

Do **not** trust a frontend-supplied value such as:

```json
{ "local": true }
```

The backend must determine this itself.

A stronger LAN-presence token can be added later if necessary.

## Printer probe / preflight

The frontend must be able to ask:

> **Can I print right now?**

This probe must **not print anything**.

Suggested endpoint conceptually:

```text
GET /api/printers/P700-01/probe
```

The result should be structured so the frontend can react appropriately.

Example success:

```json
{
  "allowed": true,
  "printer": "P700-01",
  "espOnline": true,
  "printerOnline": true,
  "localNetwork": true,
  "ready": true,
  "tapeWidthMm": 24
}
```

Examples of refusal/failure reasons:

```text
NOT_LOCAL_NETWORK
ESP_OFFLINE
PRINTER_OFFLINE
PRINTER_BUSY
PRINTER_ERROR
WRONG_TAPE
NOT_AUTHORIZED
SERVER_ERROR
```

Example refusal:

```json
{
  "allowed": false,
  "code": "NOT_LOCAL_NETWORK",
  "message": "Printing is only available from the warehouse network."
}
```

The frontend should use the machine-readable `code` for behavior and the human-readable `message` for display.

## ESP heartbeat

The ESP should periodically report its state, for example:

```json
{
  "printerId": "P700-01",
  "espOnline": true,
  "printerConnected": true,
  "printerReady": true,
  "tapeWidthMm": 24,
  "printerError": null
}
```

The backend treats the ESP as offline if no heartbeat has been received within a defined timeout.

The ESP should also query the PT-P700 status where practical so the backend can distinguish:

- ESP offline
- ESP online but printer disconnected
- printer error
- wrong tape width
- printer ready

## ESP responsibilities

Keep the ESP firmware deliberately small.

It should:

- boot quickly
- connect to Wi-Fi
- authenticate itself to the backend
- detect the PT-P700 over USB
- send regular heartbeats
- poll/wait for print jobs
- receive printer-ready binary data
- send that data to the PT-P700
- read printer status
- report printed / failed
- retry network communication safely

It should **not** contain:

- label templates
- fonts
- prop database logic
- permissions
- HTML rendering
- QR generation
- translation logic

## Backend responsibilities

The printing backend owns:

- device authentication
- user authorization
- printer configuration
- same-network check
- heartbeat tracking
- printer probe/preflight
- print queue
- print-job status
- HTML/CSS templates
- browser preview
- QR/barcode generation
- monochrome rasterization
- PT-P700 encoding
- logging/errors

The printer-specific encoder should be isolated behind an adapter so another printer can replace the PT-P700 later without changing the rest of the application.

Conceptually:

```text
renderLabel(template, data)
        |
        v
monochrome raster
        |
        v
printerAdapter.encode(raster)
        |
        v
binary print job
```

## Reliability rules

- A print request must never disappear silently.
- Do not mark a job `printed` merely because the bytes were sent; use printer status/confirmation where possible.
- Duplicate delivery must not cause accidental duplicate labels.
- Jobs need unique IDs.
- The ESP should acknowledge jobs explicitly.
- A failed job remains inspectable/retryable.
- The frontend should always be able to distinguish a permission/network refusal from a hardware failure.

##  Reference implementattions

GitHub: https://github.com/aalarchiv/labelsis


## Initial scope

Start with:

- one PT-P700
- one ESP32-S3
- 24 mm tape
- one label template
- one printer queue
- local-network printing only
- printer probe/preflight
- simple success/error feedback

Keep the architecture ready for multiple printers and templates later, but do not over-engineer the first version.
