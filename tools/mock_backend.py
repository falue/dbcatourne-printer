#!/usr/bin/env python3
"""Minimal stand-in for the printing backend (see PROTOCOL.md), for testing the ESP on the LAN.

Run it, set "server" in data/config.json to the printed URL and upload it (pio run -t uploadfs).
Logs every heartbeat and job report. Queue a label by dropping a .bin file (PT-P700 command stream)
into jobs/, or press Enter to queue a generated test label for the tape width from the last heartbeat.
No auth, no TLS, one printer: for testing only.
"""
import json
import re
import socket
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

PORT = 8000
JOBS = Path(__file__).parent / "jobs"
DONE = JOBS / "done"

states = {}  # job id (file name) -> "printing"; files in JOBS without an entry are queued
tape_mm = 24
lock = threading.Lock()


def log(msg):
    print(f"{datetime.now():%H:%M:%S} {msg}", flush=True)


def queued():
    with lock:
        files = [f for f in JOBS.glob("*.bin") if f.name not in states]
    return min(files, key=lambda f: f.stat().st_mtime, default=None)


def test_label(width_mm, length=180):
    """Framed stripe pattern, same command stream as the firmware's boot test print."""
    pins = {4: (24, 52), 6: (32, 48), 9: (50, 39), 12: (70, 29), 18: (112, 8), 24: (128, 0)}
    dots, offset = pins[width_mm]
    out = bytearray(100) + b"\x1b@" + b"\x1bia\x01"  # invalidate, initialize, raster mode
    out += b"\x1biz\x84\x00" + bytes([width_mm, 0]) + length.to_bytes(4, "little") + b"\x00\x00"  # print info
    out += b"\x1biM\x40" + b"\x1biK\x08" + b"\x1bid\x0e\x00" + b"M\x02"  # auto cut, no chain, margin, PackBits
    for x in range(length):
        line = bytearray(16)
        for y in range(dots):
            if x < 3 or x >= length - 3 or y < 3 or y >= dots - 3 or (x + y) % 24 < 3:
                line[(offset + y) // 8] |= 0x80 >> ((offset + y) % 8)
        out += b"G\x11\x00\x0f" + line  # one literal PackBits run of 16 bytes
    return bytes(out + b"\x1a")  # print with feeding


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if code != 204:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def json_body(self):
        return json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")

    def do_GET(self):
        url = urlparse(self.path)
        if not re.fullmatch(r"/api/printers/[^/]+/jobs/next", url.path):
            return self.reply(404)
        deadline = time.time() + int(parse_qs(url.query).get("wait", ["0"])[0])
        while not (job := queued()) and time.time() < deadline:
            time.sleep(0.5)
        if not job:
            return self.reply(204)
        log(f"job {job.name}: handed out ({job.stat().st_size} bytes)")
        self.reply(200, job.read_bytes(), {"X-Job-Id": job.name, "Content-Type": "application/octet-stream"})

    def do_POST(self):
        global tape_mm
        if re.fullmatch(r"/api/printers/[^/]+/heartbeat", self.path):
            hb = self.json_body()
            tape_mm = hb.get("tapeWidthMm") or tape_mm
            state = "ready" if hb.get("printerReady") else hb.get("printerError")
            log(f"heartbeat: printer {'connected' if hb.get('printerConnected') else 'NOT connected'}, "
                f"{state}, tape {hb.get('tapeWidthMm')} mm, firmware {hb.get('firmware')}")
            return self.reply(200)
        if not (m := re.fullmatch(r"/api/printers/[^/]+/jobs/([^/]+)", self.path)):
            return self.reply(404)
        job_id, body = m[1], self.json_body()
        status, file = body.get("status"), JOBS / m[1]
        with lock:
            if not file.exists():
                code = 404
            elif status == "printing":
                code = 409 if job_id in states else 200
                states[job_id] = "printing"
            elif status in ("printed", "failed"):
                code = 200
                suffix = "printed" if status == "printed" else f"failed-{body.get('error')}"
                file.rename(DONE / f"{job_id}.{suffix}")
                states.pop(job_id, None)
            else:
                code = 400
        log(f"job {job_id}: {status}{' ' + body['error'] if body.get('error') else ''} -> HTTP {code}")
        self.reply(code)

    def log_message(self, *args):  # silence the default per-request log
        pass


def queue_test_labels():
    while True:
        input()
        name = f"test-{datetime.now():%H%M%S}.bin"
        (JOBS / name).write_bytes(test_label(tape_mm))
        log(f"job {name}: queued test label for {tape_mm} mm tape")


if __name__ == "__main__":
    DONE.mkdir(parents=True, exist_ok=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 80))  # no packet is sent; picks the LAN interface
        ip = s.getsockname()[0]
    log(f'mock backend: set "server": "http://{ip}:{PORT}" in data/config.json')
    log(f"drop .bin jobs into {JOBS}, or press Enter to queue a test label")
    threading.Thread(target=queue_test_labels, daemon=True).start()
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
