#pragma once
#include <Arduino.h>

// Debounced release, once per short press. An initially held power button is
// ignored until released; long presses remain the board's hardware power action.
class BoardButton {
 public:
  explicit BoardButton(int pin):pin_(pin) {}
  void begin() {
    pinMode(pin_,INPUT_PULLUP);
    raw_=down_=digitalRead(pin_)==LOW;
    armed_=false; changed_=millis();
  }
  bool poll() {
    const uint32_t now=millis();
    const bool pressed=digitalRead(pin_)==LOW;
    if(pressed!=raw_) { raw_=pressed; changed_=now; }
    if(raw_==down_ || uint32_t(now-changed_)<30) return false;
    down_=raw_;
    if(down_) { began_=now; armed_=true; return false; }
    const bool fire=armed_ && uint32_t(now-began_)<800;
    armed_=false;
    return fire;
  }
 private:
  int pin_;
  bool raw_=false, down_=false, armed_=false;
  uint32_t changed_=0, began_=0;
};
