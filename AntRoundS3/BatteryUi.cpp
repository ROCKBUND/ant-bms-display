#include "BatteryUi.h"
#include "Config.h"
#include "TouchInput.h"
#include "src/lvgl/lvgl.h"
#include <math.h>
#include <string.h>

static_assert(LVGL_VERSION_MAJOR == 8 && LVGL_VERSION_MINOR == 3 &&
              LVGL_VERSION_PATCH == 11, "Use the bundled LVGL 8.3.11 sources");
static_assert(LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0,
              "Arduino_GFX expects native RGB565 pixels");

namespace {
constexpr uint32_t BG=0x050809, WHITE=0xF1F6F4, MUTED=0x9DAAA5;
constexpr uint32_t GREEN=0x38E4BE, TRACK=0x153B33, LINE=0x263A33;
constexpr uint32_t RED=0xFF6B70, AMBER=0xFFC568, CARD=0x0C1915;
constexpr unsigned FLOW_PERIOD_MS=2600, FLOW_FRAME_MS=50;
// 19,200 bytes; no full-screen buffer or PSRAM allocation is required.
lv_color_t pixels[240 * 40];
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t driver;
lv_disp_t* displayHandle=nullptr;
lv_obj_t* screen=nullptr;
Arduino_GC9A01* panel=nullptr;
Snapshot current;
int currentPage=0;
unsigned currentSelection=0;
bool picker=false, touchReady=false, haveSnapshot=false;
uint32_t lastTick=0, lastFlowFrame=0, flowTime=0;

void flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color) {
  // Blocking SPI write: LVGL can reuse the buffer only after it returns.
  panel->draw16bitRGBBitmap(area->x1, area->y1,
      reinterpret_cast<uint16_t*>(color),
      area->x2-area->x1+1, area->y2-area->y1+1);
  lv_disp_flush_ready(disp);
}

bool flowing() {
  return haveSnapshot && !picker && currentPage==0 && current.valid &&
         !current.data.protection && current.data.amps < -0.2f;
}

const lv_font_t* fitFont(const char* value, const lv_font_t* preferred, int width) {
  const lv_font_t* fonts[]={preferred, &lv_font_montserrat_24,
    &lv_font_montserrat_20, &lv_font_montserrat_18, &lv_font_montserrat_16,
    &lv_font_montserrat_14, &lv_font_montserrat_12, &lv_font_montserrat_10};
  for(const auto* font:fonts) {
    if(font->line_height>preferred->line_height) continue;
    lv_point_t size;
    lv_txt_get_size(&size,value,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if(size.x<=width) return font;
  }
  return &lv_font_montserrat_10;
}

void label(lv_draw_ctx_t* ctx, int x, int y, int width, const char* value,
           const lv_font_t* font=&lv_font_montserrat_12, uint32_t color=WHITE) {
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font=fitFont(value,font,width);
  dsc.color=lv_color_hex(color);
  dsc.align=LV_TEXT_ALIGN_CENTER;
  dsc.flag=LV_TEXT_FLAG_EXPAND;
  const lv_area_t box={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+width-1),
    lv_coord_t(y+dsc.font->line_height-1)};
  // Clip long names/status messages to their safe box, never into the ring.
  lv_area_t clip;
  const lv_area_t* savedClip=ctx->clip_area;
  if(_lv_area_intersect(&clip,savedClip,&box)) {
    ctx->clip_area=&clip;
    lv_draw_label(ctx,&dsc,&box,value,nullptr);
    ctx->clip_area=savedClip;
  }
}

void center(lv_draw_ctx_t* ctx, int y, int width, const char* value,
            const lv_font_t* font=&lv_font_montserrat_12, uint32_t color=WHITE) {
  label(ctx,(240-width)/2,y,width,value,font,color);
}

void rect(lv_draw_ctx_t* ctx, int x, int y, int w, int h,
          uint32_t color, int radius=0, uint32_t border=0) {
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color=lv_color_hex(color); dsc.bg_opa=LV_OPA_COVER;
  dsc.radius=radius;
  if(border) { dsc.border_width=1; dsc.border_color=lv_color_hex(border); }
  const lv_area_t box={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+w-1),lv_coord_t(y+h-1)};
  lv_draw_rect(ctx,&dsc,&box);
}

void dots(lv_draw_ctx_t* ctx) {
  for(int i=0;i<3;i++) rect(ctx,105+i*12,220,5,5,i==currentPage?GREEN:LINE,LV_RADIUS_CIRCLE);
}

