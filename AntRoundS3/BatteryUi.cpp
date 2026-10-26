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
constexpr unsigned FLOW_PERIOD_MS=2600, UI_FRAME_MS=25;
constexpr unsigned RING_STEP=2, RING_PARTS=270/RING_STEP, RING_SECTORS=15;
constexpr uint32_t SWEEP_ZERO_MS=100, SWEEP_RISE_MS=1400, SWEEP_PEAK_MS=200;
constexpr uint32_t SWEEP_SETTLE_MS=400;
// At y=49 the old 164px status box touched the ring at both upper corners.
// Keep drawing and invalidation inside the same, narrower interior rectangle.
constexpr int STATUS_X=50, STATUS_Y=49, STATUS_WIDTH=140;
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
uint32_t lastTick=0, lastFrame=0, flowTime=0;
bool fullRefreshPending=true, haveHomeFrame=false;
lv_area_t ringPartAreas[RING_PARTS], ringSectorAreas[RING_SECTORS];
lv_color_t ringColors[RING_PARTS];
unsigned ringExtent=0;
uint32_t ringBase=GREEN;
bool ringFlowing=false;
struct HomeFrame {
  char soc[12]={}, state[24]={}, volts[24]={}, amps[24]={}, watts[24]={};
  uint32_t socColor=WHITE, stateColor=GREEN, wattsColor=GREEN;
  bool valid=false;
} home;
enum class SweepKind { None, Boot, Connected };
SweepKind sweepKind=SweepKind::None;
bool connectionSweepPending=true, sweepFirstFrame=false, sweepAtPeak=false;
bool sweepSettling=false;
unsigned sweepPercent=0;
unsigned sweepTarget=0, sweepSettleArc=270;
uint32_t sweepTime=0;

bool sweeping() { return sweepKind!=SweepKind::None; }
unsigned displayedSoc() { return sweeping()?sweepPercent:current.data.soc; }

void beginSweep(SweepKind kind) {
  sweepKind=kind; sweepPercent=0; sweepTime=0;
  sweepAtPeak=false; sweepFirstFrame=true; sweepSettling=false;
  // Dirty areas are collected once per displayed frame, not every loop pass.
}

bool canStartConnectionSweep() {
  return connectionSweepPending && current.connected && current.valid &&
    !picker && currentPage==0 && !current.data.protection && !current.data.warning;
}

void startPendingConnectionSweep() {
  if(!sweeping() && canStartConnectionSweep()) {
    connectionSweepPending=false;
    beginSweep(SweepKind::Connected);
  }
}

void flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color) {
  // Blocking SPI write: LVGL can reuse the buffer only after it returns.
  panel->draw16bitRGBBitmap(area->x1, area->y1,
      reinterpret_cast<uint16_t*>(color),
      area->x2-area->x1+1, area->y2-area->y1+1);
  lv_disp_flush_ready(disp);
}

bool flowing() {
  return haveSnapshot && !sweeping() && !picker && currentPage==0 && current.valid &&
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
  // Reject before measuring glyphs: most labels do not intersect an arc update.
  const lv_area_t bounds={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+width-1),
    lv_coord_t(y+font->line_height-1)};
  if(!_lv_area_is_on(ctx->clip_area,&bounds)) return;
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
  lv_area_t bounds;
  lv_draw_arc_get_area(120,120,114,start,end,10,rounded,&bounds);
  lv_area_increase(&bounds,1,1);
  if(!_lv_area_is_on(ctx->clip_area,&bounds)) return;
  lv_draw_arc_dsc_t dsc;
  lv_draw_arc_dsc_init(&dsc);
  dsc.width=10; dsc.color=color; dsc.rounded=rounded;
  const lv_point_t middle={120,120};
  lv_draw_arc(ctx,&dsc,&middle,114,start,end);
}

