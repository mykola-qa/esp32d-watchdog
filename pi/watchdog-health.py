#!/usr/bin/env python3
"""Liveness canary for esp32d-watchdog. Bind to LAN; do not expose to the internet."""

from http.server import BaseHTTPRequestHandler, HTTPServer

BODY = b"OK\n"
PORT = 8081


class HealthHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, format, *args):
        return


if __name__ == "__main__":
    HTTPServer(("0.0.0.0", PORT), HealthHandler).serve_forever()
