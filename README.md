# esp32d-watchdog

An independently powered ESP32 watchdog for a Raspberry Pi running Home Assistant.
It distinguishes an unavailable service from an unresponsive host before
interrupting the Pi's 5 V supply through a normally closed relay.

## Architecture

```text
Independent 5 V supply ── USB-C ── ESP32 ── D26 ── 3.3 V relay input
                                      │
                                      ├── ICMP ping
                                      ├── Home Assistant HTTP
                                      ├── SSH banner
                                      └── read-only HTTP/telnet logs

Pi PSU 5 V ── COM  relay  NC ── Pi 5 V
Pi PSU GND ──────────────────── Pi GND
```

## Safety model

- ESP32 Wi-Fi unavailable: never reboot the Pi.
- Pi ping unavailable for about five minutes: cut power for 10 seconds.
- Home Assistant unavailable but SSH banner healthy: leave the Pi running.
- Home Assistant and SSH banner unavailable for about five minutes: cut power.
- After a pulse: allow three minutes for boot and limit recovery to three attempts
  per ESP uptime hour.
- ESP32 unpowered: relay coil releases and COM-NC keeps the Pi powered.

Hard power cuts can corrupt writable storage. Keep backups and use this watchdog
only as a last-resort recovery mechanism. See [WIRING.md](WIRING.md) before
connecting the Pi power path.

## Quick start

Requirements: PlatformIO Core 6.x or the PlatformIO IDE extension.

```bash
cp include/secrets.h.example include/secrets.h
```

Edit `include/secrets.h` with the Wi-Fi credentials and monitored host addresses,
then build:

```bash
pio run -e esp32dev
```

`include/secrets.h` is intentionally ignored by Git.

Build environments:

- `esp32dev`: production timings and no fault injectors
- `esp32dev_qa`: compressed timings and serial fault injection for HIL
- `esp32dev_boot`: production image uploaded without an automatic DTR reset

## Flash and USB serial

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

Type a Latin `t` for a one-second test pulse. It is accepted only while the
watchdog is in `Monitor`; do not use Ctrl+T, which opens PlatformIO's menu.
Quit with Ctrl+C.

## Read-only LAN diagnostics

After Wi-Fi connects, the serial log prints the assigned address:

- Browser: `http://<esp-ip>/` or `http://esp32-watchdog.local/`
- Live stream: `nc <esp-ip> 23`

LAN clients cannot trigger the relay. Manual testing is available only through
USB serial. These services have no authentication; keep them on a trusted LAN
and never expose them to the internet.

## HIL tests (Python)

The pytest suite drives fault-injection commands over serial and validates state
transitions and timing from firmware logs. It does not electrically measure GPIO
or relay contacts; verify those manually before connecting a load.

Never leave the QA image on a spliced Pi 5 V cable.

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
| h / H | HA fail on / off |
| s / S | SSH banner fail on / off |
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
   pigtails and closed contacts.

The hourly reboot budget is stored in RAM. Power-cycling the ESP32 resets it.

## License

MIT. See [LICENSE](LICENSE). ESP32Ping remains under its own upstream license.
