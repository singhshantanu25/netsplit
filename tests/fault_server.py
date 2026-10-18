#!/usr/bin/env python3
# Local test server that can misbehave on purpose.
# Usage: python3 fault_server.py <file> [port]
# URL:   http://127.0.0.1:8000/<mode>/<name>
# Modes: ok         correct 206 answers
#        ignore     ignores Range, sends 200 with the whole file
#        cap        sends at most 1 MB per request (Content-Range says so)
#        wrongstart sends data from byte 0, whatever start was asked
#        changed    ETag "v2" (file "changed" after the probe)
#        flaky      first request for each chunk: connection dropped halfway
#        stall      first request for each chunk: sends half, then goes silent
#                   (flaky/stall fail once per chunk per server run; restart
#                   the server to see the failures again)
# The probe (Range: bytes=0-0) always gets a correct answer with ETag "v1".

import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATH = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
SIZE = os.path.getsize(PATH)
CAP = 1024 * 1024

seen = set()              # (mode, start, end) already failed once
seen_lock = threading.Lock()


def first_time(key):
    with seen_lock:
        if key in seen:
            return False
        seen.add(key)
        return True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s  %s\n" % (self.command, self.path, self.headers.get("Range") or ""))

    def send_body(self, status, start, end, etag, content_range=True,
                  limit=None, then=None):
        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", '"%s"' % etag)
        self.send_header("Content-Length", str(end - start + 1))
        if content_range:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, SIZE))
        self.end_headers()
        if self.command == "HEAD":
            return
        with open(PATH, "rb") as f:
            f.seek(start)
            left = end - start + 1
            if limit is not None:
                left = min(left, limit)   # send only part of the body
            while left > 0:
                block = f.read(min(65536, left))
                if not block:
                    break
                try:
                    self.wfile.write(block)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(block)
        if then == "drop":
            self.close_connection = True   # hang up mid-body
        elif then == "stall":
            time.sleep(120)                # keep the connection open, send nothing
            self.close_connection = True

    def do_GET(self):
        parts = self.path.strip("/").split("/", 1)
        mode = parts[0] if parts else ""
        if mode not in ("ok", "ignore", "cap", "wrongstart", "changed",
                        "flaky", "stall"):
            self.send_error(404, "unknown mode")
            return

        m = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range") or "")
        if not m:
            self.send_body(200, 0, SIZE - 1, "v1", content_range=False)
            return
        start = int(m.group(1))
        end = int(m.group(2)) if m.group(2) else SIZE - 1
        end = min(end, SIZE - 1)
        if start >= SIZE or start > end:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % SIZE)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        # The probe is always answered correctly.
        if start == 0 and end == 0:
            self.send_body(206, 0, 0, "v1")
            return

        if mode == "ok":
            self.send_body(206, start, end, "v1")
        elif mode == "ignore":
            self.send_body(200, 0, SIZE - 1, "v1", content_range=False)
        elif mode == "cap":
            self.send_body(206, start, min(end, start + CAP - 1), "v1")
        elif mode == "wrongstart":
            length = end - start + 1
            self.send_body(206, 0, length - 1, "v1")
        elif mode == "changed":
            self.send_body(206, start, end, "v2")
        elif mode in ("flaky", "stall"):
            if first_time((mode, start, end)):
                half = (end - start + 1) // 2
                self.send_body(206, start, end, "v1", limit=half,
                               then="drop" if mode == "flaky" else "stall")
            else:
                self.send_body(206, start, end, "v1")

    do_HEAD = do_GET


if __name__ == "__main__":
    print("serving %s (%d bytes) on http://127.0.0.1:%d/<mode>/%s"
          % (PATH, SIZE, PORT, os.path.basename(PATH)))
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.daemon_threads = True   # Ctrl+C works even while a "stall" thread sleeps
    server.serve_forever()