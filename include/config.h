#pragma once

// Tongling JQC-3FF-S-Z 3.3V: VCC=ESP32 3V3, IN=D26.
// This board is LOW trigger: idle = GPIO HIGH (coil off, NC closed, Pi on).
// 30A opto module with jumper L: also RELAY_ACTIVE_HIGH 0.
#define RELAY_PIN 26
#define RELAY_ACTIVE_HIGH 0

// 1 = click 8 times at boot. Must be 0 before splicing Pi 4B 5V.
#define RELAY_BENCH_TOGGLE 0
#define STATUS_LED_PIN 2

#define HTTP_TIMEOUT_MS 5000
#define SSH_TIMEOUT_MS 4000
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_MS 5000
#define WIFI_RADIO_OFF_MS 150
#define RELAY_TEST_PULSE_MS 1000
#define RELAY_TEST_GUARD_MS 5000
#define MAX_REBOOTS_PER_HOUR 3

// Read-only LAN logs. Do not expose these services outside a trusted LAN.
#define LOG_MDNS_HOST "esp32-watchdog"
#define LOG_TELNET_PORT 23
#define LOG_HTTP_PORT 80

#ifdef QA_FAST
#define CHECK_INTERVAL_MS 1000
#define PING_COUNT 1
#define PING_FAIL_THRESHOLD 5
#define WEDGE_FAIL_THRESHOLD 5
#define HA_UPDATE_FAIL_THRESHOLD 8
#define RELAY_PULSE_MS 10000
#define POST_REBOOT_COOLDOWN_MS 5000
#define REBOOT_WINDOW_MS 180000UL
#else
#define CHECK_INTERVAL_MS 15000
#define PING_COUNT 2
#define PING_FAIL_THRESHOLD 20
#define WEDGE_FAIL_THRESHOLD 20
#define HA_UPDATE_FAIL_THRESHOLD 120
#define RELAY_PULSE_MS 10000
#define POST_REBOOT_COOLDOWN_MS 180000
#define REBOOT_WINDOW_MS 3600000UL
#endif
