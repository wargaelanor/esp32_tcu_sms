// ESP32-C3 + SIM800: WebSocket relay client (JAR replacement) + GPRS/WiFi + binary PDU SMS relay
// OpenCARWINGS server protocol (api/consumers.py):
//   ws://host/ws/smsgateway/?device_id=<id>
//   server -> client: binary (nonce16 || AES-CBC(JSON)), JSON: {"type":"pdu","pdu":hex,"data":hex,"length":N,"phone":...}
//   client -> server: text "ping" -> reply "pong"
// Build: Arduino IDE 2.x, board "ESP32C3 Dev Module", core esp32 2.0.x. No third-party libraries.
#include <Arduino.h>
#include <WiFi.h>
// Serial = HWCDC (native USB) with ARDUINO_USB_MODE=1 + CDC_ON_BOOT=1 (see platformio.ini)

#include "cfg.h"
#include "boards.h"
#include "sim800.h"
#include "transport.h"
#include "ws.h"
#include "tls_link.h"
#include "ws_link.h"
#include "aes_k.h"
#include "minijson.h"
#include "webui.h"

// ------------------------------- hardware pins -------------------------------
// Selected in boards.h depending on the target board (default ESP32-C3 + SIM800,
// BOARD_T_CALL -> TTGO T-Call v1.3/1.4).

// ------------------------------- global state -------------------------------
static uint8_t g_key[32];
static size_t  g_keyLen = 0;

static String ws_host, ws_path;
static int ws_port = 80;
static bool ws_secure = false;

static WsClient g_ws;

// GPRS path: SIM800 transparent TCP -> TLS -> WebSocket (WSS).
static GprsLink     g_gprsLink;
static TlsLink      g_tlsLink(&g_gprsLink);
static WsLinkClient g_wsLinkClient;

static bool   ws_armed = false;      // connection allowed to stay up
  static bool   ws_started = false;    // connection is up
static uint32_t wsNextTry = 0;
static const uint32_t WS_RETRY_MS = 8000;

static uint32_t lastPingSent = 0;
static uint32_t lastPongRecv = 0;
static bool     gprsUpOnceOk = false;
static uint32_t gprsNextUpTry = 0;

static uint32_t lastWifiRetry = 0;
static uint32_t staFailSince = 0;

// ------------------------------- wifi -------------------------------
static bool apRunning = false;

static void onApStaEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    uint8_t* m = info.wifi_ap_staconnected.mac;
    Serial.printf("[ap] +STA %02x:%02x:%02x:%02x:%02x:%02x\n", m[0],m[1],m[2],m[3],m[4],m[5]);
  } else if (ev == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    uint8_t* m = info.wifi_ap_stadisconnected.mac;
    Serial.printf("[ap] -STA %02x:%02x:%02x:%02x:%02x:%02x\n", m[0],m[1],m[2],m[3],m[4],m[5]);
  } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("[evt] STA disconnected reason=%u (uptime=%lus)\n", r, (unsigned long)(millis()/1000));
  } else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.printf("[evt] STA connected (uptime=%lus)\n", (unsigned long)(millis()/1000));
  }
}

static bool startAp() {
  if (apRunning) return true;
  IPAddress local_ip(192,168,4,1);
  IPAddress gateway(192,168,4,1);
  IPAddress subnet(255,255,255,0);
  bool ok = WiFi.softAP(g_cfg.ap_ssid.c_str(), g_cfg.ap_pass.c_str(), 6, false, 4);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  if (!ok) { delay(300); WiFi.softAPdisconnect(true); ok = WiFi.softAP(g_cfg.ap_ssid.c_str(), g_cfg.ap_pass.c_str(), 6, false, 4); }
  if (ok) {
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
      esp_netif_dhcp_status_t st;
      esp_err_t e = esp_netif_dhcps_get_status(ap, &st);
      if (e == ESP_OK && st != ESP_NETIF_DHCP_STARTED) { esp_netif_dhcps_start(ap); delay(50); esp_netif_dhcps_get_status(ap, &st); }
      Serial.printf("[wifi] ap dhcp status=%d\n", (int)st);
    }
  }
  apRunning = ok;
  Serial.printf("[wifi] AP '%s' ok=%d ip=%s\n", g_cfg.ap_ssid.c_str(), ok, local_ip.toString().c_str());
  return ok;
}

