#include "sim800.h"
#include "step.h"
#include "esp_task_wdt.h"

#if defined(BOARD_T_CALL)
#include <Wire.h>
// TTGO T-Call power: the IP5306 PMU is left at its factory default (0x35 =
// "boost auto").  Do NOT write 0x37 ("boost keep-on") here: with the boost
// forced on the 3.3 V rail collapses as soon as the SIM800 draws its boot /
// transmit current (~2 A peak) from a normal USB supply, and the ESP32 then
// loses DRAM contents (heap assert / stack canary / WDT reset).  The modem
// rail is fully controlled by the ESP32 (GPIO23 = POWER_ON), so the PMU does
// not have to be forced on.
static void tcallPmuInit() {}
#endif

Sim800 g_sim;

void Sim800::begin(HardwareSerial& ser, long baud, int8_t rxPin, int8_t txPin,
                   int8_t pwrPin, int8_t rstPin, int8_t powerPin) {
  _ser = &ser;
  _pwrPin = pwrPin;
#if defined(BOARD_T_CALL)
  tcallPmuInit();
#endif
  // T-Call: release modem reset and enable its power rail BEFORE talking to it.
  // Without these the SIM800 is held in reset / unpowered and never answers AT.
  if (rstPin >= 0) { pinMode(rstPin, OUTPUT); digitalWrite(rstPin, HIGH); }
  if (powerPin >= 0) { pinMode(powerPin, OUTPUT); digitalWrite(powerPin, HIGH); }
  if (rstPin >= 0 || powerPin >= 0) delay(100);
  ser.begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(100);
  flushInput();
  // The SIM800 needs ~12-20 s after its rail was switched on and then reports
  // "RDY" / "Call Ready" / "SMS Ready" on its own.  Wait for that instead of
  // pulsing PWRKEY while the module is still booting: such a pulse switches
  // the module off again and adds a second current spike on the 3.3 V rail.
  if (powerPin >= 0) {
    Serial.println("[sim] waiting for modem boot (max 25s)...");
    uint32_t t0 = millis();
    String boot;
    while (millis() - t0 < 25000) {
      while (ser.available()) {
        char c = (char)ser.read();
        boot += c;
        if (boot.length() > 256) boot.remove(0, boot.length() - 128);
      }
      if (boot.indexOf("RDY") >= 0 || boot.indexOf("Call Ready") >= 0 ||
          boot.indexOf("SMS Ready") >= 0) break;
      delay(20);
    }
    Serial.println("[sim] modem boot stage done");
  }
  // The modem survives an ESP reset and may be left in transparent data mode: any
  // AT command then goes to the socket and gets no reply. 1.2s of silence + "+++" exits to AT.
  {
    uint32_t t0 = millis();
    while (millis() - t0 < 1200) {
      while (ser.available()) ser.read();
      delay(1);
    }
    ser.print("+++");
    delay(1000);
    flushInput();
  }
  // On T-Call the modem is battery-powered and may already be on, so the
  // PWRKEY pulse is only sent if the modem doesn't answer (otherwise the pulse would switch it off).
  for (int i = 0; i < 3; i++) {
    sendCmd("AT", true);
    if (await("OK", 1500)) break;
    if (_pwrPin >= 0) { powerPulse(); delay(8000); }
    delay(1000);
  }
  sendCmd("ATE0", true); await("OK", 1500);
  setSmsPduMode();
  sendCmd("AT+CMEE=1", true); await("OK", 1000);
}

void Sim800::powerPulse() {
  if (_pwrPin < 0) return;
  pinMode(_pwrPin, OUTPUT);
  digitalWrite(_pwrPin, HIGH);
  delay(200);
  digitalWrite(_pwrPin, LOW);
  delay(1200);
  digitalWrite(_pwrPin, HIGH);
}

void Sim800::flushInput() {
  while (_ser->available()) _ser->read();
}

void Sim800::sendRaw(const char* s) {
  _ser->print(s);
}

void Sim800::sendCmd(const char* s, bool withCR) {
  flushInput();
  _ser->print(s);
  if (withCR) _ser->print("\r\n");
}

// Accumulates raw modem output until `expect` appears anywhere (works for
// line responses AND prompts like '>' ). Returns full accumulated text.
bool Sim800::await(const char* expect, uint32_t timeoutMs, String* response) {
  String acc;
  uint32_t t0 = millis();
  STEP("sim_await");
  while (millis() - t0 < timeoutMs) {
    // The guard is important: with a continuous data stream (transparent mode)
    // the inner loop would never end and the timeout would never be checked.
    int guard = 256;
    while (_ser->available() && guard-- > 0) {
      char c = (char)_ser->read();
      acc += c;
      if (acc.length() > 512) acc.remove(0, acc.length() - 256);
      if (acc.indexOf(expect) >= 0) {
        if (response) *response = acc;
        return true;
      }
    }
    delay(1);
    esp_task_wdt_reset();     // keep the task watchdog happy during long waits
  }
  if (response) *response = acc;      // return stale partial data on timeout too
  return false;
}

