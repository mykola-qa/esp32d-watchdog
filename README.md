# esp32d-watchdog

An independently powered ESP32 watchdog for a Raspberry Pi running Home Assistant.
It distinguishes an unavailable service from an unresponsive host before
interrupting the Pi's 5 V supply through a normally closed relay.

The skip-reboot probes are host health HTTP (`:8081`, body `OK ssh=N` from
remote `who` sessions) **and** SSH KEXINIT. HA HTTP **read timeout** (`-11`) is a wedge (~5 min)
even when those probes succeed. HA connect fail (`-1`) with health and SSH up
pulses after **~5 min** if `ssh=0`, or **~30 min** if someone is logged in
(`ssh>=1`). A canary without `ssh=` is treated as unknown and uses 30 min.

## Architecture

```text
Independent 5 V supply ── USB-C ── ESP32 ── D26 ── 3.3 V relay input
                                      │
                                      ├── ICMP ping
                                      ├── Home Assistant HTTP
                                      ├── host health HTTP
                                      ├── SSH KEXINIT
                                      └── read-only HTTP/telnet logs

Pi PSU 5 V ── COM  relay  NC ── Pi 5 V
Pi PSU GND ──────────────────── Pi GND
```

## Safety model

- ESP32 Wi-Fi unavailable: never reboot the Pi.
- Pi ping unavailable for about five minutes: cut power for 10 seconds.
- Home Assistant HTTP **timeout** (`-11`): cut power after about five minutes (listener stuck).
- Home Assistant connect fail but host health **and** SSH KEXINIT succeed: ~5 min
  if no remote user is logged in (`ssh=0`), ~30 min with an SSH session.
  Changing between these policies restarts the selected grace timer.
- Home Assistant down and host health or SSH failed for about five minutes: cut power.
- After a pulse: allow three minutes for boot and limit recovery to three attempts
  per ESP uptime hour.
- ESP32 unpowered: relay coil releases and COM-NC keeps the Pi powered.

Hard power cuts can corrupt writable storage. Keep backups and use this watchdog
only as a last-resort recovery mechanism. See [WIRING.md](WIRING.md) before
connecting the Pi power path.

USB-C extenders and screw terminal blocks on the Pi 5 V or GND run drop voltage
under load. The Pi 4B reports undervoltage below about 4.63 V
(`vcgencmd get_throttled` sticky `0x50000`). Keep GND as one conductor, not
through the relay and not through a terminal block. Keep the 5 V splice short
and thick (about AWG 18 / 0.75 mm²). Meter 5.1 V at the Pi USB-C plug at idle
and again under HA/SSD load.

## Quick start

Requirements: PlatformIO Core 6.x or the PlatformIO IDE extension.

```bash
cp include/secrets.h.example include/secrets.h
```

Edit `include/secrets.h` with the Wi-Fi credentials, `HA_*`, `HEALTH_*`, and
`SSH_HOST` / `SSH_PORT`. Install the Pi liveness service **before** flashing
the ESP32. While Home Assistant is down, a missing health endpoint or a dead
SSH daemon looks like a wedged host.

## Pi liveness endpoint

A systemd service on the Pi serves HTTP on port 8081 and reports
`OK ssh=N` (`N` = remote sessions from `who`; local `tty1` is ignored). An HA connect-fail with a healthy canary
**and** SSH KEXINIT waits ~5 min if `N` is 0, ~30 min if anyone is logged in.
If HA accepts TCP but never returns HTTP (`-11`), the relay pulses after about
five minutes. If HA is down and health or SSH fails, also about five minutes.

Bare `python3 -m http.server` only returns `OK` (no `ssh=`), so the ESP32 keeps
the 30 min timer. Use the unit below.

Copy the unit from this repo, then enable it:

```bash
sudo cp pi/watchdog-health.py /usr/local/bin/watchdog-health.py
sudo cp pi/watchdog-health.service /etc/systemd/system/watchdog-health.service
sudo systemctl daemon-reload
sudo systemctl enable --now watchdog-health.service
curl -sS http://127.0.0.1:8081/
```

That should print `OK ssh=0` (or `ssh=1` if you are logged in). `ssh=-1` means
session detection failed and selects the safer 30-minute grace. Keep port 8081
on the trusted LAN only.

Then build:

```bash
pio run -e esp32dev
```

`include/secrets.h` is intentionally ignored by Git.

Build environments:

- `esp32dev`: production timings and no fault injectors
- `esp32dev_qa`: compressed timings and serial fault injection for HIL
- `esp32dev_boot`: production image uploaded without an automatic DTR reset

