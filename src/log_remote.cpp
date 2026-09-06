#include "log_remote.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

#include "config.h"

static const size_t RING = 4096;
static char ring[RING];
static size_t ringHead = 0;
static size_t ringLen = 0;

static WiFiServer telnet(LOG_TELNET_PORT);
static WiFiClient telnetClient;
static WebServer web(LOG_HTTP_PORT);
static bool started = false;

LogPrint Log;

static const char PAGE_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>esp32d-watchdog</title>"
    "<style>body{font:14px/1.4 sans-serif;margin:1rem;background:#111;color:#ddd}"
    "pre{white-space:pre-wrap;background:#000;padding:1rem;overflow:auto}</style>"
    "</head><body><h1>esp32d-watchdog</h1>"
    "<p>Read-only diagnostics</p><pre>";

static const char PAGE_TAIL[] PROGMEM = "</pre></body></html>";

static void ringPut(char c) {
  ring[ringHead] = c;
  ringHead = (ringHead + 1) % RING;
  if (ringLen < RING) {
    ringLen++;
  }
}

static char ringAt(size_t index) {
  if (ringLen < RING) {
    return ring[index];
  }
  return ring[(ringHead + index) % RING];
}

static void sendRingHtml() {
  char chunk[256];
  size_t used = 0;

  auto append = [&](const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
      if (used == sizeof(chunk)) {
        web.sendContent(chunk, used);
        used = 0;
        esp_task_wdt_reset();
        yield();
      }
      chunk[used++] = text[i];
    }
  };

  for (size_t i = 0; i < ringLen; i++) {
    const char c = ringAt(i);
    if (c == '<') {
      append("&lt;", 4);
    } else if (c == '>') {
      append("&gt;", 4);
    } else if (c == '&') {
      append("&amp;", 5);
    } else {
      append(&c, 1);
    }
  }
  if (used) {
    web.sendContent(chunk, used);
  }
}

static void dumpRingTo(Print &out) {
  if (ringLen < RING) {
    out.write(reinterpret_cast<const uint8_t *>(ring), ringLen);
    return;
  }
  out.write(reinterpret_cast<const uint8_t *>(ring + ringHead), RING - ringHead);
  out.write(reinterpret_cast<const uint8_t *>(ring), ringHead);
}

static void handleRoot() {
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/html; charset=utf-8", "");
  web.sendContent_P(PAGE_HEAD);
  sendRingHtml();
  web.sendContent_P(PAGE_TAIL);
  web.sendContent("");
}

size_t LogPrint::write(uint8_t c) { return write(&c, 1); }

size_t LogPrint::write(const uint8_t *buf, size_t n) {
  Serial.write(buf, n);
  for (size_t i = 0; i < n; i++) {
    ringPut(static_cast<char>(buf[i]));
  }
  if (telnetClient && telnetClient.connected()) {
    const int writable = telnetClient.availableForWrite();
    if (writable > 0) {
      const size_t count = min(n, static_cast<size_t>(writable));
      telnetClient.write(buf, count);
    }
  }
  return n;
}

void logRemoteBegin() {
  if (started || WiFi.status() != WL_CONNECTED) {
    return;
  }

  telnet.begin();

  web.on("/", HTTP_GET, handleRoot);
  web.begin();

  if (MDNS.begin(LOG_MDNS_HOST)) {
    MDNS.addService("http", "tcp", LOG_HTTP_PORT);
    MDNS.addService("telnet", "tcp", LOG_TELNET_PORT);
  }

  started = true;
  Log.printf("Logs: http://%s/  or  http://%s.local/\n",
             WiFi.localIP().toString().c_str(), LOG_MDNS_HOST);
  Log.printf("Read-only telnet: nc %s %u\n",
             WiFi.localIP().toString().c_str(), LOG_TELNET_PORT);
}

void logRemoteStop() {
  if (!started) {
    return;
  }
  if (telnetClient) {
    telnetClient.stop();
  }
  telnet.stop();
  web.stop();
  MDNS.end();
  started = false;
}

static void acceptTelnet() {
  if (!telnet.hasClient()) {
    return;
  }
  WiFiClient incoming = telnet.accept();
  if (telnetClient && telnetClient.connected()) {
    incoming.println("busy — one telnet client only");
    incoming.stop();
    return;
  }
  telnetClient = incoming;
  telnetClient.setNoDelay(true);
  telnetClient.println("esp32d-watchdog  read-only diagnostics");
  dumpRingTo(telnetClient);
}

void logRemoteLoop() {
  if (!started) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    logRemoteStop();
    return;
  }
  acceptTelnet();
  if (telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
  }
  web.handleClient();
}