bool Sim800::at() {
  sendCmd("AT", true);
  return await("OK", 1500);
}

String Sim800::rawCmd(const char* cmd, uint32_t timeoutMs) {
  flushInput();
  _ser->print(cmd);
  _ser->print("\r\n");
  String acc;
  await("\r\nOK\r\n", timeoutMs, &acc);
  if (verbose) {
    if (acc.length()) Serial.printf("[sim] -> %s\n[sim] <- %s\n", cmd, acc.c_str());
    else               Serial.printf("[sim] -> %s\n[sim] <- (no answer)\n", cmd);
  }
  return acc;
}

String Sim800::cmdLong(const char* cmd, uint32_t timeoutMs) {
  flushInput();
  _ser->print(cmd);
  _ser->print("\r\n");
  String acc;
  uint32_t t0 = millis();
  STEP("sim_cmdlong");
  while (millis() - t0 < timeoutMs) {
    int guard = 256;
    while (_ser->available() && guard-- > 0) {
      acc += (char)_ser->read();
      if (acc.length() > 6000) acc.remove(0, 1000);
      if (acc.indexOf("\r\nOK\r\n") >= 0 || acc.indexOf("+CMS ERROR") >= 0 ||
          acc.indexOf("\r\nERROR") >= 0 || acc.indexOf("\r\nNO CARRIER") >= 0) {
        if (verbose) Serial.printf("[sim] -> %s\n[sim] <- %s\n", cmd, acc.c_str());
        return acc;
      }
    }
    delay(1);
    esp_task_wdt_reset();
  }
  if (verbose) Serial.printf("[sim] -> %s\n[sim] <- (timeout)\n", cmd);
  return acc;
}

bool Sim800::deleteSms(uint8_t index) {
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "AT+CMGD=%u", (unsigned)index);
  sendCmd(cmd, true);
  return await("OK", 5000);
}

static String field_after(const String& acc, const char* tag) {
  int i = acc.indexOf(tag);
  if (i < 0) return "";
  String s = acc.substring(i + strlen(tag));
  s.trim();
  return s;
}

int Sim800::csq() {
  if (passthroughActive) return -1;   // cannot read UART in transparent mode
  sendCmd("AT+CSQ", true);
  String acc;
  if (!await("OK", 2000, &acc)) return -1;
  String s = field_after(acc, "+CSQ:");
  int comma = s.indexOf(',');
  if (comma >= 0) s = s.substring(0, comma);
  s.trim();
  int csq = s.toInt();
  return (csq >= 0 && csq <= 31) ? csq : -1;
}

bool Sim800::creg() {
  if (passthroughActive) return false;   // cannot read UART in transparent mode
  sendCmd("AT+CREG?", true);
  String acc;
  if (!await("OK", 2000, &acc)) return false;
  String s = field_after(acc, "+CREG:");
  int comma = s.indexOf(',');
  if (comma >= 0) s = s.substring(comma + 1);
  s.trim();
  int stat = s.toInt();
  return (stat == 1 || stat == 5);
}

void Sim800::setSmsPduMode() {
  sendCmd("AT+CMGF=0", true);
  await("OK", 1500);
}

bool Sim800::sendPdu(const char* hex, int len) {
  if (passthroughActive) return false;   // caller must backToAT() first
  char cmd[32];
  String acc;
  snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", len);
  sendCmd(cmd, true);
  Serial.printf("[sim] CMGS=%d sent, waiting '>'\n", len);
  if (!await(">", 10000, &acc)) {
    Serial.print("[sms] no '>' prompt: "); Serial.println(acc);
    flushInput(); sendRaw("\x1A"); await("OK", 2000);   // unlock modem
    return false;
  }
  Serial.println("[sim] '>' ok, writing PDU");
  _ser->print(hex);
  _ser->write(0x1A);
  Serial.println("[sim] PDU written, waiting +CMGS:");
  acc = "";
  bool ok = await("+CMGS:", 15000, &acc);
  if (ok) {
    Serial.print("[sms] +CMGS resp: "); Serial.println(acc);
  } else {
    Serial.print("[sms] no +CMGS resp: "); Serial.println(acc);
    flushInput(); sendRaw("\x1A"); await("OK", 2000);   // unlock modem
  }
  return ok;
}

