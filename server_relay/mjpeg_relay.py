#!/usr/bin/env python3
"""Lightweight MJPEG relay for the ESP32-S3 doorbell (A2 scheme).

The ESP32-S3 can only produce JPEG frames (no H.264 encoder), and MJPEG-in-RTMP
is not parseable by SRS/ffmpeg, so instead of RTMP the board pushes raw JPEG
frames here over one long-lived HTTP POST, and this relay fans them out to any
number of viewers as a standard multipart/x-mixed-replace MJPEG stream.

    publish (from ESP32):  POST /pub/<cam>     body = multipart/x-mixed-replace
    view    (from App):    GET  /stream/<cam>  ->  multipart/x-mixed-replace
    status:                GET  /status        ->  JSON

Pure standard library, no pip install. Run:  python3 mjpeg_relay.py [port]
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090


class Cam:
    def __init__(self):
        self.frame = None
        self.seq = 0
        self.last = 0.0
        self.cond = threading.Condition()

    def publish(self, data):
        with self.cond:
            self.frame = data
            self.seq += 1
            self.last = time.time()
            self.cond.notify_all()


_cams = {}
_cams_lock = threading.Lock()


def get_cam(name):
    with _cams_lock:
        cam = _cams.get(name)
        if cam is None:
            cam = Cam()
            _cams[name] = cam
        return cam


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    # ---- publisher (ESP32) ----
    def do_POST(self):
        if not self.path.startswith("/pub/"):
            self.send_error(404)
            return
        name = self.path[len("/pub/"):].strip("/")
        cam = get_cam(name)
        print(f"[pub] connected cam={name} from {self.client_address[0]}", flush=True)
        n = 0
        try:
            n = self._ingest(cam)
        except Exception as e:  # noqa: BLE001 - relay keeps running
            print(f"[pub] cam={name} error after {n} frames: {e}", flush=True)
        print(f"[pub] disconnected cam={name} ({n} frames)", flush=True)
        try:
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")
        except Exception:
            pass

    def _ingest(self, cam):
        rf = self.rfile
        frames = 0
        while True:
            line = rf.readline()
            if not line:
                break
            if not line.startswith(b"--"):  # skip until a boundary line
                continue
            clen = None
            while True:  # part headers
                h = rf.readline()
                if not h or h in (b"\r\n", b"\n"):
                    break
                key, _, val = h.partition(b":")
                if key.strip().lower() == b"content-length":
                    try:
                        clen = int(val.strip())
                    except ValueError:
                        clen = None
            if clen is None or clen <= 0 or clen > 8 * 1024 * 1024:
                continue
            data = rf.read(clen)
            if len(data) < clen:
                break
            cam.publish(data)
            frames += 1
        return frames

    # ---- viewer (App / browser) ----
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/status":
            with _cams_lock:
                st = {
                    n: {
                        "seq": c.seq,
                        "age_s": round(time.time() - c.last, 1) if c.last else None,
                    }
                    for n, c in _cams.items()
                }
            body = json.dumps(st).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        name = None
        if path.startswith("/stream/"):
            name = path[len("/stream/"):].strip("/")
        elif path.endswith(".mjpg"):
            name = path[1:-5]
        if not name:
            self.send_error(404)
            return

        cam = get_cam(name)
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        print(f"[sub] connected cam={name} from {self.client_address[0]}", flush=True)
        last = 0
        try:
            while True:
                with cam.cond:
                    if cam.seq == last:
                        cam.cond.wait(timeout=5)
                    if cam.seq == last:
                        continue  # keep-alive wait; loop again
                    last = cam.seq
                    data = cam.frame
                self.wfile.write(
                    b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"
                    % len(data)
                )
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
        except Exception as e:  # noqa: BLE001 - viewer just disconnected
            print(f"[sub] disconnected cam={name}: {e}", flush=True)


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(
        f"MJPEG relay on :{PORT}  publish: POST /pub/<cam>   view: GET /stream/<cam>",
        flush=True,
    )
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
