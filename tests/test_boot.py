import pytest


@pytest.mark.hardware
def test_boot_banner_and_no_bench(dut):
    assert any("esp32d-watchdog  ping + HA + SSH banner" in ln for ln in dut.lines)
    assert not any("BENCH: toggling" in ln for ln in dut.lines)


@pytest.mark.hardware
def test_boot_logs_no_automatic_pulse(dut):
    assert not any("State: PulseRelay" in ln for ln in dut.lines)