void ring(lv_draw_ctx_t* ctx) {
  // Only skip rectangles wholly inside the empty centre. Any rectangle that
  // could erase the ring must repaint it, independent of sector-bound culling.
  // Radius 102 leaves room for the inner edge's pixel alignment/antialiasing.
  const auto& clip=*ctx->clip_area;
  const int dx1=clip.x1-120, dx2=clip.x2-120;
  const int dy1=clip.y1-120, dy2=clip.y2-120;
  const int dxSquared=dx1*dx1>dx2*dx2?dx1*dx1:dx2*dx2;
  const int dySquared=dy1*dy1>dy2*dy2?dy1*dy1:dy2*dy2;
  if(dxSquared+dySquared<=102*102) return;
  // LVGL angles: 0 at right, clockwise. A 270-degree arc opens at bottom.
  arc(ctx,135,405,lv_color_hex(TRACK));
  if(!ringExtent) return;
  arc(ctx,135,135+ringExtent,lv_color_hex(ringBase));
  if(!ringFlowing) return;
  // Colours and bounds were prepared once before LVGL starts rendering strips.
  const auto baseColor=lv_color_hex(ringBase);
  for(unsigned i=0;i<RING_PARTS && i*RING_STEP<ringExtent;i++) {
    if(ringColors[i].full==baseColor.full ||
       !_lv_area_is_on(ctx->clip_area,&ringPartAreas[i])) continue;
    const unsigned end=(i+1)*RING_STEP<ringExtent?(i+1)*RING_STEP:ringExtent;
    arc(ctx,135+i*RING_STEP,135+end,ringColors[i],false);
  }
}

const char* status() {
  if(sweeping()) return "LOADING SOC";
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
  center(ctx,30,144,"ANT BATTERY",&lv_font_montserrat_12,MUTED);
  label(ctx,STATUS_X,STATUS_Y,STATUS_WIDTH,home.state,&lv_font_montserrat_12,home.stateColor);
  center(ctx,71,172,home.soc,&lv_font_montserrat_48,home.socColor);
  rect(ctx,47,131,146,1,LINE);
  rect(ctx,119,141,1,28,LINE);
  label(ctx,35,137,80,"VOLTAGE",&lv_font_montserrat_10,MUTED);
  label(ctx,125,137,80,"CURRENT",&lv_font_montserrat_10,MUTED);
  label(ctx,32,153,84,home.volts,&lv_font_montserrat_18,home.valid?WHITE:MUTED);
  label(ctx,124,153,84,home.amps,&lv_font_montserrat_18,home.valid?WHITE:MUTED);
  center(ctx,179,90,"POWER",&lv_font_montserrat_10,MUTED);
  center(ctx,192,156,home.watts,&lv_font_montserrat_24,home.wattsColor);
  dots(ctx);
  // Restore the ring after all content in this dirty rectangle. Never flush a
  // separate clearing pass between the text and the ring.
  ring(ctx);
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

void dirty(const lv_area_t& area) {
  lv_obj_invalidate_area(screen,&area);
}

void dirtyLabel(int x, int y, int width, const lv_font_t* font) {
  const lv_area_t area={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+width-1),
    lv_coord_t(y+font->line_height-1)};
  dirty(area);
}

void prepareRingGeometry() {
  for(unsigned i=0;i<RING_PARTS;i++) {
    lv_draw_arc_get_area(120,120,114,135+i*RING_STEP,135+(i+1)*RING_STEP,
                        10,false,&ringPartAreas[i]);
    lv_area_increase(&ringPartAreas[i],1,1);
  }
  for(unsigned i=0;i<RING_SECTORS;i++) {
    lv_draw_arc_get_area(120,120,114,135+i*18,135+(i+1)*18,
                        10,true,&ringSectorAreas[i]);
    lv_area_increase(&ringSectorAreas[i],1,1);
  }
}

