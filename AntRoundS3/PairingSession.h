#pragma once

// Retain the pre-picker target across repeated scans. Cancelling an opening
// request before the worker processed it must leave the current link alone.
template<class PeerType> class PairingSession {
 public:
  bool active() const { return active_; }
  void begin(const PeerType& peer,bool persisted) {
    if(!active_) { previous_=peer; persisted_=persisted; }
    active_=true;
  }
  bool cancel(PeerType& peer,bool& persisted) {
    if(!active_) return false;
    peer=previous_; persisted=persisted_; active_=false; return true;
  }
  void commit() { active_=false; }
 private:
  PeerType previous_{};
  bool persisted_=false,active_=false;
};