void arc(lv_draw_ctx_t* ctx, unsigned start, unsigned end,
         lv_color_t color, bool rounded=true) {
  if(start==end) return;
  lv_draw_arc_dsc_t dsc;
  lv_draw_arc_dsc_init(&dsc);
  dsc.width=10; dsc.color=color; dsc.rounded=rounded;
  const lv_point_t middle={120,120};
  lv_draw_arc(ctx,&dsc,&middle,114,start,end);
}

void ring(lv_draw_ctx_t* ctx) {
  // LVGL angles: 0 at right, clockwise. A 270-degree arc opens at bottom.
  arc(ctx,135,405,lv_color_hex(TRACK));
  if(!current.valid || current.data.soc==0) return;
  const unsigned extent=(270u*current.data.soc+50u)/100u;
  const uint32_t base=current.data.protection || current.data.soc<=15 ? RED : GREEN;
  arc(ctx,135,135+extent,lv_color_hex(base));
  if(!flowing()) return;

  // Two travelling light tails INSIDE the real SOC arc. Its length and all
  // numeric readings stay unchanged; this never simulates rising charge.
  const float phase=float(flowTime)/FLOW_PERIOD_MS;
  for(unsigned offset=0;offset<extent;offset+=3) {
    const unsigned end=offset+3<extent?offset+3:extent;
    const float position=float(offset+end)/(2.0f*extent);
    float behind=2.0f*(phase-position);
    behind-=floorf(behind);
    float intensity=1.0f-behind/0.52f;
    if(intensity<=0) continue;
    intensity*=intensity;
    const auto color=lv_color_mix(lv_color_hex(0xD8FFF3),lv_color_hex(base),
                                 uint8_t(intensity*235.0f));
    arc(ctx,135+offset,135+end,color,false);
  }
}

const char* status() {
  if(!current.valid) {
    if(current.connected) return "WAIT / STALE";
    if(current.state==LinkState::Scanning) return "SCANNING";
    if(current.state==LinkState::Connecting) return "CONNECTING";
    return "OFFLINE";
  }
  if(current.data.protection) return "BMS PROTECTION";
  if(current.data.warning) return "BMS WARNING";
  if(current.data.amps < -0.2f) return "CHARGING";
  return current.data.amps > 0.2f?"DISCHARGING":"IDLE";
}

void overview(lv_draw_ctx_t* ctx) {
  ring(ctx);
  center(ctx,30,144,"ANT BATTERY",&lv_font_montserrat_12,MUTED);
  const uint32_t stateColor=!current.valid?AMBER:
    current.data.protection?RED:current.data.warning?AMBER:GREEN;
  center(ctx,49,164,status(),&lv_font_montserrat_12,stateColor);
  char b[48];
  if(current.valid) snprintf(b,sizeof(b),"%u%%",current.data.soc);
  else snprintf(b,sizeof(b),"--%%");
  center(ctx,71,172,b,&lv_font_montserrat_48,current.valid?WHITE:MUTED);
  rect(ctx,47,131,146,1,LINE);
  rect(ctx,119,141,1,28,LINE);
  label(ctx,35,137,80,"VOLTAGE",&lv_font_montserrat_10,MUTED);
  label(ctx,125,137,80,"CURRENT",&lv_font_montserrat_10,MUTED);
  if(current.valid) snprintf(b,sizeof(b),"%.2f V",current.data.volts);
  else snprintf(b,sizeof(b),"-- V");
  label(ctx,32,153,84,b,&lv_font_montserrat_18,current.valid?WHITE:MUTED);
  if(current.valid) snprintf(b,sizeof(b),"%+.1f A",current.data.amps);
  else snprintf(b,sizeof(b),"-- A");
  label(ctx,124,153,84,b,&lv_font_montserrat_18,current.valid?WHITE:MUTED);
  center(ctx,179,90,"POWER",&lv_font_montserrat_10,MUTED);
  if(current.valid) {
    const float watts=current.data.volts*current.data.amps;
    if(fabsf(watts)<0.5f) snprintf(b,sizeof(b),"0 W");
    else snprintf(b,sizeof(b),"%+.0f W",watts);
  } else snprintf(b,sizeof(b),"-- W");
  center(ctx,192,156,b,&lv_font_montserrat_24,current.valid?GREEN:MUTED);
  dots(ctx);
}

void metric(lv_draw_ctx_t* ctx, int x, int y, const char* title,
            const char* value, uint32_t color=WHITE) {
  label(ctx,x,y,94,title,&lv_font_montserrat_10,MUTED);
  label(ctx,x,y+19,94,value,&lv_font_montserrat_20,color);
}

