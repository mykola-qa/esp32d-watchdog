#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Ping.h>
#include <esp_task_wdt.h>
#include <cstring>

#include "secrets.h"
#include "config.h"
#include "log_remote.h"

static const uint32_t WDT_TIMEOUT_S = 30;

enum class State : uint8_t { ConnectWifi, Monitor, PulseRelay, Cooldown };

static State state = State::ConnectWifi;
static uint8_t pingFailCount = 0;
static uint8_t wedgeFailCount = 0;
static uint8_t rebootsInWindow = 0;
static uint32_t lastCheckMs = 0;
static uint32_t stateStartMs = 0;
static uint32_t rebootWindowStartMs = 0;
static uint32_t lastWifiAttemptMs = 0;

#ifdef QA_FAST
static bool injectPingFail = false;
static bool injectHaFail = false;
static bool injectSshFail = false;
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
    Log.println("QA: HA inject FAIL");
  } else if (c == 'H') {
    injectHaFail = false;
    Log.println("QA: HA inject off");
  } else if (c == 's') {
    injectSshFail = true;
    Log.println("QA: SSH inject FAIL");
  } else if (c == 'S') {
    injectSshFail = false;
    Log.println("QA: SSH inject off");
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

static bool homeAssistantUp() {
#ifdef QA_FAST
  if (injectHaFail) {
    Log.println("HA: no reply (-1)");
    return false;
  }
#endif
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  char url[80];
  snprintf(url, sizeof(url), "http://%s:%u%s", HA_HOST, HA_PORT, HA_PATH);

  if (!http.begin(client, url)) {
    Log.println("HA: begin failed");
    return false;
  }

  esp_task_wdt_reset();
  const int code = http.GET();
  esp_task_wdt_reset();
  http.end();

  if (code >= 200 && code < 400) {
    Log.printf("HA: HTTP %d\n", code);
    return true;
  }

  if (code > 0) {
    Log.printf("HA: unhealthy HTTP %d\n", code);
  } else {
    Log.printf("HA: no reply (%d)\n", code);
  }
  return false;
}

static bool sshBannerOk() {
#ifdef QA_FAST
  if (injectSshFail) {
    Log.println("SSH: no banner");
    return false;
  }
#endif
  WiFiClient client;
  client.setTimeout(SSH_TIMEOUT_MS);
  if (!client.connect(SSH_HOST, SSH_PORT)) {
    Log.println("SSH: connect fail");
    return false;
  }

  char buf[96];
  size_t n = 0;
  buf[0] = 0;
  const uint32_t start = millis();
  while (millis() - start < (uint32_t)SSH_TIMEOUT_MS) {
    esp_task_wdt_reset();
    while (client.available()) {
      const int c = client.read();
      if (c < 0) {
        break;
      }
      if (n + 1 < sizeof(buf)) {
        buf[n++] = (char)c;
        buf[n] = 0;
      }
      if (strstr(buf, "SSH-") != nullptr) {
        client.stop();
        Log.println("SSH: banner OK");
        return true;
      }
    }
    delay(20);
  }
  client.stop();
  Log.println("SSH: no banner");
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
    return;
  }
  Log.println(reason);
  rebootsInWindow++;
  pingFailCount = 0;
  wedgeFailCount = 0;
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
  Log.println("esp32d-watchdog  ping + HA + SSH banner");
#ifdef QA_FAST
  Log.println("BUILD QA_FAST");
#endif
  Log.printf("Ping %s every %u s, reboot after %u misses (~%u min)\n",
                PING_HOST, CHECK_INTERVAL_MS / 1000, PING_FAIL_THRESHOLD,
                (PING_FAIL_THRESHOLD * CHECK_INTERVAL_MS) / 60000);
  Log.printf("HA :%u down AND SSH :%u no banner → reboot after %u misses\n",
                HA_PORT, SSH_PORT, WEDGE_FAIL_THRESHOLD);
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
        if (homeAssistantUp()) {
          wedgeFailCount = 0;
        } else if (sshBannerOk()) {
          wedgeFailCount = 0;
          Log.println("HA down, SSH banner OK — skip reboot");
        } else {
          wedgeFailCount++;
          Log.printf("Wedge miss %u / %u\n", wedgeFailCount,
                        WEDGE_FAIL_THRESHOLD);
          if (wedgeFailCount >= WEDGE_FAIL_THRESHOLD) {
            requestReboot("HA down and SSH wedged — rebooting Pi");
          }
        }
      } else {
        wedgeFailCount = 0;
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
