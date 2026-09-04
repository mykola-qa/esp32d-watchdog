"""Serial HIL helpers. Set ESP32_PORT to the connected ESP32 device."""

from __future__ import annotations

import os
import re
import time
import pytest
import serial
from serial.tools import list_ports

BAUD = 115200
BOOT_TIMEOUT_S = 45.0
DEFAULT_PORT = os.environ.get("ESP32_PORT")


def _port_exists(port: str) -> bool:
    if os.path.exists(port):
        return True
    return any(p.device == port for p in list_ports.comports())


@pytest.fixture(scope="session")
def serial_port():
    if not DEFAULT_PORT:
        pytest.skip("set ESP32_PORT to run serial HIL tests")
    if not _port_exists(DEFAULT_PORT):
        pytest.skip(f"no ESP32 on {DEFAULT_PORT}")
    return DEFAULT_PORT


class WatchdogSerial:
    def __init__(self, port: str):
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.buf = ""
        self.lines: list[str] = []
        self._wait_index = 0
        self.is_qa_fast = False

    def close(self) -> None:
        self.ser.close()

    def reset_esp(self) -> None:
        self.ser.dtr = False
        time.sleep(0.05)
        self.ser.dtr = True
        time.sleep(0.05)
        self.ser.dtr = False
        self.buf = ""
        self.lines.clear()
        self._wait_index = 0

    def _read(self) -> None:
        chunk = self.ser.read(self.ser.in_waiting or 1).decode("utf-8", "replace")
        if not chunk:
            return
        self.buf += chunk
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            line = line.strip("\r")
            if line:
                self.lines.append(line)

    def wait_for(self, pattern: str, timeout: float) -> str:
        """Wait for a new matching line, consuming events in arrival order."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._read()
            for index in range(self._wait_index, len(self.lines)):
                line = self.lines[index]
                if rx.search(line):
                    self._wait_index = index + 1
                    return line
            time.sleep(0.05)
        recent = "\n".join(list(self.lines)[-30:])
        raise AssertionError(
            f"timeout {timeout}s waiting for /{pattern}/\n--- recent ---\n{recent}"
        )

    def send(self, cmd: str) -> None:
        self.ser.write(f"{cmd}\n".encode("ascii"))
        self.ser.flush()

    def count(self, pattern: str) -> int:
        rx = re.compile(pattern)
        return sum(1 for line in self.lines if rx.search(line))

    def wait_boot(self) -> None:
        self.reset_esp()
        self.wait_for(r"esp32d-watchdog  ping \+ HA \+ host health", BOOT_TIMEOUT_S)
        self.is_qa_fast = any("BUILD QA_FAST" in ln for ln in self.lines)
        self.wait_for(r"State: Monitor", BOOT_TIMEOUT_S)


@pytest.fixture
def dut(serial_port: str):
    w = WatchdogSerial(serial_port)
    try:
        w.wait_boot()
        yield w
    finally:
        try:
            w.send("o")
            w.send("H")
            w.send("S")
            w.send("W")
        except Exception:
            pass
        w.close()


@pytest.fixture
def qa(dut: WatchdogSerial):
    if not dut.is_qa_fast:
        pytest.skip("flash [env:esp32dev_qa] for this test")
    return dut
