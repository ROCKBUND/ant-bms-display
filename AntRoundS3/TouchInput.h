#pragma once
#include <stdint.h>

namespace ui {
enum class Sample { Contact, Release, Unknown };
enum class GestureKind { Tap, SwipeLeft, SwipeRight, LongPress };
struct Gesture {
  GestureKind kind=GestureKind::Tap;
  int x=0,y=0,endX=0,endY=0;
};

// Long press fires once while held; taps and swipes fire on a stable release.
// After long press/cancel, require a release before accepting another gesture.
class TouchInput {
 public:
  static constexpr uint32_t LONG_PRESS_MS=800, SWIPE_MAX_MS=1200;
  static constexpr int TAP_SLOP=18, SWIPE_MIN=45, SWIPE_Y_SLOP=40;
  // Discard a contact when another input changes the screen. Require a stable
  // release before accepting a new touch on the new page.
  void cancel() { down_=false; blocked_=true; releasing_=false; }
  bool update(Sample sample,int x,int y,uint32_t now,Gesture& event) {
    if(sample==Sample::Unknown) {
      releasing_=false;
      if(down_ && uint32_t(now-lastKnown_)>250) cancel();
      return false;
    }
    if((down_ || blocked_) && uint32_t(now-lastKnown_)>250) cancel();
    lastKnown_=now;
    if(sample==Sample::Contact) {
      releasing_=false;
      if(blocked_) return false;
      if(!down_) {
        down_=true; began_=now; startX_=x; startY_=y;
        moved_=false; maxDy_=0;
      }
      endX_=x; endY_=y;
      int dx=x-startX_,dy=y-startY_;
      if(dx*dx+dy*dy>TAP_SLOP*TAP_SLOP) moved_=true;
      const int absDy=dy<0?-dy:dy;
      if(absDy>maxDy_) maxDy_=absDy;
      if(!moved_ && uint32_t(now-began_)>=LONG_PRESS_MS) {
        event={GestureKind::LongPress,startX_,startY_,endX_,endY_};
        cancel();
        return true;
      }
      return false;
    }
    if(!down_ && !blocked_) return false;
    if(!releasing_) { releasing_=true; releasedAt_=now; return false; }
    if(uint32_t(now-releasedAt_)<50) return false;
    const uint32_t duration=releasedAt_-began_;
    const int dx=endX_-startX_,dy=endY_-startY_;
    const int absDx=dx<0?-dx:dx,absDy=dy<0?-dy:dy;
    bool fire=false;
    GestureKind kind=GestureKind::Tap;
    if(down_ && !blocked_ && duration>=30) {
      if(!moved_ && duration<LONG_PRESS_MS) fire=true;
      else if(duration<=SWIPE_MAX_MS && absDx>=SWIPE_MIN &&
              absDx*2>=absDy*3 && maxDy_<=SWIPE_Y_SLOP) {
        kind=dx<0?GestureKind::SwipeLeft:GestureKind::SwipeRight;
        fire=true;
      }
    }
    event={kind,startX_,startY_,endX_,endY_};
    down_=false; blocked_=false; releasing_=false;
    return fire;
  }
 private:
  bool down_=false,blocked_=false,releasing_=false,moved_=false;
  int startX_=0,startY_=0,endX_=0,endY_=0,maxDy_=0;
  uint32_t began_=0,lastKnown_=0,releasedAt_=0;
};

enum class Action { None, Bluetooth, Page, PagePrevious, Back, Scan, Previous, Next, Connect };
struct Button {
  int x,y,w,h; const char* label; Action action;
  bool contains(int px,int py) const { return px>=x && px<x+w && py>=y && py<y+h; }
};
static constexpr Button BACK{58,192,58,28,"BACK",Action::Back};
static constexpr Button SCAN{124,192,58,28,"SCAN",Action::Scan};
static constexpr Button PREV{40,132,72,26,"PREV",Action::Previous};
static constexpr Button NEXT{128,132,72,26,"NEXT",Action::Next};
static constexpr Button CONNECT{66,162,108,26,"CONNECT",Action::Connect};
inline Action hit(bool choosing,int x,int y) {
  if(!choosing) return Action::None;
  const Button* buttons[]={&BACK,&SCAN,&PREV,&NEXT,&CONNECT};
  for(const auto* b:buttons) if(b->contains(x,y)) return b->action;
  return Action::None;
}
inline Action gestureAction(bool choosing,const Gesture& event) {
  if(!choosing) {
    switch(event.kind) {
      case GestureKind::SwipeLeft: return Action::Page;
      case GestureKind::SwipeRight: return Action::PagePrevious;
      case GestureKind::LongPress: return Action::Bluetooth;
      default: return Action::None;
    }
  }
  if(event.kind!=GestureKind::Tap) return Action::None;
  const Action start=hit(true,event.x,event.y);
  return start==hit(true,event.endX,event.endY)?start:Action::None;
}
}
