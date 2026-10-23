#include "smsrx.h"
#include "sim800.h"
#include "webui.h"
#include "step.h"

SmsRx g_smsRx;

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static size_t hexToBytes(const char* h, size_t hexLen, uint8_t* out, size_t maxOut) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < hexLen && n < maxOut; i += 2) {
    int a = hexVal(h[i]);
    int b = hexVal(h[i + 1]);
    if (a < 0 || b < 0) break;
    out[n++] = (uint8_t)((a << 4) | b);
  }
  return n;
}

static String decodeSemi(const uint8_t* b, size_t bytes, uint8_t digits) {
  String s;
  for (uint8_t i = 0; i < digits && (i / 2) < bytes; i++) {
    uint8_t v = b[i / 2];
    uint8_t d = (i % 2 == 0) ? (uint8_t)(v & 0x0F) : (uint8_t)((v >> 4) & 0x0F);
    if (d == 0x0F) continue;
    s += (char)('0' + d);
  }
  return s;
}

static size_t unpack7(const uint8_t* in, size_t inLen, size_t nSeptets, uint8_t* out) {
  size_t o = 0;
  uint8_t l = 0;
  for (size_t i = 0; i < inLen && o < nSeptets; i++) {
    uint8_t c = in[i];
    if (i % 7 == 0) {
      out[o++] = (uint8_t)(c & 0x7F);
    } else if (i % 7 == 6) {
      out[o++] = (uint8_t)((l & 0x01) | ((c & 0x7F) << 1));
    } else {
      out[o++] = (uint8_t)(((l & 0x01) << 6) | ((c & 0xFE) >> 1));
      l = c;
    }
  }
  return o;
}

void SmsRx::begin() {
  g_sim.sendCmd("AT+CMGF=0", true);
  g_sim.await("OK", 1500);
  g_sim.sendCmd("AT+CNMI=2,1", true);
  g_sim.await("OK", 1500);
  _nextPoll = millis() + 3000;
}

void SmsRx::resetRx() {
  _partCount = 0;
  _partTotal = 0;
  _partGot = 0;
  _partActive = false;
  _partRef = 0;
  _msgLen = 0;
  _msgSender = "";
}

void SmsRx::emit(const uint8_t* data, size_t len, const String& sender) {
  _rxMsgs++;
  _last = "rx " + String((unsigned)len) + "B";
  _sender = sender;
  Serial.printf("[rx] message %u bytes from '%s'\n", (unsigned)len, sender.c_str());
  if (_cb) _cb(data, len, sender);
}

// Header of an incoming SMS (SMS-DELIVER). The PDU of the FSO is not always
// strictly GSM 03.40 (it may miss the TP-SCTS), so both variants are tried and
// the one with a plausible origin address wins.
struct SmsHeader {
  size_t   udStart = 0;
  uint8_t  dcs = 0;
  uint8_t  udl = 0;
  bool     udhi = false;
  bool     concat = false;
  uint16_t ref = 0;
  uint8_t  total = 0;
  uint8_t  idx = 0;
  String   sender;
};

static bool validScts(const uint8_t* p) {
  for (uint8_t i = 0; i < 6; i++) {
    if ((p[i] & 0x0F) > 9 || ((p[i] >> 4) & 0x0F) > 9) return false;
  }
  return (p[6] & 0x07) <= 7 && ((p[6] >> 4) & 0x0F) <= 9;
}

static bool parseSmsHeader(const uint8_t* p, size_t len, size_t start, bool withScts, SmsHeader& h) {
  if (start >= len) return false;
  uint8_t b0 = p[start];
  if ((b0 & 0x03) != 0x00) return false;       // only SMS-DELIVER
  h.udhi = (b0 & 0x40) != 0;
  size_t i = start + 1;
  if (i + 3 > len) return false;
  uint8_t oaDigits = p[i++];                   // TP-OA length
  if (oaDigits == 0 || oaDigits > 40) return false;
  uint8_t toa = p[i++];                        // TP-OA type
  size_t oaBytes = (oaDigits + 1) / 2;
  if (i + oaBytes + 3 > len) return false;
  if ((toa & 0x70) == 0x50) {
    h.sender = "<service>";                    // alphanumeric origin address
  } else {
    h.sender = decodeSemi(p + i, oaBytes, oaDigits);
    if (toa == 0x91) h.sender = "+" + h.sender;
  }
  i += oaBytes;
  i++;                                         // TP-PID
  if (i >= len) return false;
  h.dcs = p[i++];
  if (withScts) {
    if (i + 7 > len || !validScts(p + i)) return false;
    i += 7;                                    // TP-SCTS
  }
  if (i + 1 > len) return false;
  h.udl = p[i++];
  if (h.udhi) {
    if (i >= len) return false;
    uint8_t udhl = p[i++];
    size_t end = i + udhl;
    if (end > len) end = len;
    while (i + 1 < end) {
      uint8_t iei = p[i++];
      uint8_t iedl = p[i++];
      if (i + iedl > end) break;
      if (iei == 0x00 && iedl >= 3) {
        h.ref = p[i]; h.total = p[i + 1]; h.idx = p[i + 2]; h.concat = true;
      } else if (iei == 0x08 && iedl >= 4) {
        h.ref = (uint16_t)((p[i] << 8) | p[i + 1]);
        h.total = p[i + 2]; h.idx = p[i + 3]; h.concat = true;
      }
      i += iedl;
    }
    i = end;
  }
  if (i >= len) return false;
  h.udStart = i;
  return true;
}

