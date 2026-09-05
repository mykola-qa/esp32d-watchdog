#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Ping.h>
#include <esp_task_wdt.h>
#include <cstring>
#include <cstdlib>

#include "secrets.h"
#include "config.h"
#include "log_remote.h"

#ifndef HTTPC_ERROR_READ_TIMEOUT
#define HTTPC_ERROR_READ_TIMEOUT (-11)
#endif

static const uint32_t WDT_TIMEOUT_S = 30;

enum class State : uint8_t { ConnectWifi, Monitor, PulseRelay, Cooldown };

static State state = State::ConnectWifi;
static uint8_t pingFailCount = 0;
static uint8_t wedgeFailCount = 0;
static uint8_t haUpdateFailCount = 0;
static int8_t haUpdatePolicy = -1;
static uint8_t rebootsInWindow = 0;
static uint32_t lastCheckMs = 0;
static uint32_t stateStartMs = 0;
static uint32_t rebootWindowStartMs = 0;
static uint32_t lastWifiAttemptMs = 0;

static void resetHaUpdateGrace() {
  haUpdateFailCount = 0;
  haUpdatePolicy = -1;
}

#ifdef QA_FAST
static bool injectPingFail = false;
static bool injectHaFail = false;
static bool injectHaTimeout = false;
static bool injectHealthFail = false;
static bool injectSshFail = false;
static int8_t injectSshLoginCount = -2;
static bool injectWifiDown = false;
#endif

static void relayIdle() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
}

static void relayCut() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

static void setLed(bool on) { digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW); }

static void heartbeatLed() {
  digitalWrite(STATUS_LED_PIN, (millis() / 1000) % 2 ? HIGH : LOW);
}

static void blinkLed(uint32_t onMs, uint32_t offMs) {
  setLed(true);
  delay(onMs);
  setLed(false);
  delay(offMs);
}

static void testRelayClick() {
  if (state != State::Monitor) {
    Log.println("TEST: ignored — relay test is only available in Monitor");
    return;
  }
  Log.printf("TEST: relay click %u ms\n", RELAY_TEST_PULSE_MS);
  relayCut();
  setLed(true);
  delay(RELAY_TEST_PULSE_MS);
  relayIdle();
  setLed(false);
  esp_task_wdt_reset();
  Log.println("TEST: relay idle (click + green LED should have happened)");
}

static void onCommand(char c) {
  if (c == '\r' || c == '\n') {
    return;
  }
  if (c == 't' || c == 'T') {
    testRelayClick();
    return;
  }
#ifdef QA_FAST
  if (c == 'f') {
    injectPingFail = true;
    Log.println("QA: ping inject FAIL");
  } else if (c == 'o') {
    injectPingFail = false;
    Log.println("QA: ping inject off");
  } else if (c == 'h') {
    injectHaFail = true;
    injectHaTimeout = false;
    Log.println("QA: HA inject FAIL");
  } else if (c == 'H') {
    injectHaFail = false;
    injectHaTimeout = false;
    Log.println("QA: HA inject off");
  } else if (c == 'u') {
    injectHaTimeout = true;
    injectHaFail = false;
    Log.println("QA: HA inject TIMEOUT");
  } else if (c == 'U') {
    injectHaTimeout = false;
    Log.println("QA: HA timeout inject off");
  } else if (c == 's') {
    injectHealthFail = true;
    Log.println("QA: health inject FAIL");
  } else if (c == 'S') {
    injectHealthFail = false;
    Log.println("QA: health inject off");
  } else if (c == 'k') {
    injectSshFail = true;
    Log.println("QA: SSH inject FAIL");
  } else if (c == 'K') {
    injectSshFail = false;
    Log.println("QA: SSH inject off");
  } else if (c == 'n') {
    injectSshLoginCount = 0;
    Log.println("QA: SSH logins inject 0");
  } else if (c == 'N') {
    injectSshLoginCount = 1;
    Log.println("QA: SSH logins inject 1");
  } else if (c == 'x') {
    injectSshLoginCount = -1;
    Log.println("QA: SSH logins inject unknown");
  } else if (c == 'X') {
    injectSshLoginCount = -2;
    Log.println("QA: SSH logins inject off");
  } else if (c == 'w') {
    injectWifiDown = true;
    Log.println("QA: wifi inject DOWN");
  } else if (c == 'W') {
    injectWifiDown = false;
    Log.println("QA: wifi inject off");
  } else if (c == 'B') {
    rebootsInWindow = 0;
    rebootWindowStartMs = millis();
    pingFailCount = 0;
    wedgeFailCount = 0;
    resetHaUpdateGrace();
    Log.println("QA: budget reset");
  }
#endif
}

static void pollSerialTest() {
  while (Serial.available()) {
    onCommand(static_cast<char>(Serial.read()));
  }
}

static bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    logRemoteBegin();
    return true;
  }

  Log.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    pollSerialTest();
    blinkLed(80, 220);
    esp_task_wdt_reset();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Log.println("WiFi: failed");
    WiFi.disconnect(false);
    return false;
  }

  Log.printf("WiFi: %s  RSSI %d\n", WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
  logRemoteBegin();
  return true;
}

static bool piPingOk() {
#ifdef QA_FAST
  if (injectPingFail) {
    Log.printf("Ping fail %s\n", PING_HOST);
    return false;
  }
#endif
  esp_task_wdt_reset();
  const bool ok = Ping.ping(PING_HOST, PING_COUNT);
  esp_task_wdt_reset();
  if (ok) {
    Log.printf("Ping OK  %s  %.0f ms\n", PING_HOST, Ping.averageTime());
  } else {
    Log.printf("Ping fail %s\n", PING_HOST);
  }
  return ok;
}

enum class HaProbe : uint8_t { Up, Timeout, Down };

static int httpGet(const char *tag, const char *host, uint16_t port,
                   const char *path) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  char url[80];
  snprintf(url, sizeof(url), "http://%s:%u%s", host, port, path);

  if (!http.begin(client, url)) {
    Log.printf("%s: begin failed\n", tag);
    return -1;
  }

  esp_task_wdt_reset();
  const int code = http.GET();
  esp_task_wdt_reset();
  http.end();

  if (code >= 200 && code < 400) {
    Log.printf("%s: HTTP %d\n", tag, code);
  } else if (code > 0) {
    Log.printf("%s: unhealthy HTTP %d\n", tag, code);
  } else {
    Log.printf("%s: no reply (%d)\n", tag, code);
  }
  return code;
}

static HaProbe homeAssistantProbe() {
#ifdef QA_FAST
  if (injectHaTimeout) {
    Log.println("HA: no reply (-11)");
    return HaProbe::Timeout;
  }
  if (injectHaFail) {
    Log.println("HA: no reply (-1)");
    return HaProbe::Down;
  }
#endif
  const int code = httpGet("HA", HA_HOST, HA_PORT, HA_PATH);
  if (code >= 200 && code < 400) {
    return HaProbe::Up;
  }
  if (code == HTTPC_ERROR_READ_TIMEOUT) {
    return HaProbe::Timeout;
  }
  return HaProbe::Down;
}

static int8_t sshLoginCount = -1;

static bool hostHealthOk() {
  sshLoginCount = -1;
#ifdef QA_FAST
  if (injectHealthFail) {
    Log.println("Health: no reply (-1)");
    return false;
  }
#endif
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  char url[80];
  snprintf(url, sizeof(url), "http://%s:%u%s", HEALTH_HOST, HEALTH_PORT,
           HEALTH_PATH);

  if (!http.begin(client, url)) {
    Log.println("Health: begin failed");
    return false;
  }

  esp_task_wdt_reset();
  const int code = http.GET();
  String body;
  if (code > 0) {
    body = http.getString();
  }
  esp_task_wdt_reset();
  http.end();

  if (code < 200 || code >= 400) {
    if (code > 0) {
      Log.printf("Health: unhealthy HTTP %d\n", code);
    } else {
      Log.printf("Health: no reply (%d)\n", code);
    }
    return false;
  }

  const char *found = strstr(body.c_str(), "ssh=");
  if (found != nullptr) {
    const int n = atoi(found + 4);
    sshLoginCount = (int8_t)((n < -1) ? -1 : (n > 127 ? 127 : n));
  }
#ifdef QA_FAST
  if (injectSshLoginCount != -2) {
    sshLoginCount = injectSshLoginCount;
  }
#endif
  if (sshLoginCount < 0) {
    Log.printf("Health: HTTP %d ssh=?\n", code);
  } else {
    Log.printf("Health: HTTP %d ssh=%d\n", code, (int)sshLoginCount);
  }
  return true;
}

