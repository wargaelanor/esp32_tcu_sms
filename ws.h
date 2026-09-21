#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>

// WebSocket transport backed by the battle-tested WebSocketsClient library
// (auto-answers server PINGs, reassembles fragmented frames, reconnects itself).
// Caller keeps application-level liveness with text 'ping' -> 'pong'.
class WsClient {
 public:
  void begin(const String& host, int port, const String& path, bool secure = false);
  void stop();
  void tick();                 // call every loop(): drives connect/receive/reconnect
  bool connected();

  bool sendText(const char* s);
  bool sendBinary(const uint8_t* d, size_t n);

  // callbacks
  void (*onText)(const char* txt, size_t len)  = nullptr;
  void (*onBin)(const uint8_t* d, size_t len)  = nullptr;
  void (*onConnect)(void)    = nullptr;
  void (*onDisconnect)(void) = nullptr;

  bool aliveSinceLastPong = false;
  uint32_t lastPongMillis = 0;

  // exposed for the static event dispatcher
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  static WsClient* _instance;

 private:
  WebSocketsClient _ws;
  bool _conn = false;          // library-reported connection state
};

bool ws_parse_url(const String& url, String& host, int& port, String& path, bool& wss);