void prepareRing(bool full) {
  // Drive the sweep arc by elapsed time, independently of rounded percent text.
  const unsigned extent=sweeping()?
    (sweepSettling?sweepSettleArc:sweepAtPeak?270:sweepTime<=SWEEP_ZERO_MS?0:
      unsigned((sweepTime-SWEEP_ZERO_MS)*270u/SWEEP_RISE_MS)):
    (current.valid?(270u*current.data.soc+50u)/100u:0);
  const uint32_t base=sweeping()?GREEN:
    current.data.protection || current.data.soc<=15 ? RED : GREEN;
  const bool flow=flowing();
  bool sectorDirty[RING_SECTORS]={};
  // A base-colour/flow-mode change also clears the old highlight and rounded caps.
  if(!full && (base!=ringBase || flow!=ringFlowing ||
               (extent!=ringExtent && (flow || ringFlowing)))) {
    for(auto& changed:sectorDirty) changed=true;
  } else if(!full && extent!=ringExtent) {
    // Include both old and new rounded ends. Split large jumps to keep the
    // invalidations out of the centre, e.g. when 100% returns to actual SOC.
    const unsigned low=extent<ringExtent?extent:ringExtent;
    const unsigned high=extent>ringExtent?extent:ringExtent;
    for(unsigned from=low;from<high;from+=18) {
      const unsigned to=from+18<high?from+18:high;
      lv_area_t area;
      lv_draw_arc_get_area(120,120,114,135+from,135+to,10,true,&area);
      lv_area_increase(&area,1,1); dirty(area);
    }
  }
  const auto baseColor=lv_color_hex(base);
  const float phase=float(flowTime)/FLOW_PERIOD_MS;
  for(unsigned i=0;i<RING_PARTS;i++) {
    auto color=baseColor;
    const unsigned from=i*RING_STEP;
    if(flow && from<extent) {
      const unsigned to=from+RING_STEP<extent?from+RING_STEP:extent;
      const float position=float(from+to)/(2.0f*extent);
      float behind=2.0f*(phase-position);
      behind-=floorf(behind);
      float intensity=1.0f-behind/0.52f;
      if(intensity>0) {
        intensity*=intensity;
        color=lv_color_mix(lv_color_hex(0xD8FFF3),baseColor,uint8_t(intensity*235.0f));
      }
    }
    if(!full && (flow || ringFlowing) && color.full!=ringColors[i].full)
      sectorDirty[i/9]=true;
    ringColors[i]=color;
  }
  if(!full) for(unsigned i=0;i<RING_SECTORS;i++) if(sectorDirty[i]) dirty(ringSectorAreas[i]);
  ringExtent=extent; ringBase=base; ringFlowing=flow;
}

void prepareHomeFrame(bool full) {
  HomeFrame next;
  next.valid=current.valid;
  next.socColor=current.valid || sweeping()?WHITE:MUTED;
  next.stateColor=sweeping()?GREEN:!current.valid?AMBER:
    current.data.protection?RED:current.data.warning?AMBER:GREEN;
  next.wattsColor=current.valid?GREEN:MUTED;
  snprintf(next.state,sizeof(next.state),"%s",status());
  if(current.valid || sweeping()) snprintf(next.soc,sizeof(next.soc),"%u%%",displayedSoc());
  else snprintf(next.soc,sizeof(next.soc),"--%%");
  if(current.valid) {
    snprintf(next.volts,sizeof(next.volts),"%.2f V",current.data.volts);
    snprintf(next.amps,sizeof(next.amps),"%+.1f A",current.data.amps);
    const float watts=current.data.volts*current.data.amps;
    if(fabsf(watts)<0.5f) snprintf(next.watts,sizeof(next.watts),"0 W");
    else snprintf(next.watts,sizeof(next.watts),"%+.0f W",watts);
  } else {
    snprintf(next.volts,sizeof(next.volts),"-- V");
    snprintf(next.amps,sizeof(next.amps),"-- A");
    snprintf(next.watts,sizeof(next.watts),"-- W");
  }
  if(!full) {
    if(strcmp(home.state,next.state) || home.stateColor!=next.stateColor)
      dirtyLabel(STATUS_X,STATUS_Y,STATUS_WIDTH,&lv_font_montserrat_12);
    if(strcmp(home.soc,next.soc) || home.socColor!=next.socColor)
      dirtyLabel(34,71,172,&lv_font_montserrat_48);
    if(strcmp(home.volts,next.volts) || home.valid!=next.valid)
      dirtyLabel(32,153,84,&lv_font_montserrat_18);
    if(strcmp(home.amps,next.amps) || home.valid!=next.valid)
      dirtyLabel(124,153,84,&lv_font_montserrat_18);
    if(strcmp(home.watts,next.watts) || home.wattsColor!=next.wattsColor)
      dirtyLabel(42,192,156,&lv_font_montserrat_24);
  }
  prepareRing(full);
  home=next; haveHomeFrame=true;
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
  prepareRingGeometry();
  fullRefreshPending=true; haveHomeFrame=false;
  // Boot sweep is presentation only: unknown voltage/current/power remain --.
  current=Snapshot{}; currentPage=0; picker=false; haveSnapshot=true;
  connectionSweepPending=true;
  beginSweep(SweepKind::Boot);
  lastTick=millis(); lastFrame=lastTick-UI_FRAME_MS;
  return true;
}

