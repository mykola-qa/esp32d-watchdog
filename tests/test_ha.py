import time

import pytest


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_ha_down_health_and_ssh_ok_no_pulse(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("H")
    qa.send("S")
    qa.send("K")
    qa.send("N")
    qa.send("h")
    qa.wait_for(r"HA: no reply", 15)
    qa.wait_for(r"Health: HTTP", 10)
    qa.wait_for(r"SSH: KEXINIT OK", 10)
    qa.wait_for(r"HA down, host health and SSH OK — update miss", 5)
    time.sleep(3)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
    assert not any("HA down and host wedged" in ln for ln in qa.lines)
    assert not any("HA down ~30 min" in ln for ln in qa.lines)
    assert not any("HA HTTP timeout" in ln for ln in qa.lines)


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_unknown_login_state_uses_long_grace(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("x")
    qa.send("h")
    qa.wait_for(r"update miss 1 / 8 \(ssh \?\)", 15)
    time.sleep(3)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_login_policy_change_restarts_grace_counter(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("N")
    qa.send("h")
    qa.wait_for(r"update miss 2 / 8 \(ssh 1\)", 15)

    qa.send("n")
    qa.wait_for(r"update miss 1 / 5 \(ssh 0\)", 10)

    qa.send("N")
    qa.wait_for(r"update miss 1 / 8 \(ssh 1\)", 10)
    assert not any("State: PulseRelay" in ln for ln in qa.lines)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_down_health_fail_trips(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("h")
    qa.send("s")
    qa.wait_for(r"Wedge miss 5 / 5", 25)
    qa.wait_for(r"HA down and host wedged — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_down_ssh_fail_trips(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("h")
    qa.send("k")
    qa.wait_for(r"health OK SSH fail", 25)
    qa.wait_for(r"Wedge miss 5 / 5", 25)
    qa.wait_for(r"HA down and host wedged — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_timeout_trips(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("u")
    qa.wait_for(r"HA timeout miss 5 / 5", 20)
    qa.wait_for(r"HA HTTP timeout — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_down_no_login_trips_after_short_grace(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("n")
    qa.send("h")
    qa.wait_for(r"update miss 5 / 5", 30)
    qa.wait_for(r"HA down ~5 min \(no SSH login\) — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
@pytest.mark.relay_pulse
def test_ha_down_logged_in_trips_after_long_grace(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("B")
    qa.wait_for(r"QA: budget reset", 3)
    qa.send("N")
    qa.send("h")
    qa.wait_for(r"update miss 8 / 8", 40)
    qa.wait_for(r"HA down ~30 min — rebooting Pi", 5)
    qa.wait_for(r"State: PulseRelay", 5)


@pytest.mark.hardware
@pytest.mark.qa_fast
def test_no_ha_check_while_ping_fails(qa):
    qa.wait_for(r"Ping OK", 40)
    qa.send("f")
    qa.wait_for(r"QA: ping inject FAIL", 3)
    start_ha = qa.count(r"^HA:")
    start_health = qa.count(r"^Health:")
    start_ssh = qa.count(r"^SSH:")
    time.sleep(6)
    assert qa.count(r"^HA:") == start_ha
    assert qa.count(r"^Health:") == start_health
    assert qa.count(r"^SSH:") == start_ssh
    assert not any("State: PulseRelay" in ln for ln in qa.lines)
