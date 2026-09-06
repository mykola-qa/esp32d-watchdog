#include "log_remote.h"

#include <WiFi.h>
#include <ESPmDNS.h>

#include "config.h"

static const size_t RING = 4096;
static char ring[RING];
static size_t ringHead = 0;
static size_t ringLen = 0;

static WiFiServer telnet(LOG_TELNET_PORT);
static WiFiClient telnetClient;
static bool started = false;

LogPrint Log;

static void ringPut(char c) {
  ring[ringHead] = c;
  ringHead = (ringHead + 1) % RING;
  if (ringLen < RING) {
    ringLen++;
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

  if (MDNS.begin(LOG_MDNS_HOST)) {
    MDNS.addService("telnet", "tcp", LOG_TELNET_PORT);
  }

  started = true;
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
}
