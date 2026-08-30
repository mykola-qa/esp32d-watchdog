#pragma once

#include <Arduino.h>

class LogPrint : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buf, size_t n) override;
};

extern LogPrint Log;

void logRemoteBegin();
void logRemoteLoop();
