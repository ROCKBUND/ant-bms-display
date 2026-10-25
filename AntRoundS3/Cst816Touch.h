#pragma once
#include <Arduino.h>
#include <Wire.h>

struct Cst816Report {
  uint8_t fingers = 0;
  uint16_t x = 0, y = 0;
};

class Cst816Touch {
 public:
  bool begin(TwoWire& wire);
  bool poll(Cst816Report& report);
  uint8_t chipId() const { return chipId_; }
 private:
  bool read(uint8_t reg, uint8_t* data, size_t size);
  bool writeChecked(uint8_t reg, uint8_t value);
  TwoWire* wire_ = nullptr;
  uint8_t chipId_ = 0;
  bool ready_ = false;
};