void details(lv_draw_ctx_t* ctx) {
  center(ctx,30,174,"BATTERY DETAILS",&lv_font_montserrat_14);
  rect(ctx,119,65,1,108,LINE); rect(ctx,26,117,188,1,LINE);
  char b[48];
  const uint32_t color=current.valid?WHITE:MUTED;
  if(current.valid) snprintf(b,sizeof(b),"%.2f Ah",current.data.remainingAh); else snprintf(b,sizeof(b),"-- Ah");
  metric(ctx,23,67,"REMAINING",b,color);
  if(current.valid) snprintf(b,sizeof(b),"%.2f Ah",current.data.capacityAh); else snprintf(b,sizeof(b),"-- Ah");
  metric(ctx,123,67,"CAPACITY",b,color);
  if(current.valid) snprintf(b,sizeof(b),"%d C",current.data.mos); else snprintf(b,sizeof(b),"-- C");
  metric(ctx,23,131,"MOS TEMP",b,color);
  if(current.valid) snprintf(b,sizeof(b),"%u mV",current.data.maxMv-current.data.minMv); else snprintf(b,sizeof(b),"-- mV");
  metric(ctx,123,131,"CELL DELTA",b,color);
  rect(ctx,48,183,144,1,LINE);
  if(current.valid) snprintf(b,sizeof(b),"%uS  %.3f-%.3f V",current.data.cells,
                            current.data.minMv/1000.f,current.data.maxMv/1000.f);
  else snprintf(b,sizeof(b),"Waiting for data");
  center(ctx,194,168,b,&lv_font_montserrat_10,MUTED);
  dots(ctx);
}

void diagnostics(lv_draw_ctx_t* ctx) {
  center(ctx,30,170,"CONNECTION",&lv_font_montserrat_14);
  center(ctx,58,174,status(),&lv_font_montserrat_14,current.valid?GREEN:AMBER);
  center(ctx,83,200,current.connectedPeer.address[0]?current.connectedPeer.address:"Not paired",
         &lv_font_montserrat_12);
  center(ctx,105,206,current.message,&lv_font_montserrat_10,MUTED);
  rect(ctx,42,126,156,1,LINE);
  char b[64];
  snprintf(b,sizeof(b),"Frames  %lu",(unsigned long)current.frames);
  center(ctx,138,182,b);
  snprintf(b,sizeof(b),"CRC/format %lu   RX lost %lu",(unsigned long)current.rejected,(unsigned long)current.drops);
  center(ctx,163,180,b,&lv_font_montserrat_10,MUTED);
  snprintf(b,sizeof(b),"Touch %s / %s",TOUCH_ENABLED?(touchReady?"OK":"FAIL"):"OFF",
           current.saved?"Saved":"Unsaved");
  center(ctx,190,160,b,&lv_font_montserrat_10,MUTED);
  dots(ctx);
}

void touchButton(lv_draw_ctx_t* ctx, const ui::Button& button, bool enabled=true) {
  rect(ctx,button.x,button.y,button.w,button.h,CARD,6,enabled?GREEN:LINE);
  label(ctx,button.x+2,button.y+(button.h-14)/2,button.w-4,button.label,
        &lv_font_montserrat_12,enabled?WHITE:MUTED);
}

void devicePicker(lv_draw_ctx_t* ctx) {
  center(ctx,26,156,"SELECT BMS",&lv_font_montserrat_16);
  char b[64];
  const bool scanning=current.state==LinkState::Scanning;
  if(current.peerCount) {
    const unsigned index=currentSelection%current.peerCount;
    const auto& p=current.peers[index];
    snprintf(b,sizeof(b),"%u / %u",index+1,current.peerCount);
    center(ctx,51,100,b,&lv_font_montserrat_12,MUTED);
    const int top=TOUCH_ENABLED?69:80;
    rect(ctx,21,top-5,198,TOUCH_ENABLED?63:79,CARD,12,GREEN);
    center(ctx,top+2,186,p.name[0]?p.name:"ANT device",&lv_font_montserrat_14);
    center(ctx,top+24,186,p.address,&lv_font_montserrat_12,MUTED);
    snprintf(b,sizeof(b),"%d dBm",p.rssi);
    center(ctx,top+43,154,b,&lv_font_montserrat_12,GREEN);
    if(!TOUCH_ENABLED) center(ctx,165,176,scanning?"SCANNING...":"READY TO CONNECT",
                             &lv_font_montserrat_10,scanning?AMBER:GREEN);
  } else {
    center(ctx,86,184,scanning?"SCANNING...":"NO ANT DEVICE",&lv_font_montserrat_16,AMBER);
    center(ctx,118,186,"Close phone BMS app",&lv_font_montserrat_12,MUTED);
  }
  if(TOUCH_ENABLED) {
    if(current.peerCount) {
      touchButton(ctx,ui::PREV,current.peerCount>1);
      touchButton(ctx,ui::NEXT,current.peerCount>1);
      touchButton(ctx,ui::CONNECT,!scanning);
    }
    touchButton(ctx,ui::BACK); touchButton(ctx,ui::SCAN,!scanning);
  } else {
    center(ctx,187,168,current.peerCount?"UP Back   DOWN Next":"UP Back",
           &lv_font_montserrat_10,MUTED);
    center(ctx,205,152,scanning?"Scanning...":current.peerCount?"CENTER Connect":"CENTER Scan",
           &lv_font_montserrat_10,MUTED);
  }
}