bool SmsRx::handlePdu(const uint8_t* pdu, size_t len) {
  if (len < 10) return false;
  SmsHeader h;
  bool ok = false;
  // The modem usually returns the PDU with the SMSC address. Some FSO replies
  // omit TP-SCTS, so the standard layout is tried before the compact variant.
  for (int withSca = 1; withSca >= 0 && !ok; withSca--) {
    size_t start = withSca ? (size_t)(1 + pdu[0]) : 0;
    if (start >= len) continue;
    for (int withScts = 1; withScts >= 0 && !ok; withScts--) {
      if (parseSmsHeader(pdu, len, start, withScts != 0, h)) ok = true;
    }
  }
  if (!ok) return false;

  uint16_t ref = h.ref;
  uint8_t  total = h.total, idx = h.idx;
  bool     concat = h.concat;
  size_t   i = h.udStart;
  uint8_t  dcs = h.dcs;
  uint8_t  udl = h.udl;
  String   sender = h.sender;

  static uint8_t ud[512];
  size_t avail = len - i;
  size_t udLen = 0;
  uint8_t dcsClass = (uint8_t)(dcs & 0x0C);
  if (dcsClass != 0x04) {
    Serial.printf("[rx] ignored non-binary SMS dcs=0x%02X from %s\n", dcs, sender.c_str());
    return true;
  }
  udLen = avail > sizeof(ud) ? sizeof(ud) : avail;
  memcpy(ud, pdu + i, udLen);
  if (udLen == 0) return false;
  _rxParts++;

  if (!concat) {
    emit(ud, udLen, sender);
    return true;
  }

  if (!_partActive || _partRef != ref) {
    resetRx();
    _partActive = true;
    _partRef = ref;
    _partTotal = total;
    _partGot = 0;
  }
  if (total > MAX_PARTS) total = MAX_PARTS;
  _partTotal = total;

  int slot = -1;
  for (uint8_t k = 0; k < _partCount; k++) {
    if (_partIdx[k] == idx) { slot = k; break; }
  }
  if (slot < 0) {
    if (_partCount >= MAX_PARTS) { resetRx(); return false; }
    slot = _partCount++;
    _partIdx[slot] = idx;
    _partGot++;
  }
  if (udLen > PART_MAX) udLen = PART_MAX;
  memcpy(_partData[slot], ud, udLen);
  _partLen[slot] = (uint16_t)udLen;
  _msgSender = sender;

  if (_partGot < _partTotal) return true;

  _msgLen = 0;
  for (uint8_t n = 0; n < _partTotal; n++) {
    for (uint8_t k = 0; k < _partCount; k++) {
      if (_partIdx[k] != n) continue;
      if (_msgLen + _partLen[k] > MSG_MAX) break;
      memcpy(_msg + _msgLen, _partData[k], _partLen[k]);
      _msgLen += _partLen[k];
    }
  }
  emit(_msg, _msgLen, _msgSender);
  resetRx();
  return true;
}

bool SmsRx::readSlot(uint8_t index) {
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "AT+CMGR=%u", (unsigned)index);
  String resp = g_sim.cmdLong(cmd, 1500);
  // The SIM800 answers AT+CMGR with "+CMGR: <len>,<stat>,<from>" (some
  // firmwares use "+CMGS:"). The TPDU hex follows on the next line.
  int p = resp.indexOf("+CMGR:");
  if (p < 0) p = resp.indexOf("+CMGS:");
  if (p < 0) {
    if (resp.length() && resp.indexOf("ERROR") < 0 && resp.indexOf("OK") < 0)
      Serial.printf("[rx] slot %u: no header (%u bytes): %s\n", (unsigned)index,
                    (unsigned)resp.length(), resp.c_str());
    return false;
  }
  int hstart = resp.indexOf('\n', p);
  if (hstart < 0) return false;
  hstart++;
  while (hstart < (int)resp.length() && (resp[hstart] == '\r' || resp[hstart] == '\n')) hstart++;
  int hend = resp.indexOf('\n', hstart);
  if (hend < 0) hend = resp.length();
  String hex = resp.substring(hstart, hend);
  hex.trim();
  if (hex.length() < 8) return false;

  static uint8_t pdu[512];
  size_t n = hexToBytes(hex.c_str(), hex.length(), pdu, sizeof(pdu));
  bool ok = handlePdu(pdu, n);
  Serial.printf("[rx] slot %u: %u hex chars -> %u bytes, parsed=%d\n", (unsigned)index,
                (unsigned)hex.length(), (unsigned)n, ok ? 1 : 0);
  if (!ok) return false;
  g_sim.deleteSms(index);
  return ok;
}

void SmsRx::tick() {
  if (!g_stats.simReady) return;
  if (g_sim.passthroughActive) return;       // UART carries the data link
  if (millis() < _nextPoll) return;
  _nextPoll = millis() + _pollMs;
  // Only one slot per tick (round robin): reading all four slots blocks the
  // main loop for up to 4x the AT timeout and starved the task watchdog.
  STEP("smsrx_slot");
  uint8_t slot = _nextSlot;
  _nextSlot = (uint8_t)((_nextSlot + 1) & 3);
  readSlot(slot);
}