void updateBatteryUi(const Snapshot& snapshot, int page, bool choosing,
                     unsigned selection, bool touchOK, bool force) {
  if(!screen) return;
  const bool wasFlowing=flowing();
  const bool pageChanged=!haveSnapshot || page!=currentPage || choosing!=picker;
  const bool newConnection=!haveSnapshot || !snapshot.connected ||
    !current.connected || strcmp(current.connectedPeer.address,snapshot.connectedPeer.address)!=0;
  if(newConnection) connectionSweepPending=true;
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
  // Page changes cancel the visible sweep; never hold up buttons or scanning.
  // A stale frame cancels a connection sweep without replaying it on recovery.
  const bool alarm=current.valid && (current.data.protection || current.data.warning);
  if(sweeping() && (picker || currentPage!=0 || alarm ||
      (sweepKind==SweepKind::Connected &&
       (newConnection || !current.connected || !current.valid)))) {
    sweepKind=SweepKind::None; changed=true;
  }
  // Prefer actual warning/protection data over a decorative loading sweep.
  if(alarm && current.connected) connectionSweepPending=false;
  startPendingConnectionSweep();
  if(!wasFlowing && flowing()) flowTime=0;
  if(force || pageChanged || (changed && (picker || currentPage!=0)))
    fullRefreshPending=true;
  if(pageChanged) haveHomeFrame=false;
}

void tickBatteryUi(uint32_t now) {
  if(!screen) return;
  const uint32_t elapsed=now-lastTick;
  lastTick=now;
  lv_tick_inc(elapsed);
  bool showBoundary=false;
  if(sweeping()) {
    if(sweepFirstFrame) {
      // Paint 0 explicitly before starting the clock, including after BLE init.
      sweepFirstFrame=false; showBoundary=true;
    } else if(sweepSettling) {
      if(elapsed>=SWEEP_SETTLE_MS-sweepTime) {
        sweepKind=SweepKind::None; flowTime=0;
        // Boot returns smoothly to zero before a pending connection sweep;
        // connection sweeps settle at real SOC before restoring live display.
        startPendingConnectionSweep();
        if(sweeping()) sweepFirstFrame=false;
        showBoundary=true;
      } else sweepTime+=elapsed;
      if(sweepSettling && sweeping()) {
        // Smoothstep keeps the start/end of the transition gentle. It uses
        // bounded integer arithmetic and never writes to the BMS snapshot.
        const uint32_t t=sweepTime*1000u/SWEEP_SETTLE_MS;
        const uint32_t eased=t*t*(3000u-2u*t)/1000000u;
        const unsigned targetArc=(270u*sweepTarget+50u)/100u;
        sweepPercent=100u-((100u-sweepTarget)*eased+500u)/1000u;
        sweepSettleArc=270u-((270u-targetArc)*eased+500u)/1000u;
      }
    } else if(sweepAtPeak) {
      if(elapsed>=SWEEP_PEAK_MS-sweepTime) {
        // Keep this 100% frame; do not replace it with --/0/actual SOC at once.
        // Always target zero after boot, so data arriving during this return
        // cannot change its target halfway through and cause another jump.
        sweepTarget=sweepKind==SweepKind::Connected && current.valid?current.data.soc:0;
        sweepSettleArc=270; sweepSettling=true; sweepAtPeak=false; sweepTime=0;
      } else sweepTime+=elapsed;
    } else {
      if(elapsed>=SWEEP_ZERO_MS+SWEEP_RISE_MS-sweepTime) {
        sweepPercent=100; sweepAtPeak=true; sweepTime=0; showBoundary=true;
      } else {
        sweepTime+=elapsed;
        sweepPercent=sweepTime<=SWEEP_ZERO_MS?0:
          unsigned((sweepTime-SWEEP_ZERO_MS)*100u/SWEEP_RISE_MS);
      }
    }
  }
  if(flowing()) {
    flowTime=(flowTime+elapsed%FLOW_PERIOD_MS)%FLOW_PERIOD_MS;
  }
  // One clock for animation and display. Collect a bounded set of dirty areas
  // once per frame so queued updates cannot overflow LVGL's invalidation list.
  if(fullRefreshPending || showBoundary || uint32_t(now-lastFrame)>=UI_FRAME_MS) {
    lastFrame=now;
    bool full=fullRefreshPending;
    if(!picker && currentPage==0) {
      full=full || !haveHomeFrame;
      prepareHomeFrame(full);
    }
    if(full) lv_obj_invalidate(screen);
    fullRefreshPending=false;
    lv_refr_now(displayHandle);
  }
  // Invalidating an area resumes LVGL's auto refresh timer. Pause it here so
  // it cannot independently flush stale/new state between scheduled frames.
  lv_timer_pause(_lv_disp_get_refr_timer(displayHandle));
  lv_timer_handler();
}
