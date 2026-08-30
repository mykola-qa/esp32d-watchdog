import time

import pytest


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_wifi_down_does_not_reboot(qa):
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("w")
    qa.wait_for(r"WiFi lost — will not reboot Pi", 8)
    time.sleep(8)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
    qa.send("W")


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_cooldown_blocks_second_pulse(qa):
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("f")
    qa.wait_for(r"State: PulseRelay", 25)
    qa.wait_for(r"State: Cooldown", 15)
    pulses = qa.count(r"State: PulseRelay")
    qa.send("t")
    qa.wait_for(r"TEST: ignored .* only available in Monitor", 3)
    time.sleep(3)
    assert qa.count(r"State: PulseRelay") == pulses
    assert qa.count(r"TEST: relay click") == 0