static bool sshKexinitOk() {
#ifdef QA_FAST
  if (injectSshFail) {
    Log.println("SSH: no KEXINIT");
    return false;
  }
#endif
  WiFiClient client;
  client.setTimeout(SSH_TIMEOUT_MS);
  if (!client.connect(SSH_HOST, SSH_PORT)) {
    Log.println("SSH: connect fail");
    return false;
  }

  char line[96];
  size_t n = 0;
  line[0] = 0;
  bool gotBanner = false;
  uint32_t start = millis();
  while (millis() - start < (uint32_t)SSH_TIMEOUT_MS && !gotBanner) {
    esp_task_wdt_reset();
    while (client.available()) {
      const int c = client.read();
      if (c < 0) {
        break;
      }
      if (n + 1 < sizeof(line)) {
        line[n++] = (char)c;
        line[n] = 0;
      }
      if (c == '\n') {
        if (strstr(line, "SSH-") != nullptr) {
          gotBanner = true;
          break;
        }
        n = 0;
        line[0] = 0;
      }
    }
    if (!gotBanner) {
      delay(20);
    }
  }
  if (!gotBanner) {
    client.stop();
    Log.println("SSH: no banner");
    return false;
  }

  client.print("SSH-2.0-esp32d-watchdog\r\n");
  client.flush();

  uint8_t hdr[6];
  size_t h = 0;
  start = millis();
  while (millis() - start < (uint32_t)SSH_TIMEOUT_MS && h < sizeof(hdr)) {
    esp_task_wdt_reset();
    while (client.available() && h < sizeof(hdr)) {
      const int c = client.read();
      if (c < 0) {
        break;
      }
      if (h == 0 && (c == '\r' || c == '\n')) {
        continue;
      }
      hdr[h++] = (uint8_t)c;
    }
    if (h < sizeof(hdr)) {
      delay(20);
    }
  }
  client.stop();

  if (h < sizeof(hdr)) {
    Log.println("SSH: no KEXINIT");
    return false;
  }

  const uint32_t packetLen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                             ((uint32_t)hdr[2] << 8) | hdr[3];
  const uint8_t padding = hdr[4];
  const uint8_t msg = hdr[5];
  if (msg == 20 && packetLen >= 16 && packetLen < 35000 && padding < packetLen) {
    Log.println("SSH: KEXINIT OK");
    return true;
  }

  Log.println("SSH: no KEXINIT");
  return false;
}

static bool rebootBudgetOk() {
  const uint32_t now = millis();
  if (now - rebootWindowStartMs >= REBOOT_WINDOW_MS) {
    rebootWindowStartMs = now;
    rebootsInWindow = 0;
  }
  if (rebootsInWindow >= MAX_REBOOTS_PER_HOUR) {
    Log.printf("Reboot budget exhausted (%u / hour)\n", MAX_REBOOTS_PER_HOUR);
    return false;
  }
  return true;
}

static void enter(State next);

static void requestReboot(const char *reason) {
  if (!rebootBudgetOk()) {
    pingFailCount = 0;
    wedgeFailCount = 0;
    resetHaUpdateGrace();
    return;
  }
  Log.println(reason);
  rebootsInWindow++;
  pingFailCount = 0;
  wedgeFailCount = 0;
  resetHaUpdateGrace();
  enter(State::PulseRelay);
}

static void enter(State next) {
  state = next;
  stateStartMs = millis();
  switch (next) {
    case State::ConnectWifi:
      Log.println("State: ConnectWifi");
      break;
    case State::Monitor:
      pingFailCount = 0;
      wedgeFailCount = 0;
      resetHaUpdateGrace();
      lastCheckMs = 0;
      Log.println("State: Monitor");
      break;
    case State::PulseRelay:
      Log.println("State: PulseRelay — cutting Pi 4B power");
      relayCut();
      setLed(true);
      break;
    case State::Cooldown:
      Log.printf("State: Cooldown %u s for Pi boot\n",
                    POST_REBOOT_COOLDOWN_MS / 1000);
      relayIdle();
      setLed(false);
      break;
  }
}

static void benchToggleRelay() {
#if RELAY_BENCH_TOGGLE
  Log.println("BENCH: toggling relay 8 times — listen for clicks");
  for (int i = 0; i < 8; i++) {
    relayCut();
    Log.printf("BENCH: CUT  %d/8  (green should be bright, click)\n", i + 1);
    delay(800);
    esp_task_wdt_reset();
    relayIdle();
    Log.printf("BENCH: IDLE %d/8  (green should go off, click)\n", i + 1);
    delay(800);
    esp_task_wdt_reset();
  }
  Log.println("BENCH: done — set RELAY_BENCH_TOGGLE 0 after this works");
#endif
}

void setup() {
  relayIdle();
  pinMode(STATUS_LED_PIN, OUTPUT);
  setLed(false);

  Serial.begin(115200);
  delay(200);
  Log.println();
  Log.println("esp32d-watchdog  ping + HA + host health + SSH");
#ifdef QA_FAST
  Log.println("BUILD QA_FAST");
#endif
  Log.printf("Ping %s every %u s, reboot after %u misses (~%u min)\n",
                PING_HOST, CHECK_INTERVAL_MS / 1000, PING_FAIL_THRESHOLD,
                (PING_FAIL_THRESHOLD * CHECK_INTERVAL_MS) / 60000);
  Log.printf("HA timeout (-11) → reboot after %u misses; HA down + health + SSH → reboot after %u (~30 min)\n",
                WEDGE_FAIL_THRESHOLD, HA_UPDATE_FAIL_THRESHOLD);
  Log.println("Send t over USB to click relay 1s (test stand only)");

  esp_err_t wdt = esp_task_wdt_init(WDT_TIMEOUT_S, true);
  if (wdt != ESP_OK && wdt != ESP_ERR_INVALID_STATE) {
    Log.printf("WDT init err %d\n", (int)wdt);
  }
  esp_task_wdt_add(NULL);

  benchToggleRelay();
  relayIdle();

  rebootWindowStartMs = millis();
  enter(State::ConnectWifi);
}

