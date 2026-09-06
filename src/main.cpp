#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <esp_system.h>
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
static uint32_t wifiJoinStartMs = 0;
static uint32_t wifiRetryAtMs = 0;
static uint32_t lastTestClickMs = 0;

static void resetHaUpdateGrace() {
  haUpdateFailCount = 0;
  haUpdatePolicy = -1;
}

static void onCommand(char c);

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

static void drainSerialTestKeys() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == 't' || c == 'T' || c == '\r' || c == '\n') {
      continue;
    }
#ifdef QA_FAST
    onCommand(c);
#else
    (void)c;
#endif
  }
}

static void testRelayClick() {
  if (state != State::Monitor) {
    Log.println("TEST: ignored — relay test is only available in Monitor");
    drainSerialTestKeys();
    return;
  }
  if (lastTestClickMs != 0 &&
      millis() - lastTestClickMs < RELAY_TEST_GUARD_MS) {
    Log.println("TEST: ignored — cooldown (will not click while STA is up)");
    drainSerialTestKeys();
    return;
  }
  lastTestClickMs = millis();
  Log.printf("TEST: relay click %u ms\n", RELAY_TEST_PULSE_MS);
  relayCut();
  setLed(true);
  delay(RELAY_TEST_PULSE_MS);
  relayIdle();
  setLed(false);
  esp_task_wdt_reset();
  drainSerialTestKeys();
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

static const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    default:
      return "OTHER";
  }
}

static void wifiRadioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static void wifiStartJoin() {
  wifiRadioOff();
  delay(WIFI_RADIO_OFF_MS);
  esp_task_wdt_reset();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname(LOG_MDNS_HOST);
  Log.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiJoinStartMs = millis();
}

static bool wifiPollJoin() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiJoinStartMs != 0) {
      Log.printf("WiFi: %s  RSSI %d\n", WiFi.localIP().toString().c_str(),
                 WiFi.RSSI());
      wifiJoinStartMs = 0;
    }
    wifiRetryAtMs = 0;
    logRemoteBegin();
    return true;
  }

  const uint32_t now = millis();
  if (wifiRetryAtMs != 0 && now < wifiRetryAtMs) {
    return false;
  }
  wifiRetryAtMs = 0;

  if (wifiJoinStartMs == 0) {
    wifiStartJoin();
    return false;
  }

  if (now - wifiJoinStartMs < WIFI_CONNECT_TIMEOUT_MS) {
    blinkLed(80, 220);
    return false;
  }

  Log.println("WiFi: failed");
  wifiRadioOff();
  wifiJoinStartMs = 0;
  wifiRetryAtMs = now + WIFI_RETRY_MS;
  return false;
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

static bool waitClientData(WiFiClient &client, uint32_t deadlineMs) {
  while (millis() < deadlineMs) {
    esp_task_wdt_reset();
    if (client.available()) {
      return true;
    }
    if (!client.connected()) {
      return false;
    }
    delay(10);
  }
  return false;
}

static bool readLine(WiFiClient &client, char *buf, size_t bufSize,
                     uint32_t deadlineMs) {
  size_t n = 0;
  buf[0] = 0;
  while (millis() < deadlineMs) {
    if (!waitClientData(client, deadlineMs)) {
      return false;
    }
    const int c = client.read();
    if (c < 0) {
      return false;
    }
    if (c == '\n') {
      buf[n] = 0;
      if (n > 0 && buf[n - 1] == '\r') {
        buf[n - 1] = 0;
      }
      return true;
    }
    if (n + 1 < bufSize) {
      buf[n++] = static_cast<char>(c);
      buf[n] = 0;
    }
  }
  return false;
}

static int parseHttpStatus(const char *line) {
  if (strncmp(line, "HTTP/", 5) != 0) {
    return -11;
  }
  const char *space = strchr(line, ' ');
  if (space == nullptr) {
    return -11;
  }
  return atoi(space + 1);
}

