import time

import pytest


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_ha_down_ssh_ok_no_pulse(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("H")
    qa.send("S")
    qa.send("h")
    qa.wait_for(r"HA: no reply", 15)
    qa.wait_for(r"SSH: banner OK", 10)
    qa.wait_for(r"HA down, SSH banner OK — skip reboot", 5)
    time.sleep(3)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
    assert not any("HA down and SSH wedged" in ln for ln in qa.lines)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_down_ssh_wedged_trips(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("h")
    qa.send("s")
    qa.wait_for(r"Wedge miss 5 / 5", 20)
    qa.wait_for(r"HA down and SSH wedged — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_no_ha_check_while_ping_fails(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("f")
    qa.wait_for(r"QA: ping inject FAIL", 3)
    start_ha = qa.count(r"^HA:")
    start_ssh = qa.count(r"^SSH:")
    time.sleep(6)
    assert qa.count(r"^HA:") == start_ha
    assert qa.count(r"^SSH:") == start_ssh
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
