import pytest


@pytest.mark.hardware
def test_serial_t_clicks_one_second(dut):
    dut.send("t")
    dut.wait_for(r"TEST: relay click 1000 ms", 5)
    dut.wait_for(r"TEST: relay idle", 5)
    assert not any("State: PulseRelay" in ln for ln in dut.lines)