## Flash and USB serial

In production the ESP32 stays on its **own** 5 V adapter so the watchdog lives
when the Pi dies. Pi USB is for flash and serial only. Leaving the ESP32 (and
relay coil) on the Home Assistant Pi USB port can undervolt that Pi.

Connect the ESP32 with a USB **data** cable. Discover its port instead of assuming
`ttyUSB0`:

```bash
ls /dev/ttyUSB*
export ESP32_PORT=/dev/ttyUSB0
```

If several ports appear, unplug only the ESP32; the port that disappears is the
correct one.

```bash
pio run -e esp32dev -t upload -t monitor --upload-port "$ESP32_PORT"
```

For boards whose auto-reset circuit does not enter the bootloader:

```bash
pio run -e esp32dev_boot -t upload --upload-port "$ESP32_PORT"
```

Hold **BOOT**, tap and release **EN**, keep BOOT held until writing starts, then
release BOOT. Tap EN after upload. Monitor an already flashed board with:

```bash
pio device monitor --port "$ESP32_PORT" -b 115200 --dtr 0 --rts 0
```

Type a Latin `t` once for a one-second test pulse (`Monitor` only). Extra `t`
keystrokes within five seconds are ignored so key-repeat cannot pulse the coil.
Do not use Ctrl+T, which opens PlatformIO's menu.
Quit with Ctrl+C.

## Read-only LAN diagnostics

After Wi-Fi connects, the serial log prints the assigned address:

- Browser: `http://<esp-ip>/` or `http://esp32-watchdog.local/`
- Live stream: `nc <esp-ip> 23`

LAN clients cannot trigger the relay. Manual testing is available only through
USB serial. These services have no authentication; keep them on a trusted LAN
and never expose them to the internet.

If the router still shows the ESP32 DHCP lease but ping and port 80 fail from
the LAN, tap **EN** (or `esptool --port "$ESP32_PORT" run`) before reflashing.
A hung boot can keep the association while HTTP/telnet are dead.

## HIL tests (Python)

The pytest suite drives fault-injection commands over serial and validates state
transitions and timing from firmware logs. It does not electrically measure GPIO
or relay contacts; verify those manually before connecting a load.

Never leave the QA image on a spliced Pi 5 V cable. The skip-reboot HIL case
needs the Pi health canary on port 8081 **and** sshd answering KEXINIT.

```bash
pip install -r tests/requirements.txt

pio run -e esp32dev_qa -t upload --upload-port "$ESP32_PORT"
ESP32_PORT="$ESP32_PORT" pytest -m hardware
```

To include the read-only HTTP diagnostics test:

```bash
ESP32_PORT="$ESP32_PORT" ESP32_IP="<esp-ip>" pytest -m hardware
```

Prod image (no injectors; ping/HA timer tests skip):

```bash
pio run -e esp32dev -t upload --upload-port "$ESP32_PORT"
ESP32_PORT="$ESP32_PORT" pytest -m "hardware and not qa_fast"
```

### QA serial injectors (`QA_FAST` only)

| Key | Effect |
|-----|--------|
| f / o | ping fail on / off |
| h / H | HA connect-fail on / off |
| u / U | HA read-timeout (-11) on / off |
| s / S | host health fail on / off |
| k / K | SSH KEXINIT fail on / off |
| n / N | SSH login count 0 / 1 |
| x / X | SSH login count unknown / live canary |
| w / W | pretend WiFi down / off |
| B | reset reboot budget |
| t | 1 s relay click in Monitor (all builds, USB only) |

`relay_pulse` tests energize the coil for about 10 seconds. Keep COM/NC
disconnected from the Pi PSU.

## Manual electrical acceptance checks

1. ESP32 unplugged: COM-NC has continuity and the Pi power path stays closed.
2. Repeated EN resets: the relay does not click or flash.
3. USB `t` in Monitor: the relay opens COM-NC for about one second.
4. QA failure injection: the automatic pulse opens COM-NC for about 10 seconds.
5. Under normal Pi load: verify no undervoltage and minimal drop across the
   pigtails and closed contacts. Idle at the Pi USB-C plug should be about
   5.1 V. A USB-C extender or a terminal block on 5 V or GND is a typical
   cause of `rpi_power` Problem events.

The hourly reboot budget is stored in RAM. Power-cycling the ESP32 resets it.

## License

MIT. See [LICENSE](LICENSE). ESP32Ping remains under its own upstream license.
