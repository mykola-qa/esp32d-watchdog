import importlib.util
from pathlib import Path
from unittest.mock import patch

MODULE_PATH = Path(__file__).parents[1] / "pi" / "watchdog-health.py"
SPEC = importlib.util.spec_from_file_location("watchdog_health", MODULE_PATH)
watchdog_health = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(watchdog_health)


def test_remote_login_count_ignores_local_console():
    output = """\
pi       tty1         2026-09-05 15:17 03:08        1144
pi       pts/0        2026-09-05 18:25   .         15095 (192.168.50.70)
pi       pts/1        2026-09-05 18:26   .         15120 (workstation.local)
"""

    assert watchdog_health.remote_login_count(output) == 2


def test_login_count_returns_unknown_when_who_fails():
    with patch.object(
        watchdog_health.subprocess, "check_output", side_effect=TimeoutError
    ):
        assert watchdog_health.login_count() == -1
