import pytest


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_fourth_pulse_in_window_suppressed(qa):
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("f")
    for i in range(3):
        qa.wait_for(r"State: PulseRelay", 30)
        qa.wait_for(r"State: Monitor", 25)
    qa.wait_for(r"Reboot budget exhausted", 30)
    pulses = qa.count(r"State: PulseRelay")
    assert pulses == 3
