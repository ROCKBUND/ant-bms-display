#include "Cst816Touch.h"
#include "Config.h"

namespace { constexpr uint8_t ADDRESS = 0x15; }

bool Cst816Touch::read(uint8_t reg, uint8_t* data, size_t size) {
  if(!wire_) return false;
  wire_->beginTransmission(ADDRESS);
  wire_->write(reg);
  if(wire_->endTransmission(false)!=0) return false;
  const size_t received=wire_->requestFrom(ADDRESS,size,true);
  if(received!=size) {
    while(wire_->available()) wire_->read();
    return false;
  }
  for(size_t i=0;i<size;i++) data[i]=wire_->read();
  return true;
}

bool Cst816Touch::writeChecked(uint8_t reg, uint8_t value) {
  wire_->beginTransmission(ADDRESS);
  wire_->write(reg); wire_->write(value);
  if(wire_->endTransmission()!=0) return false;
  uint8_t actual=0;
  return read(reg,&actual,1) && actual==value;
}

bool Cst816Touch::begin(TwoWire& wire) {
  ready_=false; chipId_=0; wire_=&wire;
  if(!TOUCH_ENABLED) return false;
  // GPIO0 is also the BOOT button: release it using open drain, never drive
  // it high against a pressed button. This reset runs only after ESP32 boots.
  pinMode(TOUCH_RST,OUTPUT_OPEN_DRAIN);
  pinMode(TOUCH_INT,INPUT_PULLUP);
  digitalWrite(TOUCH_RST,LOW); delay(10);
  digitalWrite(TOUCH_RST,HIGH); delay(50);
  if(!wire_->begin(TOUCH_SDA,TOUCH_SCL,TOUCH_I2C_HZ)) return false;
  wire_->setTimeOut(30);
  bool identified=false;
  for(unsigned attempt=0;attempt<3 && !identified;attempt++) {
    identified=read(0xA7,&chipId_,1);
    if(!identified) delay(10);
  }
  // IDs listed in the supplied board example: CST816S/T/D.
  if(!identified || (chipId_!=0xB4 && chipId_!=0xB5 && chipId_!=0xB6)) return false;
  // CST816S register manual: keep polling available at idle and stop automatic
  // resets during a held finger. Read back each write; fail closed on mismatch.
  if(!writeChecked(0xFE,0x01) || !writeChecked(0xFB,0x00) ||
     !writeChecked(0xFC,0x00)) return false;
  ready_=true;
  return true;
}

bool Cst816Touch::poll(Cst816Report& report) {
  report={};
  if(!ready_) return false;
  // FingerNum and all coordinate bytes are read together (0x02..0x06).
  // Missing IRQs or I2C failures are never interpreted as a finger release.
  uint8_t bytes[5];
  if(!read(0x02,bytes,sizeof(bytes))) return false;
  report.fingers=bytes[0];
  if(report.fingers==0) return true;
  if(report.fingers!=1) return false;
  int x=((bytes[1]&0x0F)<<8)|bytes[2];
  int y=((bytes[3]&0x0F)<<8)|bytes[4];
  if(x>=240 || y>=240) return false;
  if(TOUCH_SWAP_XY) { int t=x; x=y; y=t; }
  switch(TOUCH_ROTATION&3) {
    case 1: { int t=x; x=y; y=239-t; } break;
    case 2: x=239-x; y=239-y; break;
    case 3: { int t=x; x=239-y; y=t; } break;
    default: break;
  }
  if(TOUCH_MIRROR_X) x=239-x;
  if(TOUCH_MIRROR_Y) y=239-y;
  report.x=x; report.y=y;
  return true;
}
