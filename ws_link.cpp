#include "ws_link.h"

// ---- Helpers ----
static void wsMask(uint8_t* data, size_t len, const uint8_t* key) {
  for (size_t i = 0; i < len; i++) data[i] ^= key[i & 3];
}

void WsLinkClient::begin(Link* link, const String& host, const String& path) {
  _link = link;
  _host = host;
  _path = path;
  _open = false;
  _rxlen = _rxpos = 0;
  _nextConnect = 0;
}

void WsLinkClient::stop() {
  closeLink(true);
  _nextConnect = millis() + 0xFFFFFF;   // не реконнектиться до begin()
}

bool WsLinkClient::connected() { return _open; }

int WsLinkClient::readSome(uint8_t* out, size_t max, uint32_t timeoutMs) {
  while (_rxpos < _rxlen) {
    size_t n = _rxlen - _rxpos;
    if (n > max) n = max;
    memcpy(out, _rxbuf + _rxpos, n);
    _rxpos += n;
    return (int)n;
  }
  int n = _link->read(out, max, timeoutMs);
  if (n > 0) return n;
  if (n == 0) return 0;
  return -1;
}

// Гарантируем presence байт и подкачиваем с линка, компактно сдвигая остаток.
bool WsLinkClient::ensureBytes(size_t need, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  for (;;) {
    if (_rxpos == _rxlen) { _rxpos = _rxlen = 0; }
    else if (_rxpos > 0 && _rxlen - _rxpos >= need) return true;
    else if (_rxlen - _rxpos >= need) return true;

    if (_rxpos > 0) {                        // сдвигаем остаток в начало
      memmove(_rxbuf, _rxbuf + _rxpos, _rxlen - _rxpos);
      _rxlen -= _rxpos; _rxpos = 0;
    }
    if (_rxlen >= need) return true;
    if (_rxlen >= sizeof(_rxbuf)) return false;

    int n = _link->read(_rxbuf + _rxlen, sizeof(_rxbuf) - _rxlen, 200);
    if (n > 0) { _rxlen += (size_t)n; continue; }
    if (n < 0) return false;
    if (millis() - t0 >= timeoutMs) return false;
  }
}

bool WsLinkClient::readLine(String& line, uint32_t timeoutMs) {
  line = "";
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (!ensureBytes(1, 200)) continue;      // ждать до общего timeoutMs, не сдаваться через 200мс
    char c = (char)_rxbuf[_rxpos++];
    if (c == '\n') return true;
    if (c != '\r') line += c;
  }
  return false;
}

static void wsBase64(const uint8_t* in, size_t n, String& out) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  while (i + 2 < n) {
    uint32_t v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    i += 3;
  }
  if (i + 1 == n) {
    uint32_t v = (in[i] << 16);
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == n) {
    uint32_t v = (in[i] << 16) | (in[i+1] << 8);
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
  }
}

bool WsLinkClient::doHandshake() {
  if (!_link->open()) return false;

  uint8_t keyraw[16];
  for (size_t i = 0; i < sizeof(keyraw); i++) keyraw[i] = (uint8_t)esp_random();
  String keyB64;
  wsBase64(keyraw, 16, keyB64);

  String req = "GET " + _path + " HTTP/1.1\r\n";
  req += "Host: " + _host + "\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: " + keyB64 + "\r\n";
  req += "Sec-WebSocket-Version: 13\r\n\r\n";
  _link->write((const uint8_t*)req.c_str(), req.length());

  uint32_t t0 = millis();
  bool got101 = false;
  String line;
  while (millis() - t0 < 10000) {
    if (!readLine(line, 6000)) break;
    if (line.startsWith("HTTP/")) {
      got101 = line.indexOf(" 101 ") >= 0 || line.endsWith(" 101");
      if (!got101) return false;                 // 200/404/etc -> abort
    } else if (line.length() == 0) {
      return got101;
    }
  }
  return false;
}

bool WsLinkClient::sendFrame(uint8_t opcode, const uint8_t* d, size_t n) {
  uint8_t hdr[14];
  size_t h = 0;
  hdr[h++] = 0x80 | opcode;                       // FIN + opcode
  if (n < 126) {
    hdr[h++] = 0x80 | (uint8_t)n;                 // masked
  } else if (n <= 0xFFFF) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (uint8_t)(n >> 8); hdr[h++] = (uint8_t)n;
  } else {
    hdr[h++] = 0x80 | 127;
    uint64_t l = n;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)(l >> (i * 8));
  }
  uint8_t key[4];
  uint32_t r = esp_random();
  key[0] = r & 0xFF; key[1] = (r >> 8) & 0xFF; key[2] = (r >> 16) & 0xFF; key[3] = (r >> 24) & 0xFF;
  memcpy(hdr + h, key, 4); h += 4;

  if (n == 0) return _link->write(hdr, h) == h;

  static uint8_t tmp[9100];
  size_t first = (h + n <= sizeof(tmp)) ? n : (sizeof(tmp) - h);   // весь кадр одним TLS-record
  memcpy(tmp, hdr, h);
  memcpy(tmp + h, d, first);
  wsMask(tmp + h, first, key);
  if (_link->write(tmp, h + first) != h + first) return false;

  size_t off = first;
  while (off < n) {
    size_t chunk = n - off; if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
    memcpy(tmp, d + off, chunk);
    wsMask(tmp, chunk, key);
    if (_link->write(tmp, chunk) != chunk) return false;
    off += chunk;
  }
  return true;
}

