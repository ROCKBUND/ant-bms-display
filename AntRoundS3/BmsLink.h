#pragma once
#include <Arduino.h>
#include "AntProtocol.h"

struct Peer {
  char address[18]={}, name[32]={};
  uint8_t type=0;
  int rssi=0;
};
enum class LinkState { Starting, Scanning, Choose, Connecting, Waiting, Live, Retry };
struct Snapshot {
  ant::Data data;
  Peer peers[6], connectedPeer;
  unsigned peerCount=0;
  LinkState state=LinkState::Starting;
  bool connected=false, valid=false, pairing=false, saved=false;
  uint32_t updated=0, frames=0, rejected=0, drops=0;
  char message[48]="Starting BLE";
};
bool startBms();
Snapshot bmsSnapshot();
bool requestScan();
bool cancelPairing();
bool choosePeer(unsigned index);
void setRawLogging(bool enabled);
