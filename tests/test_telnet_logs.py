import os
import socket

import pytest


@pytest.fixture
def esp_ip():
    host = os.environ.get("ESP32_IP")
    if not host:
        pytest.skip("set ESP32_IP to run telnet diagnostics tests")
    return host


@pytest.mark.hardware
def test_telnet_logs_are_available_and_http_is_absent(esp_ip):
    with socket.create_connection((esp_ip, 23), timeout=5) as client:
        client.settimeout(5)
        data = b""
        while b"esp32d-watchdog" not in data and len(data) < 8192:
            chunk = client.recv(1024)
            if not chunk:
                break
            data += chunk
        text = data.decode("utf-8", "replace")
    assert "esp32d-watchdog" in text
    assert "read-only diagnostics" in text.lower()

    with pytest.raises((OSError, TimeoutError)):
        socket.create_connection((esp_ip, 80), timeout=3)