void loop() {
  esp_task_wdt_reset();
  pollSerialTest();
  logRemoteLoop();
  const uint32_t now = millis();

  switch (state) {
    case State::ConnectWifi:
      if (connectWifi()) {
        enter(State::Monitor);
      } else {
        delay(WIFI_RETRY_MS);
      }
      break;

    case State::Monitor: {
      const bool wifiUp =
#ifdef QA_FAST
          !injectWifiDown &&
#endif
          WiFi.status() == WL_CONNECTED;
      if (!wifiUp) {
        Log.println("WiFi lost — will not reboot Pi");
        pingFailCount = 0;
        wedgeFailCount = 0;
        resetHaUpdateGrace();
        if (now - lastWifiAttemptMs >= WIFI_RETRY_MS) {
          lastWifiAttemptMs = now;
          enter(State::ConnectWifi);
        }
        break;
      }

      if (lastCheckMs != 0 && now - lastCheckMs < CHECK_INTERVAL_MS) {
        delay(50);
        break;
      }
      lastCheckMs = now;

      if (piPingOk()) {
        pingFailCount = 0;
        const HaProbe ha = homeAssistantProbe();
        if (ha == HaProbe::Up) {
          wedgeFailCount = 0;
          resetHaUpdateGrace();
        } else if (ha == HaProbe::Timeout) {
          resetHaUpdateGrace();
          wedgeFailCount++;
          Log.printf("HA timeout miss %u / %u\n", wedgeFailCount,
                        WEDGE_FAIL_THRESHOLD);
          if (wedgeFailCount >= WEDGE_FAIL_THRESHOLD) {
            requestReboot("HA HTTP timeout — rebooting Pi");
          }
        } else {
          const bool healthOk = hostHealthOk();
          const bool sshOk = sshKexinitOk();
          if (healthOk && sshOk) {
            wedgeFailCount = 0;
            const int8_t policy = (sshLoginCount == 0) ? 0 : 1;
            if (policy != haUpdatePolicy) {
              haUpdateFailCount = 0;
              haUpdatePolicy = policy;
            }
            const uint8_t limit =
                (policy == 0) ? WEDGE_FAIL_THRESHOLD : HA_UPDATE_FAIL_THRESHOLD;
            haUpdateFailCount++;
            if (sshLoginCount < 0) {
              Log.printf(
                  "HA down, host health and SSH OK — update miss %u / %u (ssh ?)\n",
                  haUpdateFailCount, limit);
            } else {
              Log.printf(
                  "HA down, host health and SSH OK — update miss %u / %u (ssh %d)\n",
                  haUpdateFailCount, limit, (int)sshLoginCount);
            }
            if (haUpdateFailCount >= limit) {
              if (sshLoginCount == 0) {
                requestReboot("HA down ~5 min (no SSH login) — rebooting Pi");
              } else {
                requestReboot("HA down ~30 min — rebooting Pi");
              }
            }
          } else {
            resetHaUpdateGrace();
            wedgeFailCount++;
            Log.printf("Wedge miss %u / %u (health %s SSH %s)\n",
                          wedgeFailCount, WEDGE_FAIL_THRESHOLD,
                          healthOk ? "OK" : "fail", sshOk ? "OK" : "fail");
            if (wedgeFailCount >= WEDGE_FAIL_THRESHOLD) {
              requestReboot("HA down and host wedged — rebooting Pi");
            }
          }
        }
      } else {
        wedgeFailCount = 0;
        resetHaUpdateGrace();
        pingFailCount++;
        Log.printf("Ping miss %u / %u\n", pingFailCount, PING_FAIL_THRESHOLD);
        blinkLed(50, 50);
        blinkLed(50, 50);
        if (pingFailCount >= PING_FAIL_THRESHOLD) {
          requestReboot("Ping down ~5 min — rebooting Pi");
        }
      }
      break;
    }

    case State::PulseRelay:
      if (now - stateStartMs >= RELAY_PULSE_MS) {
        enter(State::Cooldown);
      }
      break;

    case State::Cooldown:
      if (now - stateStartMs >= POST_REBOOT_COOLDOWN_MS) {
        enter(State::Monitor);
      }
      delay(50);
      break;
  }

  if (state == State::PulseRelay) {
    setLed(true);
  } else {
    heartbeatLed();
  }
}