static const char *haProbePath() {
  if (strcmp(HA_PATH, "/") == 0 || strcmp(HA_PATH, "/api/") == 0 ||
      strcmp(HA_PATH, "/api") == 0) {
    return "/manifest.json";
  }
  return HA_PATH;
}

static bool haCodeMeansUp(int code) {
  return (code >= 200 && code < 400) || code == 401;
}

static void logHttpCode(const char *tag, int code, bool treat401Up) {
  if ((code >= 200 && code < 400) || (treat401Up && code == 401)) {
    Log.printf("%s: HTTP %d\n", tag, code);
  } else if (code > 0) {
    Log.printf("%s: unhealthy HTTP %d\n", tag, code);
  } else {
    Log.printf("%s: no reply (%d)\n", tag, code);
  }
}

static int httpGet(const char *tag, const char *host, uint16_t port,
                   const char *path, char *body, size_t bodySize,
                   bool treat401Up) {
  if (body != nullptr && bodySize > 0) {
    body[0] = 0;
  }

  WiFiClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);
  if (!client.connect(host, port, HTTP_TIMEOUT_MS)) {
    logHttpCode(tag, -1, treat401Up);
    return -1;
  }

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);
  client.print("\r\nConnection: close\r\n\r\n");
  client.flush();

  const uint32_t deadline = millis() + HTTP_TIMEOUT_MS;
  char line[96];
  if (!readLine(client, line, sizeof(line), deadline)) {
    client.stop();
    logHttpCode(tag, HTTPC_ERROR_READ_TIMEOUT, treat401Up);
    return HTTPC_ERROR_READ_TIMEOUT;
  }

  const int code = parseHttpStatus(line);
  if (code <= 0) {
    client.stop();
    logHttpCode(tag, HTTPC_ERROR_READ_TIMEOUT, treat401Up);
    return HTTPC_ERROR_READ_TIMEOUT;
  }

  bool headersDone = false;
  while (readLine(client, line, sizeof(line), deadline)) {
    if (line[0] == 0) {
      headersDone = true;
      break;
    }
  }

  if (body != nullptr && bodySize > 0 && headersDone) {
    size_t n = 0;
    while (n + 1 < bodySize && millis() < deadline) {
      if (!client.available()) {
        if (!client.connected()) {
          break;
        }
        esp_task_wdt_reset();
        delay(10);
        continue;
      }
      const int c = client.read();
      if (c < 0) {
        break;
      }
      body[n++] = static_cast<char>(c);
    }
    body[n] = 0;
  }

  client.stop();
  if (body == nullptr || code < 200 || code >= 400) {
    logHttpCode(tag, code, treat401Up);
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
  const int code =
      httpGet("HA", HA_HOST, HA_PORT, haProbePath(), nullptr, 0, true);
  if (haCodeMeansUp(code)) {
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
  char body[96];
  const int code = httpGet("Health", HEALTH_HOST, HEALTH_PORT, HEALTH_PATH,
                           body, sizeof(body), false);

  if (code < 200 || code >= 400) {
    return false;
  }

  const char *found = strstr(body, "ssh=");
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
  if (!client.connect(SSH_HOST, SSH_PORT, SSH_TIMEOUT_MS)) {
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
  Log.printf("Reset %s  heap %u\n", resetReasonText(), ESP.getFreeHeap());
  Log.println("Send t over USB to click relay 1s (test stand; 5 s cooldown)");

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

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
      if (wifiPollJoin()) {
        enter(State::Monitor);
      } else {
        delay(50);
      }
      break;

    case State::Monitor: {
      const bool wifiUp =
#ifdef QA_FAST
          !injectWifiDown &&
#endif
          WiFi.status() == WL_CONNECTED;
      if (!wifiUp) {
        pingFailCount = 0;
        wedgeFailCount = 0;
        resetHaUpdateGrace();
        if (WiFi.status() != WL_CONNECTED) {
          logRemoteStop();
        }
        if (now - lastWifiAttemptMs >= WIFI_RETRY_MS) {
          lastWifiAttemptMs = now;
          Log.println("WiFi lost — will not reboot Pi");
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
