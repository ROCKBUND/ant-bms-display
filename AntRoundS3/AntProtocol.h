#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ant {
// Protocol ported from the supplied JLC BMS_Screen/main/main.c (status
// parsing around lines 619-741 and ble_write_task), verified against the
// three complete packets in BMS_Protocol_Table.markdown. See PORTING.md.
static constexpr uint8_t STATUS_REQUEST[] = {0x7e,0xa1,0x01,0x00,0x00,0xbe,0x18,0x55,0xaa,0x55};
struct Data {
  uint8_t cells = 0, temperatureCount = 0, status = 0;
  uint16_t soc = 0, cellMv[32] = {};
  int16_t temperature[4] = {}, mos = 0, balancer = 0;
  float volts = 0, amps = 0, capacityAh = 0, remainingAh = 0;
  uint16_t minMv = 0, maxMv = 0;
  uint8_t chargeMos = 0, dischargeMos = 0;
  bool protection = false, warning = false;
};
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | uint32_t(u16(p+2)) << 16; }
inline uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0xffff;
  while(n--) { c ^= *p++; for(int b=0;b<8;b++) c = (c & 1) ? (c>>1)^0xa001 : c>>1; }
  return c;
}
inline bool decode(const uint8_t* p, size_t n, Data& result) {
  if(n<10 || p[0]!=0x7e || p[1]!=0xa1 || p[2]!=0x11 || n!=size_t(p[5])+10) return false;
  if(p[n-2]!=0xaa || p[n-1]!=0x55 || crc16(p+1,n-5)!=u16(p+n-4)) return false;
  const unsigned cells=p[9], temps=p[8], off=2*(cells+temps);
  if(cells<1 || cells>32 || temps>4 || n<off+90) return false;
  Data d;
  d.cells=uint8_t(cells); d.temperatureCount=uint8_t(temps); d.status=p[7];
  d.soc=u16(p+42+off); if(d.soc>100) return false;
  d.minMv=0xffff;
  for(unsigned i=0;i<cells;i++) {
    d.cellMv[i]=u16(p+34+2*i);
    if(d.cellMv[i]<d.minMv) d.minMv=d.cellMv[i];
    if(d.cellMv[i]>d.maxMv) d.maxMv=d.cellMv[i];
  }
  for(unsigned i=0;i<temps;i++) d.temperature[i]=int16_t(u16(p+34+2*cells+2*i));
  d.mos=int16_t(u16(p+34+off)); d.balancer=int16_t(u16(p+36+off));
  d.volts=u16(p+38+off)*0.01f; d.amps=int16_t(u16(p+40+off))*0.1f;
  d.chargeMos=p[46+off]; d.dischargeMos=p[47+off];
  d.capacityAh=u32(p+50+off)*0.000001f; d.remainingAh=u32(p+54+off)*0.000001f;
  for(unsigned i=10;i<18;i++) d.protection |= p[i]!=0;
  for(unsigned i=18;i<26;i++) d.warning |= p[i]!=0;
  result=d; return true;
}

// Bounded byte-stream assembler. Accepts split headers, arbitrary ATT fragments,
// several frames per notification, and resynchronizes after corrupt frames.
class Parser {
 public:
  using Handler = void (*)(const Data&, void*);
  uint32_t accepted=0, rejected=0;
  void reset() { used_=0; }
  void feed(const uint8_t* bytes, size_t n, uint32_t now, Handler handler, void* context) {
    if(used_ && uint32_t(now-last_)>750) { reset(); rejected++; }
    last_=now;
    for(size_t i=0;i<n;i++) {
      if(used_==sizeof(buffer_)) discard(1);
      buffer_[used_++]=bytes[i];
      while(used_>=2) {
        if(buffer_[0]!=0x7e || buffer_[1]!=0xa1) { discard(1); continue; }
        if(used_<6) break;
        const size_t length=size_t(buffer_[5])+10;
        if(used_<length) break;
        if(buffer_[length-2]!=0xaa || buffer_[length-1]!=0x55 ||
           crc16(buffer_+1,length-5)!=u16(buffer_+length-4)) {
          rejected++; discard(1); continue;
        }
        Data d;
        if(buffer_[2]==0x11) {
          if(decode(buffer_,length,d)) { accepted++; handler(d,context); }
          else rejected++;
        }
        discard(length);
      }
    }
  }
 private:
  uint8_t buffer_[265]={}; size_t used_=0; uint32_t last_=0;
  void discard(size_t n) { used_-=n; memmove(buffer_,buffer_+n,used_); }
};
}
