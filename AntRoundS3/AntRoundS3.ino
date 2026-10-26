#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "Cst816Touch.h"
#include "BoardButtons.h"
#include "BatteryUi.h"
#include "Config.h"
#include "BmsLink.h"
#include "TouchInput.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This project is for ESP32-S3. Select ESP32S3 Dev Module."
#endif

Arduino_ESP32SPI bus(LCD_DC,LCD_CS,LCD_SCLK,LCD_MOSI,GFX_NOT_DEFINED,HSPI);
Arduino_GC9A01 panel(&bus,LCD_RST,DISPLAY_ROTATION,true,240,240);
Cst816Touch touch;
BoardButton keyUp(BUTTON_UP), keyPower(BUTTON_POWER), keyDown(BUTTON_DOWN);
constexpr uint16_t BG=0x0841, WHITE=0xef9c, RED=0xfac9;
int page=0;
unsigned selection=0;
bool choosing=false, touchOK=false, bmsOK=false;
bool redraw=true, touchLogging=false;
ui::TouchInput touchInput;
ui::Gesture gestures[4];
unsigned gestureCount=0;
uint32_t lastRender=0, lastTouch=0;

void handleTouch();
void dispatchAction(ui::Action action);

void discardTouch() {
  touchInput.cancel(); gestureCount=0;
}
void openPicker() {
  if(requestScan()) { discardTouch(); choosing=true; selection=0; redraw=true; }
}
void closePicker() {
  if(cancelPairing()) { discardTouch(); choosing=false; page=0; redraw=true; }
}
void dispatchAction(ui::Action action) {
  const auto s=bmsSnapshot();
  switch(action) {
    case ui::Action::Bluetooth: openPicker(); break;
    case ui::Action::Page:
      if(!choosing) { discardTouch(); page=(page+1)%3; redraw=true; }
      break;
    case ui::Action::PagePrevious:
      if(!choosing) { discardTouch(); page=(page+2)%3; redraw=true; }
      break;
    case ui::Action::Back: closePicker(); break;
    case ui::Action::Scan:
      if(s.state!=LinkState::Scanning) openPicker();
      break;
    case ui::Action::Connect:
      if(s.peerCount && s.state!=LinkState::Scanning && choosePeer(selection%s.peerCount)) {
        discardTouch(); choosing=false; page=0; redraw=true;
      }
      break;
    case ui::Action::Previous:
      if(s.peerCount>1) { selection=(selection+s.peerCount-1)%s.peerCount; redraw=true; }
      break;
    case ui::Action::Next:
      if(s.peerCount>1) { selection=(selection+1)%s.peerCount; redraw=true; }
      break;
    default: break;
  }
}
void handleTouch() {
  if(!TOUCH_ENABLED) return;
  static uint32_t lastGood=0;
  if(millis()-lastTouch<15) return;
  lastTouch=millis();
  Cst816Report report{};
  bool ok=touch.poll(report);
  ui::Sample sample=ui::Sample::Unknown;
  int x=0,y=0;
  if(ok) {
    touchOK=true; lastGood=millis();
    x=report.x; y=report.y;
    sample=report.fingers==0?ui::Sample::Release:ui::Sample::Contact;
  } else if(millis()-lastGood>250) touchOK=false;
  static ui::Sample previous=ui::Sample::Unknown;
  static int previousX=-1, previousY=-1;
  if(touchLogging && (sample!=previous || (sample==ui::Sample::Contact &&
      (x!=previousX || y!=previousY)))) {
    Serial.printf("TOUCH %s x=%d y=%d ID=0x%02X\n",
      sample==ui::Sample::Contact?"DOWN":sample==ui::Sample::Release?"UP":"READ ERROR",
      x,y,touch.chipId());
  }
  previous=sample; previousX=x; previousY=y;
  ui::Gesture event;
  if(touchInput.update(sample,x,y,millis(),event) && gestureCount<4)
    gestures[gestureCount++]=event;
}
void dispatchGestures() {
  // At most one action per frame/batch. A hold that opens the picker cannot
  // become a button tap when the user releases over the new page.
  for(unsigned i=0;i<gestureCount;i++) {
    const auto event=gestures[i];
    if(touchLogging) Serial.printf("GESTURE %u %d,%d -> %d,%d\n",
      unsigned(event.kind),event.x,event.y,event.endX,event.endY);
    const auto action=ui::gestureAction(choosing,event);
    if(action!=ui::Action::None) {
      dispatchAction(action);
      discardTouch();
      break;
    }
  }
  gestureCount=0;
}
bool handleButtons() {
  // Poll all keys before dispatch; at most one action per pass.
  const bool up=keyUp.poll(), power=keyPower.poll(), down=keyDown.poll();
  if(!up && !power && !down) return false;
  // A finger already resting on the old page must not activate the new page.
  discardTouch();
  if(up) dispatchAction(choosing?ui::Action::Back:ui::Action::Bluetooth);
  else if(down) dispatchAction(choosing?ui::Action::Next:ui::Action::Page);
  else if(power) {
    const auto s=bmsSnapshot();
    dispatchAction(!choosing?ui::Action::Bluetooth:
      s.peerCount?ui::Action::Connect:ui::Action::Scan);
  }
  return true;
}
void handleSerial() {
  static char command[32]; static unsigned used=0;
  while(Serial.available()) {
    char ch=Serial.read();
    if(ch=='\r') continue;
    if(ch=='\n') {
      command[used]=0; used=0;
      if(!strcmp(command,"scan")) openPicker();
      else if(!strcmp(command,"back")) { if(choosing) closePicker(); }
      else if(!strncmp(command,"pick ",5)) {
        int index=atoi(command+5);
        if(index>0 && choosePeer(unsigned(index-1))) { discardTouch(); choosing=false; page=0; redraw=true; }
        else Serial.println("Invalid device index; use scan first.");
      } else if(!strcmp(command,"page")) {
        if(choosing) closePicker(); else dispatchAction(ui::Action::Page);
      }
      else if(!strcmp(command,"raw on")) setRawLogging(true);
      else if(!strcmp(command,"raw off")) setRawLogging(false);
      else if(!strcmp(command,"touch on")) {
        touchLogging=TOUCH_ENABLED;
        Serial.println(TOUCH_ENABLED?"Touch logging ON":"Touch disabled: this board uses side buttons.");
      }
      else if(!strcmp(command,"touch off")) touchLogging=false;
      else Serial.println("Commands: scan | back | pick N | page | raw on/off | touch on/off | help. Baud: 115200.");
    } else if(used<sizeof(command)-1) command[used++]=ch;
  }
}
void setup() {
  Serial.begin(115200);
  keyUp.begin(); keyPower.begin(); keyDown.begin();
  // The power button has a hardware latch; no GPIO power-hold is required.
  // Keep the unused SD card deselected and hide LCD reset flashes.
  pinMode(SD_CS,OUTPUT); digitalWrite(SD_CS,HIGH);
  pinMode(LCD_BL,OUTPUT); digitalWrite(LCD_BL,LOW);
  delay(200);
  Serial.println("AntRound-S3 v0.11 power: LCD CS2 SCK3 DC18 MOSI10 RST21 BL42");
  Serial.printf("Flash=%lu MB PSRAM=%lu MB\n",
    (unsigned long)(ESP.getFlashChipSize()/1024/1024),
    (unsigned long)(ESP.getPsramSize()/1024/1024));
  if(!panel.begin(LCD_SPI_HZ)) {
    for(;;) { Serial.println("Display bus initialization failed"); delay(1000); }
  }
  // Show progress before touch or BLE initialization, even if either later stalls.
  panel.fillScreen(BG); panel.setTextColor(WHITE); panel.setTextSize(2);
  panel.setCursor(54,88); panel.print("ANT BATTERY");
  panel.setCursor(54,120); panel.print("STARTING...");
  digitalWrite(LCD_BL,HIGH);
  Serial.println("Display startup commands sent; initializing bundled LVGL");
  if(!beginBatteryUi(panel)) {
    panel.setCursor(54,155); panel.setTextSize(1); panel.print("UI ERROR");
    for(;;) { Serial.println("UI initialization failed"); delay(1000); }
  }
  if(TOUCH_ENABLED) {
    Serial.println("Starting CST816: SDA8 SCL9 RST0 INT11, I2C 0x15");
    touchOK=touch.begin(Wire);
    Serial.printf("CST816 ID=0x%02X setup=%s\n",touch.chipId(),
      touchOK?"OK":"FAILED; use side buttons / serial");
  } else {
    Serial.println("Touch disabled: no touch hardware; use side buttons.");
  }
  bmsOK=startBms();
  Serial.printf("AntRound-S3 v0.11 | touch=%d | free heap=%u | BLE task=%d\n",touchOK,ESP.getFreeHeap(),bmsOK);
  if(!bmsOK) {
    panel.setCursor(40,100); panel.setTextColor(RED); panel.print("BLE memory error");
  }
}
void loop() {
  if(!bmsOK) { delay(1000); return; }
  handleSerial(); handleTouch();
  if(!handleButtons()) dispatchGestures();
  const uint32_t now=millis();
  if(redraw || uint32_t(now-lastRender)>=50) {
    updateBatteryUi(bmsSnapshot(),page,choosing,selection,touchOK,redraw);
    redraw=false; lastRender=now;
  }
  tickBatteryUi(now);
  delay(1); // Yield to BLE while keeping the 25 ms UI schedule responsive.
}