// --- GPRS ---
static bool parse_ip_from_sapbr(const String& acc, String& ip) {
  int i = acc.indexOf('"');
  if (i < 0) return false;
  String tail = acc.substring(i + 1);
  int j = tail.indexOf('"');
  if (j <= 0) return false;
  ip = tail.substring(0, j);
  return ip.length() && ip != "0.0.0.0";
}

static bool looks_like_ip(const String& s) {
  int dots = 0;
  if (!s.length()) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '.') { dots++; continue; }
    if (c < '0' || c > '9') return false;
  }
  return dots == 3;
}

// AT+CDNSGIP="host" -> +CDNSGIP: 1,"host","1.2.3.4" ; otherwise "" (CIPSTART by domain
// on this modem yields CONNECT FAIL, so we resolve through the modem and go by IP).
static String resolve_host_ip(const String& host) {
  if (host.length() == 0) return "";
  if (looks_like_ip(host)) return host;
  g_sim.sendCmd(("AT+CDNSGIP=\"" + host + "\"").c_str(), true);
  String acc;
  g_sim.await("OK", 20000, &acc);
  int i = acc.indexOf("+CDNSGIP:");
  if (i < 0) return "";
  String rest = acc.substring(i + 9);
  int q = 0;
  while (true) {
    int a = rest.indexOf('"', q);
    if (a < 0) break;
    int b = rest.indexOf('"', a + 1);
    if (b < 0) break;
    String tok = rest.substring(a + 1, b);
    if (looks_like_ip(tok)) return tok;
    q = b + 1;
  }
  return "";
}

bool Sim800::gprsUp(const String& apn, const String& user, const String& pass, String& ip) {
  // Already up? (after an ESP reset the modem may still be holding the bearer)
  sendCmd("AT+SAPBR=2,1", true);
  String acc;
  if (await("OK", 4000, &acc) && parse_ip_from_sapbr(acc, ip)) {
    pdpClosed = false;
    return true;
  }
  // CIPMODE=1 MUST be set BEFORE opening the bearer: on an active PDP the command
  // is ignored, and CIPSTART then returns +CME ERROR: 3.
  sendCmd("AT+CIPMODE=1", true);
  await("OK", 2000);
  sendCmd("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", true); await("OK", 2000);
  String apnCmd = "AT+CSTT=\"" + apn + "\",\"" + user + "\",\"" + pass + "\"";
  sendCmd(apnCmd.c_str(), true);
  await("OK", 2500);                                   // not fatal (CSTT sometimes returns nothing)
  sendCmd("AT+SAPBR=1,1", true);                       // open bearer (up to 60s)
  await("OK", 45000);
  sendCmd("AT+CIICR", true);                           // attach (up to 60s)
  await("OK", 45000);
  // Poll the IP: on this modem the automatic "CIFSR" may stay silent, so read the IP via SAPBR.
  for (int attempt = 0; attempt < 5; attempt++) {
    sendCmd("AT+SAPBR=2,1", true);
    if (await("OK", 8000, &acc) && parse_ip_from_sapbr(acc, ip)) {
      pdpClosed = false;
      return true;
    }
    delay(3000);
  }
  return false;
}

static void parse_cipping(const String& acc, int* sent, int* rcvd, int* rttMs) {
  int idx = acc.indexOf("+CIPPING:");
  if (idx < 0) return;
  String s = acc.substring(idx + 9);
  s.trim();
  int i1 = s.indexOf(',');
  if (i1 < 0) { if (sent) *sent = s.toInt(); return; }
  if (sent) *sent = s.substring(0, i1).toInt();
  s = s.substring(i1 + 1);            // rcvd,rtt[,ttl]
  int i2 = s.indexOf(',');
  if (i2 < 0) { if (rcvd) *rcvd = s.toInt(); return; }
  if (rcvd) *rcvd = s.substring(0, i2).toInt();
  s = s.substring(i2 + 1);
  int idx2 = s.indexOf(',');
  if (rttMs) *rttMs = (idx2 >= 0 ? s.substring(0, idx2) : s).toInt();
}

bool Sim800::pingHost(const String& host, int* sent, int* rcvd, int* rttMs) {
  String cmd = "AT+CIPPING=\"" + host + "\",3,1,1000";
  sendCmd(cmd.c_str(), true);
  String acc;
  if (!await("OK", 20000, &acc)) return false;
  if (acc.indexOf("+CIPPING:") < 0) return false;
  parse_cipping(acc, sent, rcvd, rttMs);
  return (rcvd && *rcvd > 0);
}