void render(lv_event_t* event) {
  auto* ctx=lv_event_get_draw_ctx(event);
  if(!haveSnapshot) return;
  if(picker) devicePicker(ctx);
  else if(currentPage==0) overview(ctx);
  else if(currentPage==1) details(ctx);
  else diagnostics(ctx);
}

void invalidateRing() {
  // Avoid sending the centre's unchanged numbers on every animation frame.
  const lv_area_t regions[]={{4,4,235,47},{4,48,49,195},
                            {190,48,235,195},{30,196,209,235}};
  for(const auto& area:regions) lv_obj_invalidate_area(screen,&area);
}

bool sameVisibleData(const Snapshot& a, const Snapshot& b) {
  const auto& x=a.data; const auto& y=b.data;
  // Deliberately compare fields, not struct padding. Do not include updated:
  // receiving an identical packet should not force a full-screen repaint.
  return a.valid==b.valid && a.connected==b.connected && a.state==b.state &&
    a.saved==b.saved && a.peerCount==b.peerCount &&
    !strcmp(a.message,b.message) && !strcmp(a.connectedPeer.address,b.connectedPeer.address) &&
    x.soc==y.soc && x.volts==y.volts && x.amps==y.amps &&
    x.remainingAh==y.remainingAh && x.capacityAh==y.capacityAh && x.mos==y.mos &&
    x.cells==y.cells && x.minMv==y.minMv && x.maxMv==y.maxMv &&
    x.protection==y.protection && x.warning==y.warning;
}
} // namespace

bool beginBatteryUi(Arduino_GC9A01& display) {
  panel=&display;
  lv_init();
  lv_disp_draw_buf_init(&drawBuffer,pixels,nullptr,240*40);
  lv_disp_drv_init(&driver);
  driver.hor_res=240; driver.ver_res=240;
  driver.flush_cb=flush; driver.draw_buf=&drawBuffer;
  driver.antialiasing=1;
  displayHandle=lv_disp_drv_register(&driver);
  if(!displayHandle) return false;
  screen=lv_disp_get_scr_act(displayHandle);
  if(!screen) return false;
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);
  lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(screen,render,LV_EVENT_DRAW_MAIN,nullptr);
  lastTick=lastFlowFrame=millis();
  return true;
}

void updateBatteryUi(const Snapshot& snapshot, int page, bool choosing,
                     unsigned selection, bool touchOK, bool force) {
  if(!screen) return;
  const bool wasFlowing=flowing();
  bool changed=force || !haveSnapshot || page!=currentPage || choosing!=picker ||
    selection!=currentSelection || touchOK!=touchReady || !sameVisibleData(current,snapshot);
  if(page==2 && !choosing && (current.frames!=snapshot.frames ||
      current.rejected!=snapshot.rejected || current.drops!=snapshot.drops)) changed=true;
  if(!changed && choosing) {
    for(unsigned i=0;i<snapshot.peerCount && i<6;i++) {
      if(strcmp(current.peers[i].name,snapshot.peers[i].name) ||
         strcmp(current.peers[i].address,snapshot.peers[i].address) ||
         current.peers[i].rssi!=snapshot.peers[i].rssi) { changed=true; break; }
    }
  }
  current=snapshot; currentPage=page; picker=choosing;
  currentSelection=selection; touchReady=touchOK; haveSnapshot=true;
  if(!wasFlowing && flowing()) flowTime=0;
  if(changed) lv_obj_invalidate(screen);
}

void tickBatteryUi(uint32_t now) {
  if(!screen) return;
  const uint32_t elapsed=now-lastTick;
  lastTick=now;
  lv_tick_inc(elapsed);
  if(flowing()) {
    flowTime=(flowTime+elapsed%FLOW_PERIOD_MS)%FLOW_PERIOD_MS;
    if(uint32_t(now-lastFlowFrame)>=FLOW_FRAME_MS) {
      lastFlowFrame=now; invalidateRing();
    }
  } else lastFlowFrame=now;
  lv_timer_handler();
}
