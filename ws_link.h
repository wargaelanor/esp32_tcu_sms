#pragma once
#include <Arduino.h>
#include "transport.h"

// Minimal WebSocket (RFC6455) client over an arbitrary Link stream.
// Handles: HTTP upgrade, masking, fragmentation, server PING->PONG,
// text/binary frames. Reads are synchronous with timeouts (Link::read).
class WsLinkClient {
 public:
  void begin(Link* link, const String& host, const String& path);
  void stop();
  void tick();                       // drive: connect + read frames
  bool connected();

  bool sendText(const char* s);
  bool sendBinary(const uint8_t* d, size_t n);
  bool sendPing();
  bool sendClose();

  void (*onText)(const char* txt, size_t len)  = nullptr;
  void (*onBin)(const uint8_t* d, size_t len)  = nullptr;
  void (*onConnect)(void)    = nullptr;
  void (*onDisconnect)(void) = nullptr;

  bool aliveSinceLastPong = false;
  uint32_t lastPongMillis = 0;

 private:
  Link* _link = nullptr;
  String _host, _path;
  bool _open = false;
  uint32_t _nextConnect = 0;

  uint8_t  _rxbuf[2048];
  size_t   _rxlen = 0;               // bytes buffered in _rxbuf
  size_t   _rxpos = 0;

  bool     _fragActive = false;
  uint8_t  _fragOp = 0;
  size_t   _fragLen = 0;

  bool ensureBytes(size_t need, uint32_t timeoutMs);
  int  readSome(uint8_t* out, size_t max, uint32_t timeoutMs);
  bool readLine(String& line, uint32_t timeoutMs);
  bool doHandshake();
  int  processFrames();              // 1=ok,0=idle,-1=fatal->disconnect
  bool sendFrame(uint8_t opcode, const uint8_t* d, size_t n);
  void closeLink(bool notify);
};