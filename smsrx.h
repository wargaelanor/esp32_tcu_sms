#pragma once
#include <Arduino.h>

// Receives SMS through the SIM800 (AT+CMGR in PDU mode), reassembles
// concatenated (multipart) messages and hands the payload to a callback.
//
// The FSO answers the TCU commands with binary SMS. A 500+ byte answer is
// split into several parts that are concatenated with a UDH (IEI 0x00 / 0x08),
// so the parts have to be joined before the packet can be forwarded to the
// OpenCARWINGS server.
class SmsRx {
 public:
  typedef void (*MsgCallback)(const uint8_t* data, size_t len, const String& sender);

  void begin();
  void tick();
  void setCallback(MsgCallback cb) { _cb = cb; }
  void setPollInterval(uint32_t ms) { _pollMs = ms; }

  uint32_t parts() const      { return _rxParts; }
  uint32_t messages() const   { return _rxMsgs; }
  uint32_t relayed() const    { return _txMsgs; }
  String   lastInfo() const   { return _last; }
  String   lastSender() const { return _sender; }

 private:
  bool readSlot(uint8_t index);
  bool handlePdu(const uint8_t* pdu, size_t len);
  void resetRx();
  void emit(const uint8_t* data, size_t len, const String& sender);

  MsgCallback _cb = nullptr;
  uint32_t _pollMs = 6000;
  uint32_t _nextPoll = 0;
  uint8_t  _nextSlot = 0;
  uint32_t _rxParts = 0;
  uint32_t _rxMsgs = 0;
  uint32_t _txMsgs = 0;
  String   _last;
  String   _sender;

  static const uint8_t  MAX_PARTS = 10;
  static const uint16_t PART_MAX = 160;
  static const uint16_t MSG_MAX  = 1600;

  uint8_t  _partIdx[MAX_PARTS];
  uint16_t _partLen[MAX_PARTS];
  uint8_t  _partData[MAX_PARTS][PART_MAX];
  uint8_t  _partCount = 0;
  uint16_t _partRef = 0;
  uint8_t  _partTotal = 0;
  uint8_t  _partGot = 0;
  bool     _partActive = false;
  uint8_t  _msg[MSG_MAX];
  uint16_t _msgLen = 0;
  String   _msgSender;
};

extern SmsRx g_smsRx;