bool Sim800::tcpOpenPassthrough(const String& host, int port, const String& apn, const String& user, const String& pass) {
  // Always clear the TCP stack: the modem survives an ESP reset and keeps the socket,
  // which makes CIPSTART return +CME ERROR: 3 (operation not allowed).
  if (passthroughActive) backToAT();
  sendCmd("AT+CIPSHUT", true);
  await("OK", 10000);
  passthroughActive = false;
  pdpClosed = true;

  String ip;
  if (!gprsUp(apn, user, pass, ip)) {
    Serial.println("[sim] gprsUp FAIL");
    return false;
  }
  flushInput();
  String connHost = resolve_host_ip(host);
  if (!connHost.length()) {
    Serial.printf("[sim] DNS %s FAIL -> CIPSTART by name\n", host.c_str());
    connHost = host;
  } else if (connHost != host) {
    Serial.printf("[sim] DNS %s -> %s\n", host.c_str(), connHost.c_str());
  }
  /***************************************************************************
   * SIM800 flow for transparent TCP:
   * 0) AT+CIPSHUT -> clean TCP stack state (otherwise +CME ERROR: 3)
   * 1) AT+CIPMODE=1 (inside gprsUp, BEFORE the bearer is opened) -> transparent mode
   * 2) AT+CIPSTART="TCP",<host>,<p> -> "OK", then asynchronously "CONNECT"
   * 3) after "CONNECT" the modem itself is in data mode - raw bytes without CIPSEND.
   * Exit data mode: 1.2s of silence + "+++".
   **************************************************************************/
  String cmd = "AT+CIPSTART=\"TCP\",\"" + connHost + "\"," + String(port);
  sendCmd(cmd.c_str(), true);
  // In CIPMODE=1 the modem sends "OK", then LINE BY LINE either "CONNECT" (success, data mode),
  // or "CONNECT FAIL"/"ERROR" (not established). A prefix indexOf("CONNECT")
  // wrongly treated "CONNECT FAIL" as success -> read line by line.
  String line;
  bool connected = false, failed = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 40000) {
    while (_ser->available()) {
      char c = (char)_ser->read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (line == "CONNECT") { connected = true; break; }
        if (line == "CONNECT FAIL" || line == "ERROR") { failed = true; break; }
        line = "";
        continue;
      }
      line += c;
    }
    if (connected || failed) break;
    delay(2);
  }
  if (failed || !connected) {
    Serial.printf("[sim] CIPSTART %s:%d: %s, last='%s'\n", host.c_str(), port,
                  failed ? "FAIL" : "timeout", line.c_str());
    return false;
  }
  // Swallow the "\r\n" tail after CONNECT, otherwise TLS gets garbage (invalid record).
  delay(150);
  {
    String tail;
    uint32_t td = millis();
    while (millis() - td < 300) {
      while (_ser->available()) tail += (char)_ser->read();
      delay(1);
    }
    if (tail.length()) {
      Serial.printf("[sim] after CONNECT tail %d bytes:", tail.length());
      for (size_t i = 0; i < tail.length() && i < 32; i++)
        Serial.printf(" %02X", (uint8_t)tail[i]);
      Serial.println();
    }
  }
  passthroughActive = true;
  Serial.printf("[sim] passthrough %s:%d up\n", host.c_str(), port);
  return true;
}

bool Sim800::enterTransparent() {
  flushInput();
  _ser->print("AT+CIPSEND\r\n");
  String acc;
  if (!await(">", 5000, &acc)) {
    uint32_t t0 = millis();
    while (_ser->available() && millis() - t0 < 1000) acc += (char)_ser->read();
    // after waiting for ">" don't eat a possible "CONNECT OK"/data - dump on failure:
    Serial.printf("[sim] CIPSEND no '>', got: '%s'\n", acc.c_str());
    return false;
  }
  passthroughActive = true;
  return true;
}

bool Sim800::backToAT() {
  flushInput();
  uint32_t t0 = millis();
  while (millis() - t0 < 1200) {
    if (_ser->available()) _ser->read();   // hold the line quiet >=1s
    delay(1);
  }
  _ser->print("+++");
  bool ok = await("OK", 2000);
  passthroughActive = false;
  return ok;
}

bool Sim800::closeTcp() {
  if (passthroughActive) backToAT();
  sendCmd("AT+CIPSHUT", true);
  bool r = await("OK", 10000);
  passthroughActive = false;
  pdpClosed = true;
  return r;
}

int Sim800::tcpRead(uint8_t* out, size_t max, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  size_t n = 0;
  while (n == 0) {
    while (_ser->available() && n < max) out[n++] = (uint8_t)_ser->read();
    if (n) return (int)n;
    if (!passthroughActive) return -1;
    if (millis() - t0 > timeoutMs) return 0;
    delay(1);
  }
  return (int)n;
}

size_t Sim800::tcpWrite(const uint8_t* d, size_t n) {
  return _ser->write(d, n);
}