bool WsLinkClient::sendText(const char* s) {
  if (!_open) return false;
  return sendFrame(0x1, (const uint8_t*)s, strlen(s));
}

bool WsLinkClient::sendPing() {
  if (!_open) return false;
  const char p[] = "hb";
  return sendFrame(0x9, (const uint8_t*)p, 2);
}

bool WsLinkClient::sendClose() {
  if (!_open) return false;
  const uint8_t p[2] = { 0x03, 0xE8 };   // close code 1000
  return sendFrame(0x8, p, 2);
}

bool WsLinkClient::sendBinary(const uint8_t* d, size_t n) {
  if (!_open) return false;
  return sendFrame(0x2, d, n);
}

int WsLinkClient::processFrames() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) {
    if (!ensureBytes(2, 200)) {
      return _link->isConnected() ? 0 : -1;   // idle != closed
    }

    uint8_t b0 = _rxbuf[_rxpos], b1 = _rxbuf[_rxpos + 1];
    uint8_t opcode = b0 & 0x0F;
    bool masked = b1 & 0x80;
    size_t len = b1 & 0x7F;
    size_t need = 2;
    if (len == 126) need += 2;
    else if (len == 127) need += 8;
    if (masked) need += 4;

    if (!ensureBytes(need, 200)) return _link->isConnected() ? 0 : -1;

    size_t off = _rxpos + 2;
    if (len == 126) {
      len = ((size_t)_rxbuf[off] << 8) | _rxbuf[off + 1]; off += 2;
    } else if (len == 127) {
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | _rxbuf[off + i];
      off += 8;
    }
    uint8_t mask[4];
    if (masked) { memcpy(mask, _rxbuf + off, 4); off += 4; }

    if (len > 9000) return -1;
    if (!ensureBytes(off + len, 500)) return _link->isConnected() ? 0 : -1;

    static uint8_t payload[9100];
    memcpy(payload, _rxbuf + off, len);
    if (masked) wsMask(payload, len, mask);
    _rxpos = off + len;

    static uint8_t frag[9100];

    switch (opcode) {
      case 0x0:   // continuation: доклеиваем к нефинальному кадру
        if (_fragActive && _fragLen + len <= sizeof(frag)) {
          memcpy(frag + _fragLen, payload, len);
          _fragLen += len;
          if (b0 & 0x80) {                         // FIN -> отдать как целое
            if (_fragOp == 0x2 && onBin) onBin(frag, _fragLen);
            else if (_fragOp == 0x1 && onText) onText((const char*)frag, _fragLen);
            _fragActive = false;
          }
        }
        return 1;
      case 0x1:   // text
        if (b0 & 0x80) { if (onText) onText((const char*)payload, len); }
        else { _fragActive = true; _fragOp = 0x1; _fragLen = 0;
               memcpy(frag, payload, len); _fragLen = len; }
        return 1;
      case 0x2:   // binary
        if (b0 & 0x80) { if (onBin) onBin(payload, len); }
        else { _fragActive = true; _fragOp = 0x2; _fragLen = 0;
               memcpy(frag, payload, len); _fragLen = len; }
        return 1;
      case 0x8:   // close
        closeLink(true);
        return -1;
      case 0x9: { // ping -> pong
        sendFrame(0xA, payload, len);
        return 1;
      }
      case 0xA:   // pong
        aliveSinceLastPong = true;
        lastPongMillis = millis();
        return 1;
      default:
        return 1;
    }
  }
  return 0;
}

void WsLinkClient::closeLink(bool notify) {
  _open = false;
  _link->close();
  _rxlen = _rxpos = 0;
  if (notify && onDisconnect) onDisconnect();
}

void WsLinkClient::tick() {
  if (!_link) return;
  if (!_open) {
    if (millis() < _nextConnect) return;
    if (doHandshake()) {
      _open = true;
      if (onConnect) onConnect();
      _nextConnect = 0;
    } else {
      Serial.println("[ws] handshake fail -> retry");
      _link->close();
      _rxlen = _rxpos = 0;
      _nextConnect = millis() + 3000;
    }
    return;
  }
  int r = processFrames();
  if (r < 0) {
    closeLink(true);
    _nextConnect = millis() + 3000;
  }
}