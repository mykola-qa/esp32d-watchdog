#!/usr/bin/env python3
"""Liveness canary for esp32d-watchdog. Bind to LAN; do not expose to the internet."""

import re
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8081


def remote_login_count(output):
    return sum(1 for line in output.splitlines() if re.search(r"\([^()]+\)\s*$", line))


def login_count():
    try:
        out = subprocess.check_output(["who"], timeout=2, text=True)
    except Exception:
        return -1
    return remote_login_count(out)


class HealthHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("OK ssh=%d\n" % login_count()).encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


if __name__ == "__main__":
    HTTPServer(("0.0.0.0", PORT), HealthHandler).serve_forever()
