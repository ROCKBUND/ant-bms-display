#include "BmsLink.h"
#include "Config.h"
#include "PairingSession.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>

namespace {
SemaphoreHandle_t lockHandle;
QueueHandle_t rxQueue, commandQueue;
Snapshot shared;
struct Packet { uint32_t at; uint16_t size; uint8_t bytes[265]; };
enum class CommandKind { Scan, Select, Resume };
struct Command { CommandKind kind; Peer peer; };
std::atomic<uint32_t> dropped{0};
std::atomic<bool> disconnected{false};
std::atomic<bool> rawLogging{false};
ant::Parser parser;
Preferences prefs;
Peer target;
bool persisted=false;
uint32_t lastValid=0;

void state(LinkState s,const char* message,bool connected=false) {
  xSemaphoreTake(lockHandle,portMAX_DELAY);
  shared.state=s; shared.connected=connected;
  if(!connected) shared.valid=false;
  snprintf(shared.message,sizeof(shared.message),"%s",message);
  xSemaphoreGive(lockHandle);
  Serial.printf("BLE: %s\n",message);
}
class Callbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    disconnected.store(true);
    // Invalidate immediately; callbacks never perform reconnects or delays.
    xSemaphoreTake(lockHandle,portMAX_DELAY);
    shared.valid=false; shared.connected=false;
    xSemaphoreGive(lockHandle);
    Serial.printf("BLE disconnected: %d\n",reason);
  }
} callbacks;
void notify(NimBLERemoteCharacteristic*,uint8_t* bytes,size_t n,bool) {
  if(n>265) { dropped.fetch_add(1); return; }
  Packet packet{}; packet.at=millis(); packet.size=n;
  memcpy(packet.bytes,bytes,n);
  if(xQueueSend(rxQueue,&packet,0)!=pdTRUE) dropped.fetch_add(1);
}
void decoded(const ant::Data& data,void*) {
  if(disconnected.load()) return;
  lastValid=millis();
  if(!persisted) {
    // Save a single record only after a CRC-valid status frame, not just discovery.
    persisted=prefs.putBytes("peer",&target,sizeof(target))==sizeof(target);
  }
  xSemaphoreTake(lockHandle,portMAX_DELAY);
  if(!shared.connected || disconnected.load()) { xSemaphoreGive(lockHandle); return; }
  shared.data=data; shared.updated=lastValid; shared.valid=true;
  shared.connected=true; shared.state=LinkState::Live; shared.saved=persisted;
  shared.frames=parser.accepted; shared.rejected=parser.rejected; shared.drops=dropped.load();
  snprintf(shared.message,sizeof(shared.message),"%s",persisted?"Live":"Live / save failed");
  xSemaphoreGive(lockHandle);
  Serial.printf("DATA soc=%u V=%.2f A=%.1f Ah=%.3f/%.3f cells=%u MOS=%d heap=%u\n",
    data.soc,data.volts,data.amps,data.remainingAh,data.capacityAh,data.cells,data.mos,ESP.getFreeHeap());
}
bool scanPeers(NimBLEScan* scan,bool autoPick) {
  state(LinkState::Scanning,"Scanning ANT devices");
  xSemaphoreTake(lockHandle,portMAX_DELAY); shared.peerCount=0; xSemaphoreGive(lockHandle);
  auto results=scan->getResults(SCAN_MS,false);
  Peer peers[6]{}; unsigned count=0, matches=0; Peer exact;
  for(int i=0;i<results.getCount();i++) {
    auto dev=results.getDevice(i);
    String name=dev->getName().c_str(); name.toUpperCase();
    String address=dev->getAddress().toString().c_str(); address.toUpperCase();
    const bool addressMatch=TARGET_ADDRESS[0] && address==TARGET_ADDRESS;
    if(!addressMatch && !name.startsWith("ANT") && !dev->isAdvertisingService(NimBLEUUID(uint16_t(0xffe0)))) continue;
    Peer p; snprintf(p.name,sizeof(p.name),"%s",dev->getName().c_str());
    snprintf(p.address,sizeof(p.address),"%s",dev->getAddress().toString().c_str());
    p.type=dev->getAddress().getType(); p.rssi=dev->getRSSI();
    if(addressMatch || (!TARGET_ADDRESS[0] && name==TARGET_NAME)) { matches++; exact=p; }
    if(count<6) peers[count++]=p;
  }
  scan->clearResults();
  xSemaphoreTake(lockHandle,portMAX_DELAY);
  shared.peerCount=count; memcpy(shared.peers,peers,sizeof(peers));
  xSemaphoreGive(lockHandle);
  for(unsigned i=0;i<count;i++) Serial.printf("PEER %u %s %s (%d dBm)\n",i+1,peers[i].address,peers[i].name,peers[i].rssi);
  if(autoPick && matches==1) { target=exact; persisted=false; return true; }
  state(LinkState::Choose,count?"Select your BMS":"No ANT found. Rescan");
  return false;
}
void worker(void*) {
  prefs.begin("ant-round",false);
  if(prefs.getBytesLength("peer")==sizeof(target)) {
    prefs.getBytes("peer",&target,sizeof(target));
    target.address[17]=0; target.name[31]=0;
    persisted=strlen(target.address)==17 && target.type<=3;
    if(!persisted) target={};
  }
  NimBLEDevice::init("AntRound-S3");
  NimBLEDevice::setMTU(247);
  auto scan=NimBLEDevice::getScan();
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(80); scan->setMaxResults(40);
  auto client=NimBLEDevice::createClient();
  if(!client) { state(LinkState::Retry,"BLE allocation failed"); vTaskDelete(nullptr); return; }
  client->setClientCallbacks(&callbacks,false);
  client->setConnectTimeout(5000);
  client->setConnectionParams(24,40,0,400);
  NimBLERemoteCharacteristic* chr=nullptr;
  PairingSession<Peer> pairing;
  bool scanNeeded=!persisted, live=false;
  uint32_t nextTry=millis(), connectedAt=0,lastPoll=0,seenDrops=0;
  unsigned backoff=1000;
  for(;;) {
    Command command;
    if(xQueueReceive(commandQueue,&command,0)==pdTRUE) {
      if(command.kind==CommandKind::Resume && !pairing.active()) {
        // BACK may have replaced a queued SCAN before it was executed.
        state(live?LinkState::Waiting:LinkState::Retry,live?"Resuming data":"Resuming connection",live);
        xSemaphoreTake(lockHandle,portMAX_DELAY); shared.pairing=false; xSemaphoreGive(lockHandle);
        continue;
      }
      if(client->isConnected()) client->disconnect();
      live=false; chr=nullptr; parser.reset(); xQueueReset(rxQueue);
      if(command.kind==CommandKind::Scan) {
        pairing.begin(target,persisted); scanNeeded=true; target={}; persisted=false;
      } else if(command.kind==CommandKind::Resume) {
        pairing.cancel(target,persisted); scanNeeded=!target.address[0];
      } else {
        target=command.peer; persisted=false; pairing.commit(); scanNeeded=false;
      }
      nextTry=millis(); backoff=1000;
      xSemaphoreTake(lockHandle,portMAX_DELAY); shared.pairing=pairing.active(); shared.saved=persisted; shared.peerCount=0; xSemaphoreGive(lockHandle);
      state(LinkState::Retry,command.kind==CommandKind::Scan?"Starting scan":command.kind==CommandKind::Resume?"Resuming previous BMS":"Selected BMS");
    }
    if(disconnected.exchange(false) || (live && !client->isConnected())) {
      live=false; chr=nullptr; parser.reset(); xQueueReset(rxQueue);
      state(LinkState::Retry,"Disconnected. Retrying");
      nextTry=millis()+backoff;
    }
    const uint32_t now=millis();
    if(!live && int32_t(now-nextTry)>=0) {
      if(scanNeeded) {
        bool found=scanPeers(scan,!pairing.active());
        scanNeeded=!found && !pairing.active();
        nextTry=millis()+4000;
        // Process BACK/new selection before attempting a connection after a scan.
        if(uxQueueMessagesWaiting(commandQueue)) continue;
      }
      if(target.address[0]) {
        state(LinkState::Connecting,"Connecting to BMS");
        xSemaphoreTake(lockHandle,portMAX_DELAY); shared.connectedPeer=target; xSemaphoreGive(lockHandle);
        disconnected.store(false); parser.reset(); xQueueReset(rxQueue);
        bool ok=client->connect(NimBLEAddress(std::string(target.address),target.type));
        if(ok) {
          auto service=client->getService(NimBLEUUID(uint16_t(0xffe0)));
          chr=service?service->getCharacteristic(NimBLEUUID(uint16_t(0xffe1))):nullptr;
          ok=chr && chr->canNotify() && (chr->canWrite() || chr->canWriteNoResponse()) && chr->subscribe(true,notify);
        }
        if(ok && client->isConnected()) {
          live=true; connectedAt=millis(); lastValid=0; lastPoll=connectedAt-POLL_MS; backoff=1000;
          state(LinkState::Waiting,"Connected. Waiting data",true);
        } else {
          if(client->isConnected()) client->disconnect();
          chr=nullptr; state(LinkState::Retry,"Connect / FFE1 failed");
          nextTry=millis()+backoff; backoff=backoff<16000?backoff*2:16000;
        }
      }
    }
    if(live && chr) {
      Packet packet;
      const auto drops=dropped.load();
      if(drops!=seenDrops) { parser.reset(); xQueueReset(rxQueue); seenDrops=drops; }
      while(xQueueReceive(rxQueue,&packet,0)==pdTRUE) {
        if(rawLogging.load()) {
          Serial.print("RX ");
          for(unsigned i=0;i<packet.size;i++) Serial.printf("%02X ",packet.bytes[i]);
          Serial.println();
        }
        if(uint32_t(millis()-packet.at)<1000 && client->isConnected())
          parser.feed(packet.bytes,packet.size,packet.at,decoded,nullptr);
      }
      xSemaphoreTake(lockHandle,portMAX_DELAY);
      shared.rejected=parser.rejected; shared.drops=dropped.load();
      xSemaphoreGive(lockHandle);
      if(uint32_t(millis()-lastPoll)>=POLL_MS) {
        lastPoll=millis();
        bool sent=chr->writeValue(ant::STATUS_REQUEST,sizeof(ant::STATUS_REQUEST),!chr->canWriteNoResponse());
        if(!sent) Serial.println("Status request failed");
      }
      if(uint32_t(millis()-(lastValid?lastValid:connectedAt))>RECONNECT_MS) {
        client->disconnect(); live=false; chr=nullptr; parser.reset(); xQueueReset(rxQueue);
        state(LinkState::Retry,"No valid data. Retrying"); nextTry=millis()+2000;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}
bool startBms() {
  lockHandle=xSemaphoreCreateMutex(); rxQueue=xQueueCreate(12,sizeof(Packet)); commandQueue=xQueueCreate(1,sizeof(Command));
  if(!lockHandle || !rxQueue || !commandQueue) return false;
  return xTaskCreate(worker,"ant-ble",6144,nullptr,1,nullptr)==pdPASS;
}
Snapshot bmsSnapshot() {
  xSemaphoreTake(lockHandle,portMAX_DELAY); Snapshot s=shared; xSemaphoreGive(lockHandle);
  if(!s.connected || uint32_t(millis()-s.updated)>STALE_MS) s.valid=false;
  return s;
}
static bool enqueue(const Command& command) {
  xSemaphoreTake(lockHandle,portMAX_DELAY);
  // Latest intent wins: BACK cannot get stuck behind repeated SCAN requests.
  bool sent=xQueueOverwrite(commandQueue,&command)==pdTRUE;
  if(sent) {
    shared.valid=false; shared.connected=false;
    shared.state=command.kind==CommandKind::Scan?LinkState::Scanning:LinkState::Connecting;
    snprintf(shared.message,sizeof(shared.message),"%s",command.kind==CommandKind::Scan?"Scan requested":command.kind==CommandKind::Resume?"Returning to BMS":"Selection requested");
  }
  xSemaphoreGive(lockHandle);
  return sent;
}
bool requestScan() { Command c{}; c.kind=CommandKind::Scan; return enqueue(c); }
bool cancelPairing() { Command c{}; c.kind=CommandKind::Resume; return enqueue(c); }
void setRawLogging(bool enabled) { rawLogging.store(enabled); }
bool choosePeer(unsigned index) {
  const auto s=bmsSnapshot(); if(index>=s.peerCount) return false;
  Command c{}; c.kind=CommandKind::Select; c.peer=s.peers[index]; return enqueue(c);
}