static void stopAp() {
  if (!apRunning) return;
  WiFi.softAPdisconnect(true);
  apRunning = false;
}

static void startApIfEnabled() {
  if (!g_cfg.ap_enable) {
    if (apRunning) stopAp();
    return;
  }
  if (!apRunning) startAp();
}

static void wifi_init() {
  if (!g_cfg.wifi_ssid.length()) {
    WiFi.mode(WIFI_AP);
    startApIfEnabled();
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  startApIfEnabled();
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
  Serial.printf("[wifi] STA+AP; connecting to %s\n", g_cfg.wifi_ssid.c_str());
}

static void wifi_tick() {
  if (!g_cfg.wifi_ssid.length()) {
    if (!apRunning && millis() > 500) startApIfEnabled();
    return;
  }
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    staFailSince = 0;
    startApIfEnabled();   // AP available for reconfiguration (if ap_enable)
    return;
  }
  if (staFailSince == 0) staFailSince = millis();
  if (millis() - lastWifiRetry > 10000) {
    lastWifiRetry = millis();
    WiFi.begin(g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
  }
}

// ------------------------------- GPRS -------------------------------
static bool gprs_up_once() {
  String ip;
  bool ok = g_sim.gprsUp(g_cfg.apn, g_cfg.apn_user, g_cfg.apn_pass, ip);
  g_stats.gprsUp = ok;
  g_stats.gprsIp = ok ? ip : "";
  Serial.printf("[gprs] up=%d ip=%s\n", ok, ip.c_str());
  gprsUpOnceOk = ok;
  if (!ok) gprsNextUpTry = millis() + 30000;
  return ok;
}

// ------------------------------- WS -------------------------------
static void buildWsPath(String& path) {
  if (g_cfg.jar_device_id.length()) {
    path += (path.indexOf('?') >= 0 ? "&" : "?") + String("device_id=") + g_cfg.jar_device_id;
  }
}

bool ws_start() {
  if (g_cfg.net_mode == "gprs") {
    if (!gprsUpOnceOk && !gprs_up_once()) return false;
    if (!gprsUpOnceOk) return false;
    String path = ws_path;
    buildWsPath(path);
    g_gprsLink.configure(ws_host, ws_port == 0 ? (ws_secure ? 443 : 80) : ws_port, g_cfg.apn, g_cfg.apn_user, g_cfg.apn_pass);
    Serial.printf("[ws] gprs connecting %s://%s:%d%s%s\n",
                  ws_secure ? "wss" : "ws", ws_host.c_str(), ws_port, path.c_str(),
                  ws_secure ? " (TLS over SIM800)" : "");
    if (ws_secure) g_tlsLink.configure(ws_host);
    g_wsLinkClient.begin(ws_secure ? (Link*)&g_tlsLink : (Link*)&g_gprsLink, ws_host, path);
    g_stats.wsConnected = false;
    g_stats.wsState = "connecting";
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  String path = ws_path;
  buildWsPath(path);
  Serial.printf("[ws] connecting %s:%d%s\n", ws_host.c_str(), ws_port, path.c_str());
  g_ws.begin(ws_host, ws_port, path, ws_secure);
  g_stats.wsConnected = false;
  g_stats.wsState = "connecting";
  return true;
}

void ws_stop() {
  g_ws.stop();
  g_wsLinkClient.stop();
  g_stats.wsConnected = false;
  g_stats.wsState = "down";
}

void ws_force_reconnect() {
  ws_stop();
  ws_started = false;
  wsNextTry = millis() + 500;
}

static bool sendSmsFromPdu(const String& pduHex, int cmgsLen);

static String g_lastPdu;
static int    g_lastPduLen = 0;

// decrypted JSON from server
static void onParcelText(const String& jsonRaw) {
  String type;
  if (!json_get_string(jsonRaw.c_str(), jsonRaw.length(), "type", type)) return;
  if (type == "pdu") {
    String pdu, phone;
    long length = 0;
    json_get_string(jsonRaw.c_str(), jsonRaw.length(), "pdu", pdu);
    json_get_string(jsonRaw.c_str(), jsonRaw.length(), "phone", phone);
    json_get_int(jsonRaw.c_str(), jsonRaw.length(), "length", length);
    Serial.printf("[ws] PDU len=%d phone=%s pdu=%s\n", (int)length, phone.c_str(), pdu.c_str());
    g_lastPdu = pdu; g_lastPduLen = (int)length;
    g_stats.smsIn = jsonRaw;   // message text as received from the server (kept until the next one)
    if (phone.length()) {
      bool ok = sendSmsFromPdu(pdu, (int)length);
      g_stats.smsLast = (ok ? "OK len=" : "FAIL len=") + String(length);
      if (ok) g_stats.smsOk++; else g_stats.smsFail++;
    }
  } else if (type == "sms") {
    Serial.println("[ws] sms(plain) not supported, skip");   // plain-text route is not used
  } else if (type == "connect") {
    Serial.println("[ws] server: connect");
  } else {
    Serial.printf("[ws] unknown type=%s\n", type.c_str());
  }
}

static void onWsText(const char* txt, size_t len) {
  if ((len == 4) && memcmp(txt, "pong", 4) == 0) {
    lastPongRecv = millis();
    g_ws.aliveSinceLastPong = true;
    g_wsLinkClient.aliveSinceLastPong = true;
  }
}

static void onWsBin(const uint8_t* d, size_t len) {
  uint8_t plain[2048];
  size_t plen = 0;
  if (!aes_cbc_decrypt_parcel(g_key, g_keyLen, d, len, plain, plen)) {
    Serial.println("[ws] aes decrypt FAIL");
    Serial.printf("[ws] FAILHEX(%u): ", (unsigned)len);
    for (size_t i = 0; i < len && i < 64; i++) Serial.printf("%02x", d[i]);
    Serial.println();
    g_stats.smsLast = "decrypt error";
    return;
  }
  String jsonRaw((const char*)plain, plen);
  onParcelText(jsonRaw);
}

static void ws_onConnect() {
  g_stats.wsConnected = true;
  g_stats.wsState = "up";
  g_stats.wsConnects++;
  g_stats.wsReconnects = 0;
  lastPingSent = millis();
  lastPongRecv = millis();
  if (g_cfg.net_mode == "gprs") {
    Serial.printf("[ws] connected via GPRS (heap=%u)\n", (unsigned)ESP.getFreeHeap());
  } else {
    Serial.printf("[ws] connected (rssi=%d heap=%u)\n", (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  }
}

static void ws_onDisconnect() {
  g_stats.wsConnected = false;
  g_stats.wsState = "down";
  Serial.printf("[ws] disconnected (wifi=%d rssi=%d heap=%u)\n",
                (int)WiFi.status(), (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  if (!g_cfg.ws_autoreconnect) {
    ws_armed = false;
    ws_started = false;
  }
}

void ws_setup() {
  g_ws.onText = onWsText;
  g_ws.onBin = onWsBin;
  g_ws.onConnect = ws_onConnect;
  g_ws.onDisconnect = ws_onDisconnect;
  g_wsLinkClient.onText = onWsText;
  g_wsLinkClient.onBin = onWsBin;
  g_wsLinkClient.onConnect = ws_onConnect;
  g_wsLinkClient.onDisconnect = ws_onDisconnect;
}

void ws_tick() {
  if (!ws_armed) {
    g_ws.tick();
    g_wsLinkClient.stop();        // don't retry or keep the modem alive while disabled
    return;
  }
  bool gprs = (g_cfg.net_mode == "gprs");

  if (!ws_started) {
    if (millis() < wsNextTry) return;
    if (!gprs && WiFi.status() != WL_CONNECTED) {
      wsNextTry = millis() + 3000;
      return;
    }
    if (ws_start()) {
      ws_started = true;
    } else {
      g_stats.wsReconnects++;
      g_stats.wsLastErr = "connect fail";
      wsNextTry = millis() + WS_RETRY_MS;
    }
    return;
  }

  if (gprs) {
    g_wsLinkClient.tick();
    if (g_wsLinkClient.connected()) {
      if (millis() - lastPingSent > 15000) {
        lastPingSent = millis();
        g_wsLinkClient.sendText("ping");
      }
      if (millis() - lastPongRecv > 60000) {
        g_stats.wsReconnects++;
        g_stats.wsLastErr = "pong timeout";
        Serial.println("[ws] pong timeout -> reconnect");
        ws_force_reconnect();
        if (!g_cfg.ws_autoreconnect) ws_armed = false;
      }
    }
    return;
  }

  g_ws.tick();
  if (!g_ws.connected()) {
    // library reconnects on its own; wait for CONNECTED event
    return;
  }

  // heartbeats
  if (millis() - lastPingSent > 15000) {
    lastPingSent = millis();
    Serial.printf("[ws] tx ping (uptime=%lus rssi=%d)\n", (unsigned long)(millis()/1000), (int)WiFi.RSSI());
    g_ws.sendText("ping");
  }
  if (millis() - lastPongRecv > 60000) {
    g_stats.wsReconnects++;
    g_stats.wsLastErr = "pong timeout";
    Serial.println("[ws] pong timeout -> reconnect");
    ws_force_reconnect();
    if (!g_cfg.ws_autoreconnect) ws_armed = false;
  }
}

// ------------------------------- PDU -> SMS -------------------------------
// SIM800: AT+CMGS=tpduLen, body — the ENTIRE PDU including the SCA octet (leading 00 =
// "SMSC from SIM"). Without the leading 00 the modem hangs waiting for an SCA octet.
static bool sendSmsFromPdu(const String& pduHex, int cmgsLen) {
  if (cmgsLen <= 0 || !g_cfg.jar_device_id.length()) return false;
  String body = pduHex;
  int octets = body.length() / 2;
  if (octets == cmgsLen) {
    body = "00" + body;   // server sent TPDU without SCA -> prepend SCA `00`
    octets++;
  }
  if (octets != cmgsLen + 1) {
    Serial.printf("[sms] len mismatch pduHex=%d len=%d\n", pduHex.length(), cmgsLen);
    g_stats.smsLast = "len mismatch";
    return false;
  }
  g_stats.smsOut = body;   // SMS body that will be written to the modem (kept until the next one)
  bool usingGprs = (g_cfg.net_mode == "gprs") && g_sim.passthroughActive;
  if (usingGprs) g_sim.backToAT();            // pause data link for SMS
  bool ok = g_sim.sendPdu(body.c_str(), cmgsLen);
  if (usingGprs) {
    g_sim.closeTcp();                          // data link closed -> WS will reconnect on its own
    ws_force_reconnect();
  }
  Serial.printf("[sms] sendPdu len=%d -> %s\n", cmgsLen, ok ? "OK" : "FAIL");
  g_stats.lastSmsTs = millis();
  return ok;
}

// ------------------------------- SIM health -------------------------------
static uint32_t simPollAt = 0;
static uint32_t simLastDiag = 0;
void sim_tick() {
  if (g_sim.passthroughActive) return;   // transparent mode: UART busy with TLS stream
  if (millis() < simPollAt) return;
  simPollAt = millis() + 15000;

  if (g_stats.simReady) {
    g_stats.simCsq = g_sim.csq();
    g_stats.simReg = g_sim.creg();
    if (millis() - simLastDiag > 60000) {
      simLastDiag = millis();
      Serial.printf("[sim] csq=%d reg=%d up=%lus\n", (int)g_stats.simCsq, (int)g_stats.simReg, (unsigned long)(millis()/1000));
    }
  } else {
    bool ok = g_sim.at();
    if (ok) {
      g_stats.simReady = true;
      g_sim.setSmsPduMode();
      Serial.println("[sim] ready");
    } else {
      g_stats.simInitAttempts++;
      if (g_stats.simInitAttempts % 8 == 1) {
        Serial.printf("[sim] no AT answer (%u attempts)\n", (unsigned)g_stats.simInitAttempts);
      }
    }
  }
}

// ------------------------------- setup/loop -------------------------------

static int hexbyte(char a, char b) {
  int va = (a >= '0' && a <= '9') ? a - '0' : ((a | 0x20) - 'a' + 10);
  int vb = (b >= '0' && b <= '9') ? b - '0' : ((b | 0x20) - 'a' + 10);
  return (va < 0 || va > 15 || vb < 0 || vb > 15) ? -1 : (va * 16 + vb);
}

// CLI 'm': try AT+CMGS length variants on the last received PDU and print raw
// modem replies (find which length the SIM800 accepts).
static void diagPduTrials() {
  if (!g_lastPdu.length()) { Serial.println("[diag] no pdu stored"); return; }
  int octets = g_lastPdu.length() / 2;
  int sca = hexbyte(g_lastPdu[0], g_lastPdu[1]);
  int tpdulen = (sca >= 0) ? (octets - 1 - sca) : octets;
  Serial.printf("[diag] pdu=%d octets sca=0x%02x tpdulen=%d\n", octets, sca, tpdulen);

  // abort any pending CMGS left by the auto path (ESC unlocks the modem)
  g_sim.flushInput();
  g_sim.sendRaw("\x1A");
  String r = g_sim.rawCmd("AT", 2000);
  Serial.printf("[diag] unlock(AT): %s\n", r.c_str());
  delay(500);

  // variant A: CMGS=88, body=full hex WITH SCA (89 octets incl. leading 00)
  // variant B: CMGS=88, body=TPDU only (drop leading 00) - SIM's default SMSC
  // variant C: CMGS=89, body=full hex (count SCA+TPDU)
  struct Try { int cmgs; bool strip; };
  Try tries[3] = { { tpdulen, false }, { tpdulen, true }, { octets, false } };
  for (int pass = 0; pass < 3; pass++) {
    String body = g_lastPdu;
    if (tries[pass].strip && body.length() >= 2) body = body.substring(2);
    Serial.printf("[diag] --- trial %d: CMGS=%d body=%d octets ---\n",
                  pass, tries[pass].cmgs, body.length() / 2);
    String r1 = g_sim.rawCmd("AT+CMGF=0", 2000);
    String cmds = String("AT+CMGS=") + String(tries[pass].cmgs);
    r = g_sim.rawCmd(cmds.c_str(), 4000);
    if (r.indexOf('>') < 0) {
      Serial.printf("[diag] no '>' resp(%u): %s\n", (unsigned)r.length(), r.c_str());
      // unlock again
      g_sim.flushInput(); g_sim.sendRaw("\x1A"); g_sim.rawCmd("AT", 2000);
      delay(2000);
      continue;
    }
    g_sim.sendRaw(body.c_str());
    g_sim.sendRaw("\x1A");
    String acc; uint32_t t0 = millis();
    bool done = false;
    while (millis() - t0 < 12000) {
      while (SIM_SERIAL.available()) {
        char c = (char)SIM_SERIAL.read();
        acc += c;
        if (acc.length() > 600) acc.remove(0, acc.length() - 400);
      }
      if (acc.indexOf("+CMGS:") >= 0 || acc.indexOf("+CMS ERROR") >= 0 ||
          acc.indexOf("\r\nERROR") >= 0) { done = true; break; }
      delay(10);
    }
    Serial.printf("[diag] trial %d resp(%u, done=%d): %s\n", pass, (unsigned)acc.length(), (int)done, acc.c_str());
    // unlock for next pass
    g_sim.flushInput(); g_sim.sendRaw("\x1A"); g_sim.rawCmd("AT", 2000);
    delay(2000);
  }
}

// CLI 'z': manual two-way modem passthrough (USB<->SIM_SERIAL) for ~45s, exit with 'x'.
static void modemPassthrough() {
  Serial.println("[z] passthrough on (45s, type 'x' to exit)");
  uint32_t t0 = millis();
  while (millis() - t0 < 45000) {
    while (SIM_SERIAL.available()) Serial.write(SIM_SERIAL.read());
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == 'x') { Serial.println("[z] exit"); return; }
      SIM_SERIAL.write(c);
      Serial.write(c);   // local echo
    }
    delay(2);
  }
  Serial.println("[z] timeout");
}

// CLI 'g': bring up GPRS data and probe reachability (ICMP) to several targets.
static void gprsDiag() {
  Serial.printf("[gprs] diag apn='%s' user='%s' pass='%s'\n", g_cfg.apn.c_str(), g_cfg.apn_user.c_str(), g_cfg.apn_pass.c_str());
  g_sim.verbose = true;
  g_sim.rawCmd("AT", 2000);
  g_sim.rawCmd("ATI", 2000);                    // modem firmware version
  Serial.println("--- CIPMODE probe ---");
  g_sim.rawCmd("AT+CIPMODE?", 3000);
  g_sim.rawCmd("AT+CIPMODE=1", 5000);
  g_sim.rawCmd("AT+CIPMODE?", 3000);
  g_sim.rawCmd("AT+CIPMODE=0", 5000);
  Serial.println("--- GPRS bring-up ---");
  g_sim.rawCmd("AT+CPIN?", 2000);
  g_sim.rawCmd("AT+CREG?", 2000);
  String csttCmd = "AT+CSTT=\"" + g_cfg.apn + "\",\"" + g_cfg.apn_user + "\",\"" + g_cfg.apn_pass + "\"";
  g_sim.rawCmd(csttCmd.c_str(), 3000);
  Serial.println("[gprs] SAPBR=2,1 (query existing) ...");
  g_sim.rawCmd("AT+SAPBR=2,1", 3000);
  Serial.println("[gprs] SAPBR=1,1 (open bearer if needed, up to 60s) ...");
  g_sim.rawCmd("AT+SAPBR=1,1", 60000);
  Serial.println("[gprs] CIICR (attach, up to 40s) ...");
  g_sim.rawCmd("AT+CIICR", 40000);
  Serial.println("[gprs] SAPBR=2,1 (query) ...");
  g_sim.rawCmd("AT+SAPBR=2,1", 3000);
  Serial.println("--- transparent TCP full flow (CIPMODE=1 preferred) ---");
  g_sim.rawCmd("AT+CGATT?", 3000);
  g_sim.rawCmd("AT+CGACT?", 3000);
  // try transparent mode: CONNECT without OK (modem enters data mode after TCP established)
  Serial.println("[gprs] CIPSTART 104.21.1.89:443 (IP direct)...");
  String cip = g_sim.rawCmd("AT+CIPSTART=\"TCP\",\"104.21.1.89\",443", 30000);
  Serial.printf("[gprs] CIPSTART result len=%d: %s\n", cip.length(), cip.length()?cip.c_str():"(empty)");
  Serial.println("[gprs] CIPSEND ...");
  g_sim.rawCmd("AT+CIPSEND", 6000);
  Serial.println("[gprs] +++ (exit data mode) + CIPSHUT ...");
  g_sim.rawCmd("+++", 4000);
  g_sim.rawCmd("AT+CIPSHUT", 10000);
  g_sim.verbose = false;
  Serial.println("[gprs] done (see above)");
}

// --- CLI: char-by-char command read, buffer accumulation, handled on '\n'.
static String cliBuf;
void cli_tick() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (cliBuf.length()) {
        String c = cliBuf; cliBuf = ""; c.trim();
        if (c == "l" || c == "?") {
          Serial.printf("[cfg] net_mode=%s wifi_ssid='%s' wifi_pass='%s'\n", g_cfg.net_mode.c_str(), g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
          Serial.printf("[cfg] ap_ssid='%s' ap_pass='%s' ap_enable=%d\n", g_cfg.ap_ssid.c_str(), g_cfg.ap_pass.c_str(), (int)g_cfg.ap_enable);
          Serial.printf("[cfg] check_url=%s gprs_host=%s\n", g_cfg.check_url.c_str(), g_cfg.gprs_host.c_str());
          Serial.printf("[cfg] apn='%s' apn_user='%s' apn_pass='%s'\n", g_cfg.apn.c_str(), g_cfg.apn_user.c_str(), g_cfg.apn_pass.c_str());
          Serial.printf("[cfg] ws_url=%s autoreconnect=%d\n", g_cfg.ws_url.c_str(), g_cfg.ws_autoreconnect);
        }
        else if (c.length() > 2 && c[0] == 's' && c[1] == ' ') {
          String kv = c.substring(2);
          int eq = kv.indexOf('=');
          if (eq > 0) {
            String key = kv.substring(0, eq); key.trim();
            String val = kv.substring(eq + 1); val.trim();
            cfg_update(key, val);
            cfg_save();
            Serial.printf("[cfg] set %s='%s' saved\n", key.c_str(), val.c_str());
          } else {
            Serial.println("[cfg] format: s key=value");
          }
        }
        else if (c == "g") {
          gprsDiag();
        }
        else if (c == "o") {
          g_wsLinkClient.stop();
          ws_armed = !ws_armed;
          Serial.printf("[cli] ws_armed=%d\n", (int)ws_armed);
          if (!ws_armed) { ws_stop(); ws_started = false; }
        }
        else if (c == "q") {
          Serial.println("[sys] reboot");
          delay(100);
          ESP.restart();
        }
        else if (c == "k") {
          Serial.print("[dev] device_id="); Serial.println(g_cfg.jar_device_id);
          Serial.print("[dev] enc_key=");   Serial.println(g_cfg.jar_enc_key);
        }
        else if (c == "z") {
          modemPassthrough();
        }
        else if (c == "m") {
          diagPduTrials();
        }
        else if (c == "u") {
          Serial.printf("[st] armed=%d started=%d connected=%d state=%s\n",
            (int)ws_armed, (int)ws_started, (int)g_stats.wsConnected, g_stats.wsState.c_str());
          Serial.printf("[st] conns=%u recon=%u err=%s\n", (unsigned)g_stats.wsConnects, (unsigned)g_stats.wsReconnects, g_stats.wsLastErr.c_str());
          Serial.printf("[st] gprs=%d smsOk=%u smsFail=%u last='%s' wifi=%d heap=%u\n",
            (int)gprsUpOnceOk, (unsigned)g_stats.smsOk, (unsigned)g_stats.smsFail, g_stats.smsLast.c_str(),
            (int)WiFi.status(), (unsigned)ESP.getFreeHeap());
        }
        else if (c == "w") {
          uint8_t mode = WiFi.getMode();
          IPAddress aip = WiFi.softAPIP();
          String apip = String(aip[0]) + '.' + String(aip[1]) + '.' + String(aip[2]) + '.' + String(aip[3]);
          String apssid = WiFi.softAPSSID();
          Serial.printf("[ap] time_ms=%lu heap=%u rst=%d | mode=0x%02X | ssid='%s' | stations=%d | ip=%s\n",
            (unsigned long)millis(), (unsigned)ESP.getFreeHeap(), (int)esp_reset_reason(),
            mode, apssid.c_str(), WiFi.softAPgetStationNum(), apip.c_str());
          int n = WiFi.scanNetworks(false, true, false, 300);
          Serial.printf("[ap] scan=%d networks:\n", n);
          for (int i = 0; i < n && i < 20; i++) {
            String ssid = WiFi.SSID(i);
            Serial.printf("[ap]   ch%d rssi%d '%s'\n", WiFi.channel(i), WiFi.RSSI(i), ssid.c_str());
          }
        }
        else if (c == "r") {
          Serial.println("[reset] factory reset -> erased, rebooting...");
          LittleFS.format();
          delay(200);
          ESP.restart();
        }
        else {
          Serial.println("[cli] commands: l (list), s key=value, k (ID/key), w (WiFi), r (reset)");
        }
      }
    } else {
      cliBuf += ch;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32 TCU SMS (JAR relay) ===");
  Serial.printf("[fw] %s\n", FW_VER);
  int rr = (int)esp_reset_reason();
  Serial.printf("[rst] reason=%d heap=%u\n", rr, (unsigned)ESP.getFreeHeap());

  cfg_load();
  Serial.print("[cfg] "); Serial.println(cfg_status());
  Serial.print("[dev] device_id="); Serial.println(g_cfg.jar_device_id);
  Serial.print("[dev] enc_key=");   Serial.println(g_cfg.jar_enc_key);
  Serial.println("[dev] set these values in the car settings in OpenCARWINGS (sms_config)");

  // AES key
  g_keyLen = hex_key_parse(g_cfg.jar_enc_key, g_key, sizeof(g_key));
  if (g_keyLen != 16 && g_keyLen != 24 && g_keyLen != 32) {
    g_keyLen = 0;
    Serial.println("[key] encryption key invalid/empty -> WS disabled");
  }

  // WS URL
  if (ws_parse_url(g_cfg.ws_url, ws_host, ws_port, ws_path, ws_secure)) {
    Serial.printf("[url] %s://%s:%d%s\n", ws_secure ? "wss" : "ws", ws_host.c_str(), ws_port, ws_path.c_str());
  } else {
    Serial.println("[url] ws_url parse fail -> WS disabled");
  }

  g_sim.begin(SIM_SERIAL, 115200, SIM800_RX_PIN, SIM800_TX_PIN, SIM800_PWR_PIN);
  sim_tick();

  wifi_init();
  WiFi.onEvent(onApStaEvent);
  web_setup();
  ws_setup();

  if (g_keyLen && ws_host.length() && g_cfg.jar_device_id.length()) {
    ws_armed = true;
    wsNextTry = millis() + 1500;
  }
  Serial.printf("[main] ready; key=%d bytes; ws_armed=%d\n", (int)g_keyLen, ws_armed);
}

void loop() {
  cli_tick();
  web_handle();
  wifi_tick();
  sim_tick();
  ws_tick();
  delay(5);
}