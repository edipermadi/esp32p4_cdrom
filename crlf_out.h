#pragma once
#include <Arduino.h>

// Print wrapper around Serial that turns every bare "\n" into "\r\n", so
// raw-mode terminals (picocom, minicom, screen) start each line in column 0.
// Serial writes go straight to the UART driver and get no newline translation.
class CrlfOut : public Print {
public:
  size_t write(uint8_t c) override {
    if (c == '\n' && _last != '\r') Serial.write('\r');
    _last = c;
    return Serial.write(c);
  }
  size_t write(const uint8_t *buf, size_t len) override {
    for (size_t i = 0; i < len; i++) write(buf[i]);
    return len;
  }
  void flush() override { Serial.flush(); }

private:
  uint8_t _last = 0;
};

inline CrlfOut Term;
