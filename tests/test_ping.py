import time

import pytest


@pytest.mark.hardware
def test_ping_ok_when_pi_reachable(dut):
    dut.wait_for(r"Ping OK\s+\S+", 40)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ping_down_trips_after_threshold(qa):
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("f")
    qa.wait_for(r"Ping miss 5 / 5", 20)
    qa.wait_for(r"Ping down ~5 min — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)
    qa.wait_for(r"State: Cooldown", 15)


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_ping_blip_does_not_trip(qa):
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("f")
    qa.wait_for(r"Ping miss 2 / 5", 10)
    qa.send("o")
    qa.wait_for(r"QA: ping inject off", 3)
    qa.wait_for(r"Ping OK", 15)
    time.sleep(3)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
