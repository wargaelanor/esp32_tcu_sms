#include "transport.h"
#include "sim800.h"

// ---------------- WifiLink ----------------
void WifiLink::configure(const String& host, int port, uint32_t timeoutMs) {
  _host = host; _port = port; _timeout = timeoutMs;
}

bool WifiLink::open() {
  if (_c.connected()) return true;
  if (!_host.length()) return false;
  _c.stop();
  return _c.connect(_host.c_str(), _port, _timeout);
}

void WifiLink::close() { _c.stop(); }

bool WifiLink::isConnected() { return _c.connected(); }

size_t WifiLink::write(const uint8_t* d, size_t n) { return _c.write(d, n); }

int WifiLink::read(uint8_t* out, size_t max, uint32_t timeoutMs) {
  if (!_c.connected()) return -1;
  uint32_t t0 = millis();
  while (!_c.available()) {
    if (!_c.connected()) return -1;
    if (millis() - t0 > timeoutMs) return 0;
    delay(2);
  }
  return (int)_c.read(out, max);
}

// ---------------- GprsLink ----------------
void GprsLink::configure(const String& host, int port, const String& apn, const String& user, const String& pass) {
  _host = host; _port = port; _apn = apn; _apnUser = user; _apnPass = pass;
}

bool GprsLink::open() {
  if (g_sim.passthroughActive) return true;   // already in transparent mode
  if (!g_sim.tcpOpenPassthrough(_host, _port, _apn, _apnUser, _apnPass)) {
    Serial.printf("[gprsLink] tcpOpenPassthrough %s:%d FAIL\n", _host.c_str(), _port);
    g_sim.closeTcp();
    return false;
  }
  Serial.printf("[gprsLink] tcp passthrough %s:%d OK\n", _host.c_str(), _port);
  return true;
}

void GprsLink::close() {
  g_sim.closeTcp();
}

bool GprsLink::isConnected() { return g_sim.passthroughActive; }

size_t GprsLink::write(const uint8_t* d, size_t n) {
  if (!g_sim.passthroughActive) return 0;
  return g_sim.tcpWrite(d, n);
}

int GprsLink::read(uint8_t* out, size_t max, uint32_t timeoutMs) {
  return g_sim.tcpRead(out, max, timeoutMs);
}