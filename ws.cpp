#include "ws.h"

WsClient* WsClient::_instance = nullptr;

static void wsEventHook(WStype_t type, uint8_t* payload, size_t length) {
  if (WsClient::_instance) WsClient::_instance->handleEvent(type, payload, length);
}

void WsClient::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      _conn = true;
      aliveSinceLastPong = false;
      if (onConnect) onConnect();
      break;
    case WStype_DISCONNECTED:
      _conn = false;
      if (onDisconnect) onDisconnect();
      break;
    case WStype_TEXT:
      if (onText) onText((const char*)payload, length);
      break;
    case WStype_BIN:
      if (onBin) onBin(payload, length);
      break;
    case WStype_PING:
    case WStype_PONG:
      aliveSinceLastPong = true;
      lastPongMillis = millis();
      break;
    default:
      break;
  }
}

void WsClient::begin(const String& host, int port, const String& path, bool secure) {
  _instance = this;
  _conn = false;
  uint16_t p = (uint16_t)(port == 0 ? (secure ? 443 : 80) : port);
  if (secure) {
    _ws.beginSSL(host.c_str(), p, path.c_str(), "");     // TLS, no cert pinning
  } else {
    _ws.begin(host.c_str(), p, path.c_str(), "ws");
  }
  _ws.onEvent(wsEventHook);
  _ws.setReconnectInterval(3000);
  _ws.enableHeartbeat(0, 0, 0);   // keepalive = our app-level 'ping'/'pong'
}

void WsClient::stop() {
  _ws.disconnect();
  _conn = false;
}

void WsClient::tick() {
  _ws.loop();
}

bool WsClient::connected() {
  return _conn;
}

bool WsClient::sendText(const char* s) {
  if (!_conn) return false;
  return _ws.sendTXT(s);
}

bool WsClient::sendBinary(const uint8_t* d, size_t n) {
  if (!_conn) return false;
  return _ws.sendBIN((const uint8_t*)d, (uint32_t)n);
}

// ---- URL parsing: ws://host:port/path | wss://host:port/path ----
bool ws_parse_url(const String& url, String& host, int& port, String& path, bool& wss) {
  wss = false;
  String rest;
  if (url.startsWith("wss://")) { wss = true; rest = url.substring(6); }
  else if (url.startsWith("ws://")) rest = url.substring(5);
  else return false;
  int slash = rest.indexOf('/');
  String auth;
  if (slash >= 0) { auth = rest.substring(0, slash); path = rest.substring(slash); }
  else { auth = rest; path = "/"; }
  if (!auth.length()) return false;
  int colon = auth.indexOf(':');
  if (colon >= 0) {
    host = auth.substring(0, colon);
    port = auth.substring(colon + 1).toInt();
  } else {
    host = auth;
    port = wss ? 443 : 80;
  }
  if (host.startsWith("[") ) { host = host.substring(1, host.length()-1); }
  return host.length() > 0;
}