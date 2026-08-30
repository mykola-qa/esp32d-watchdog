import os
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest


@pytest.fixture
def esp_http_url():
    host = os.environ.get("ESP32_IP")
    if not host:
        pytest.skip("set ESP32_IP to run read-only HTTP diagnostics tests")
    return f"http://{host}"


@pytest.mark.hardware
def test_http_logs_are_available_and_relay_endpoint_is_absent(esp_http_url):
    with urlopen(f"{esp_http_url}/", timeout=5) as response:
        page = response.read().decode("utf-8")
    assert "esp32d-watchdog" in page
    assert "Read-only diagnostics" in page
    assert "action=/t" not in page

    request = Request(f"{esp_http_url}/t", data=b"", method="POST")
    with pytest.raises(HTTPError) as error:
        urlopen(request, timeout=5)
    assert error.value.code == 404
