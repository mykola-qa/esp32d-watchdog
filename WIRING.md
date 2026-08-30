# Wiring — ESP32-D + 3.3 V relay watchdog

Primary module: **Tongling JQC-3FF-S-Z 3.3VDC** (pins `VCC` `GND` `IN`).
Firmware pin **D26**. Contacts 10 A — enough for Pi 4B ~3 A.

Do **not** put 5 V / VIN on this coil. Do not use the USB-plug relay.
The old 5 V 3-pin JQC3F and the 30A opto board are optional fallbacks (see below).

Firmware: `RELAY_PIN 26`, `RELAY_ACTIVE_HIGH 0` (low trigger), `RELAY_BENCH_TOGGLE 0`.
Idle: coil **off**, COM–NC **closed**, Pi **on**. If idle still cuts the Pi, you are on NO, not NC.

---

## 3.3 V relay — power from the ESP32

Coil and `IN` are both 3.3 V. The module has a transistor, so GPIO 26 only drives the input LED/base; **VCC** feeds the coil from ESP32 **3V3**.

USB 5 V → AMS1117 → **3V3**. Wi-Fi + a ~10 s coil pulse is usually OK on a 5 V 1 A USB supply. If the ESP32 **resets when it clicks**, 3V3 is sagging — go back to the 30A module with a separate 5 V coil feed.

```text
ESP32 3V3  ──►  relay VCC     coil + logic
ESP32 GND  ──►  relay GND
ESP32 D26  ──►  relay IN
optional: ESP32 3V3 ── 10 kΩ ── D26

relay NO   COM   NC     empty on the test stand
```

| Relay | ESP32 | Why |
|-------|--------|-----|
| **VCC** | **3V3** | 3.3 V coil — never VIN / 5 V |
| **GND** | **GND** | Common ground |
| **IN** | **D26** | Watchdog pulse |
| **NO COM NC** | empty until clicks work | Load later |

The optional 10 kΩ pull-up gives the low-trigger input a defined idle level
before firmware configures D26. This tested module did not click during repeated
EN resets, so the resistor is additional protection rather than a requirement.

---

## Test stand (Pi 5)

Pi 5 = USB computer for flash/logs. Relay power comes from the ESP32 **3V3**, not from Pi 5 pin 2.
**COM / NC / NO stay empty.** Do not splice the Pi 4B power cable here.

```text
Raspberry Pi 5 USB-A ── data USB-C ── ESP-32D USB-C

ESP-32D                         Tongling 3.3 V relay
┌─────────────┐                 ┌─────────────────┐
│ 3V3  ●──────┼────────────────►│ VCC             │
│ GND  ●──────┼────────────────►│ GND             │
│ D26  ●──────┼────────────────►│ IN              │
│ D2   (status LED)             │ NO  COM  NC     │  EMPTY
└─────────────┘                 └─────────────────┘
```

Idle: cube released. Latin serial `t`: 1 s click.
Green LED alone is **not** a click — feel/hear the blue cube.

Flash prod image:

```bash
pio run -e esp32dev -t upload -t monitor --upload-port "$ESP32_PORT"
```

`esp32dev_qa` is only for pytest. Never leave QA firmware on a spliced Pi 4B.

---

## Production (box)

Pi 5 is **out**. ESP32 USB stays on its **own** 5 V 1 A wall adapter so the watchdog lives when the Pi 4B dies. Relay **VCC** still from ESP32 **3V3**.

```text
Wall 5V 1A adapter
    └─ USB-C ────────────── ESP32 USB-C
                            ESP32 3V3 ── relay VCC
                            ESP32 GND ── relay GND
                            ESP32 D26 ── relay IN

Pi 4B official PSU (5.1 V / 3 A) — its own plug
    cut RED 5V only:
        PSU 5V ── relay COM
        relay NC ── Pi 4B USB-C 5V
    GND (black) ── one continuous wire, not through the relay
```

Keep the added 5 V conductors short and use copper wire sized for the Pi load.
Measure the voltage at COM and at the Pi-side pigtail under load; a large drop
means a weak cable, terminal, or contact. Check `vcgencmd get_throttled` for
recorded undervoltage.

Hard power cuts can corrupt a writable SD card or filesystem. Maintain backups
and use this relay only after software recovery options have failed.

| Role | Test stand | Prod |
|------|------------|------|
| ESP32 5 V | Pi 5 USB | Wall 5 V 1 A USB |
| Relay VCC | ESP32 **3V3** | ESP32 **3V3** |
| IN | D26 | D26 |
| COM / NC | empty | Pi 4B USB-C **5 V** only |
| Pi 5 | required | gone |
| `RELAY_BENCH_TOGGLE` | 0 | **0** |

**COM + NC** so the Pi stays **on** if the ESP32 is unplugged or reset.
If the Pi dies when ESP32 is off, you used NO instead of NC, or trigger is inverted.

---

## Fallback: 30A opto module

Use this if 3V3 browns out on click.

Jumpers: **JD+ ↔ DC+ OFF**, **JD− ↔ DC− ON**, trigger **L**.
Firmware: `RELAY_ACTIVE_HIGH 0`.

Coil **JD+** = real 5 V (Pi 5 pin 2 on the stand, wall 5 V in the box).
Logic **DC+** = ESP32 3V3. **IN** = D26. **DC−** = GND.

---

## What the firmware does

Ping the configured `PING_HOST` every 15 s, pulse after ~5 min down.
HA on the configured port down **and** SSH with no `SSH-` banner for ~5 min → pulse.
HA down but SSH banner still works (typical update) → no pulse.

## WiFi logs (no USB)

Read the assigned IP from the serial `WiFi:` line:

- Browser: `http://<esp-ip>/` or `http://esp32-watchdog.local/`
- Live stream: `nc <esp-ip> 23`

LAN diagnostics are read-only and have no password. Keep them on a trusted LAN;
never expose ports 80 or 23 to the internet. USB serial remains available for
manual `t` testing and flashing.

---

## Status LED (optional)

Firmware already drives **D2** (`STATUS_LED_PIN`). Onboard LED and this red LED blink together.

```text
ESP32 D2 ── 220 Ω ── (+) red LED (−) ── GND
```

Long LED leg (anode) toward the resistor. Short leg / flat edge (cathode) to **GND**.
Do not skip the resistor. Do not use 3V3 or VIN as the LED supply; GPIO D2 sources 3.3 V.

---

## Move test → prod (order)

1. Test stand: cube clicks on `t`, serial shows `Ping OK`.
2. Confirm `RELAY_BENCH_TOGGLE` is 0, flash `esp32dev` (not `_qa`).
3. Power ESP32 from the wall USB adapter; unplug Pi 5 USB. Relay VCC stays on ESP32 3V3.
4. Splice only Pi 4B **5 V** through COM–NC. Meter: COM–NC closed with ESP32 unplugged.
5. ESP32 on, Pi 4B on, HA up: no click.
6. Optional: unplug Pi ethernet ~5 min → one 10 s pulse (live test, once).

---

## Parts you are not using

- Old **5 V** 3-pin JQC3F — LED can blink, coil often will not click from 3.3 V IN.
- USB-A relay dongle — ESP32 is not a USB host.
- USB data extender — does not reboot the Pi; Pi 4B power is USB-C 5 V.
