// ESP32-C3 + SIM800: WebSocket relay client (JAR replacement) + GPRS/WiFi + binary PDU SMS relay
// OpenCARWINGS server protocol (api/consumers.py):
//   ws://host/ws/smsgateway/?device_id=<id>
//   server -> client: binary (nonce16 || AES-CBC(JSON)), JSON: {"type":"pdu","pdu":hex,"data":hex,"length":N,"phone":...}
//   client -> server: text "ping" -> reply "pong"
// Build: Arduino IDE 2.x, board "ESP32C3 Dev Module", core esp32 2.0.x. No third-party libraries.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
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
#include "smsrx.h"
#include "step.h"

const char* g_step = "boot";
#define STEP(s) do { g_step = (s); } while (0)

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

// SMS queue: keep the newest server command until the SIM800 acknowledges it.
struct SmsJob {
  String body;
  int    len = 0;
  uint8_t attempts = 0;
  bool   valid = false;
  uint32_t nextAt = 0;
};
static SmsJob g_smsJob;
static const uint8_t SMS_MAX_ATTEMPTS = 3;
static uint32_t g_smsBusyUntil = 0;
static uint32_t g_rxWindowUntil = 0;
static uint32_t g_wsResumeAt = 0;

struct RelayJob {
  uint8_t data[1600];
  size_t len = 0;
  uint8_t attempts = 0;
  bool valid = false;
  uint32_t nextAt = 0;
};
static RelayJob g_relayJob;

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
static bool wifiWasConnected = false;
static bool wifiSuspended = false;
static uint32_t wifiResumeAt = 0;
static String g_netEff = "wifi";
static uint32_t g_gprsDownSince = 0;
static uint32_t g_gprsUpAt = 0;
static bool g_relayBusy = false;
#if defined(BOARD_T_CALL)
static constexpr bool WIFI_TX_GUARD = true;
#else
static constexpr bool WIFI_TX_GUARD = false;
#endif

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

static void wifi_suspend() {
  if (!WIFI_TX_GUARD || wifiSuspended) return;
  wifiSuspended = true;
  Serial.println("[radio] suspend WiFi (SIM800 TX)");
  WiFi.softAPdisconnect(true);
  apRunning = false;
  WiFi.mode(WIFI_OFF);
  delay(50);
}

static void wifi_resume() {
  if (!WIFI_TX_GUARD || !wifiSuspended) return;
  if (millis() < wifiResumeAt) return;
  wifiSuspended = false;
  wifiResumeAt = 0;
  Serial.println("[radio] resume WiFi");
  bool useSta = g_netEff != "gprs" && g_cfg.wifi_ssid.length();
  WiFi.mode(useSta ? WIFI_AP_STA : WIFI_AP);
  startApIfEnabled();
  if (useSta) {
    WiFi.setAutoReconnect(!WIFI_TX_GUARD);
    WiFi.begin(g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
    lastWifiRetry = millis();
  }
}

static void wifi_gprs_ap_only() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  apRunning = false;
  startApIfEnabled();
  wifiSuspended = false;
  wifiResumeAt = 0;
  Serial.println("[radio] GPRS mode: AP only, STA disabled");
}

static void wifi_init() {
  if (!g_cfg.wifi_ssid.length()) {
    WiFi.mode(WIFI_AP);
    startApIfEnabled();
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  startApIfEnabled();
  WiFi.setAutoReconnect(!WIFI_TX_GUARD);
  WiFi.begin(g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
  Serial.printf("[wifi] STA+AP; connecting to %s\n", g_cfg.wifi_ssid.c_str());
}

static void wifi_tick() {
  if (wifiSuspended) {
    // wifi_resume uses AP-only mode during GPRS, so no STA scan can race the
    // SIM800. In WiFi mode it restores AP+STA normally.
    if (wifiResumeAt && millis() >= wifiResumeAt) wifi_resume();
    return;
  }
  // While GPRS owns the radio, never touch WiFi: even a background STA retry
  // triggers a scan and can corrupt the WiFi stack while SIM800 is streaming.
  if (g_netEff == "gprs") return;
  if (!g_cfg.wifi_ssid.length()) {
    if (!apRunning && millis() > 500) startApIfEnabled();
    return;
  }
  static uint32_t lastApTry = 0;
  if (g_cfg.ap_enable && !apRunning && millis() - lastApTry > 5000) {
    lastApTry = millis();
    startAp();
  }
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    staFailSince = 0;
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.printf("[wifi] STA restored (uptime=%lus rssi=%d)\n",
                    (unsigned long)(millis()/1000), (int)WiFi.RSSI());
    }
    startApIfEnabled();
    return;
  }
  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("[wifi] STA lost; reconnect is scheduled");
    lastWifiRetry = millis() - 9000;
  }
  if (staFailSince == 0) staFailSince = millis();
  // Avoid a full scan every 10 s: the scan draws a current spike and can be
  // the trigger for the T-Call brownout. Retry at a moderate interval.
  if (millis() - lastWifiRetry > 15000) {
    lastWifiRetry = millis();
    WiFi.begin(g_cfg.wifi_ssid.c_str(), g_cfg.wifi_pass.c_str());
    Serial.println("[wifi] retry STA connect");
  }
}

// ------------------------------- GPRS -------------------------------
String net_eff_name() { return g_netEff; }

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
  if (g_netEff == "gprs") {
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

static bool sendSmsNow(const String& pduHex, int cmgsLen);
static bool relayViaWifi(const uint8_t* data, size_t len);
static bool relayViaGprs(const uint8_t* data, size_t len);
static void relay_queue_tick();
static void sms_queue_tick();

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
    g_stats.smsIn = jsonRaw;   // message text received from the server (kept until the next one)
    if (phone.length()) {
      int octets = pdu.length() / 2;
      int wanted = (int)length;
      if (octets == wanted) pdu = "00" + pdu;
      if (pdu.length() / 2 != wanted + 1) {
        g_stats.smsLast = "len mismatch";
        g_stats.smsFail++;
        return;
      }
      if (g_smsJob.valid) {
        if (g_smsJob.body == pdu && g_smsJob.len == wanted) {
          Serial.println("[sms] duplicate queued PDU ignored");
        } else {
          g_stats.smsFail++;
          Serial.println("[sms] queue busy; new PDU rejected");
        }
        return;
      }
      g_smsJob.body = pdu;
      g_smsJob.len = wanted;
      g_smsJob.attempts = 0;
      g_smsJob.valid = true;
      g_smsJob.nextAt = millis();
      g_stats.smsQueued++;
      g_stats.smsLast = "queued len=" + String(wanted);
      Serial.printf("[sms] queued len=%d queue=%u\n", wanted, (unsigned)g_stats.smsQueued);
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
  if (WiFi.status() == WL_CONNECTED) g_netEff = "wifi";
  else if (g_cfg.net_mode == "gprs") g_netEff = "gprs";
  else g_netEff = "wifi";
  if (g_netEff == "gprs") {
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
  if (WiFi.status() != WL_CONNECTED && g_gprsDownSince == 0) g_gprsDownSince = millis();
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
  // Dynamic transport selection: keep WiFi as the primary path. If STA has
  // been unavailable long enough and GPRS is enabled, switch at a frame
  // boundary; switch back immediately when WiFi recovers.
  if (WiFi.status() == WL_CONNECTED) {
    if (g_netEff != "wifi" || g_gprsDownSince) {
      Serial.println("[net] WiFi available -> switch to WiFi transport");
      g_netEff = "wifi";
      g_gprsDownSince = 0;
      if (g_wsLinkClient.connected() || g_sim.passthroughActive) {
        g_wsLinkClient.stop();
        g_sim.closeTcp();
      }
      wifiResumeAt = 0;
      wifi_resume();
      ws_force_reconnect();
    }
  } else if (g_netEff == "gprs") {
    // GPRS owns the radio until an actual WiFi connection is available.
  } else if (g_gprsDownSince == 0) {
    g_gprsDownSince = millis();
  } else if (g_cfg.net_mode == "gprs" && millis() - g_gprsDownSince > 30000 &&
             g_netEff != "gprs" && !g_smsJob.valid && millis() >= g_rxWindowUntil) {
    Serial.println("[net] WiFi unavailable 30s -> switch to GPRS transport");
    g_netEff = "gprs";
    g_ws.stop();
    ws_started = false;
    wsNextTry = millis() + 500;
    wifi_gprs_ap_only();
  }

  if (!ws_armed) {
    g_ws.tick();
    g_wsLinkClient.stop();        // don't retry or keep the modem alive while disabled
    return;
  }
  bool gprs = (g_netEff == "gprs");

  // The SIM800 UART is exclusive: after a climate PDU is accepted, leave it
  // in AT mode until the FSO reply has been read and relayed.
  if (gprs && (g_smsJob.valid || g_relayJob.valid || millis() < g_rxWindowUntil ||
               g_relayBusy || g_sim.passthroughActive && !g_wsLinkClient.connected())) {
    return;
  }

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
    if (!gprsUpOnceOk && !gprs_up_once()) return;
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
static bool sendSmsNow(const String& pduHex, int cmgsLen) {
  if (cmgsLen <= 0 || !g_cfg.jar_device_id.length()) return false;
  g_stats.smsOut = pduHex;
  // The T-Call radio and SIM800 share a fragile power rail. GPRS TCP must be
  // closed before CMGS, and WiFi is kept quiet during the modem TX window.
  if (g_netEff == "gprs" && g_sim.passthroughActive) {
    g_wsLinkClient.stop();
    Serial.printf("[sms] GPRS socket closed, passthrough=%d\n",
                  (int)g_sim.passthroughActive);
    ws_started = false;
    if (g_sim.passthroughActive) return false;
  } else if (g_netEff == "wifi") {
    // Stop the library reconnect timer before taking the radio down for CMGS.
    g_ws.stop();
    ws_started = false;
  }
  wifi_suspend();
  bool ok = g_sim.sendPdu(pduHex.c_str(), cmgsLen);
  if (WIFI_TX_GUARD) wifiResumeAt = millis() + 2000;
  Serial.printf("[sms] sendPdu len=%d -> %s\n", cmgsLen, ok ? "OK" : "FAIL");
  g_stats.lastSmsTs = millis();
  g_smsBusyUntil = millis() + 30000;
  g_rxWindowUntil = millis() + 120000;
  g_smsRx.setPollInterval(2000);
  if (g_netEff == "gprs") {
    g_wsResumeAt = g_rxWindowUntil;
    wsNextTry = g_rxWindowUntil;
    Serial.println("[sms] GPRS WS paused for 120 s SMS reply window");
  } else {
    wsNextTry = wifiResumeAt + 1500;
    Serial.println("[sms] WiFi resume scheduled after CMGS guard");
  }
  return ok;
}

static void sms_queue_tick() {
  if (!g_smsJob.valid || millis() < g_smsJob.nextAt) return;
  if (g_sim.passthroughActive) {
    if (g_netEff == "gprs") {
      Serial.println("[sms] closing GPRS WS before CMGS");
      g_wsLinkClient.stop();
      ws_started = false;
    }
    return;   // GprsLink::close exits data mode; send on the next loop pass.
  }
  if (g_stats.simReady && !g_stats.simReg) {
    Serial.println("[sms] waiting for SIM registration");
    g_smsJob.nextAt = millis() + 3000;
    return;
  }
  if (g_smsJob.attempts >= SMS_MAX_ATTEMPTS) {
    Serial.println("[sms] queue exhausted; command dropped");
    g_smsJob.valid = false;
    g_stats.smsLast = "retry exhausted";
    g_stats.smsFail++;
    return;
  }
  g_smsJob.attempts++;
  bool ok = sendSmsNow(g_smsJob.body, g_smsJob.len);
  if (ok) {
    g_stats.smsOk++;
    g_stats.smsLast = "OK len=" + String(g_smsJob.len) + " attempt=" + String(g_smsJob.attempts);
    g_smsJob.valid = false;
  } else {
    g_stats.smsRetries++;
    g_stats.smsLast = "retry " + String(g_smsJob.attempts) + " len=" + String(g_smsJob.len);
    g_smsJob.nextAt = millis() + 5000 * g_smsJob.attempts;
  }
}

// ------------------------------- FSO reply relay -------------------------------
static bool relayViaWifi(const uint8_t* data, size_t len) {
  if (!g_cfg.relay_url.length()) return false;
  if (WiFi.status() != WL_CONNECTED || wifiSuspended) return false;
  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false);
  if (!http.begin(g_cfg.relay_url)) {
    Serial.println("[relay] http.begin FAIL");
    return false;
  }
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("User-Agent", "NISSAN TCU");
  int code = http.POST(const_cast<uint8_t*>(data), len);
  Serial.printf("[relay] wifi POST %s -> %d\n", g_cfg.relay_url.c_str(), code);
  http.end();
  return code >= 200 && code < 300;
}

static bool relayViaGprs(const uint8_t* data, size_t len) {
  if (!g_cfg.relay_url.length() || g_sim.passthroughActive) return false;
  String host = "138.16.224.7";
  String path = "/ficosa/gdc";
  int scheme = g_cfg.relay_url.indexOf("://");
  String rest = scheme >= 0 ? g_cfg.relay_url.substring(scheme + 3) : g_cfg.relay_url;
  int slash = rest.indexOf('/');
  String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
  if (slash >= 0) path = rest.substring(slash);
  int colon = hostPort.indexOf(':');
  int port = 80;
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = hostPort.substring(colon + 1).toInt();
  } else if (hostPort.length()) {
    host = hostPort;
  }
  if (port <= 0) return false;

  g_relayBusy = true;
  wifi_suspend();
  bool ok = false;
  if (g_sim.tcpOpenPassthrough(host, port, g_cfg.apn, g_cfg.apn_user, g_cfg.apn_pass)) {
    char header[320];
    int n = snprintf(header, sizeof(header),
                     "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: NISSAN TCU\r\n"
                     "Content-Type: application/octet-stream\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     path.c_str(), host.c_str(), (unsigned)len);
    bool wrote = n > 0 && (size_t)n < sizeof(header) &&
                 g_sim.tcpWrite((const uint8_t*)header, (size_t)n) == (size_t)n &&
                 g_sim.tcpWrite(data, len) == len;
    String response;
    uint8_t chunk[256];
    uint32_t started = millis();
    while (wrote && millis() - started < 20000) {
      int got = g_sim.tcpRead(chunk, sizeof(chunk), 1000);
      if (got < 0) break;
      if (got > 0) {
        for (int i = 0; i < got; i++) response += (char)chunk[i];
        if (response.length() > 512) response.remove(0, response.length() - 256);
        if (response.indexOf("\r\n\r\n") >= 0) break;
      }
    }
    int firstSpace = response.indexOf(' ');
    int code = 0;
    if (response.startsWith("HTTP/") && firstSpace > 0)
      code = response.substring(firstSpace + 1, firstSpace + 4).toInt();
    ok = code >= 200 && code < 300;
    Serial.printf("[relay] gprs POST %s:%d%s -> %d\n", host.c_str(), port, path.c_str(), code);
    g_sim.closeTcp();
  } else {
    Serial.println("[relay] GPRS TCP open failed");
  }
  g_relayBusy = false;
  if (WIFI_TX_GUARD) wifiResumeAt = millis() + 2000;
  return ok;
}

static void onFsoReply(const uint8_t* data, size_t len, const String& sender) {
  g_stats.rxParts = g_smsRx.parts();
  g_stats.rxMsgs = g_smsRx.messages();
  g_stats.rxLast = g_smsRx.lastInfo() + " from " + sender;
  g_rxWindowUntil = millis();
  g_wsResumeAt = 0;
  if (!g_cfg.relay_enabled) {
    Serial.println("[relay] disabled, answer kept on device only");
    return;
  }
  if (!g_cfg.relay_url.length()) {
    Serial.println("[relay] no relay_url configured");
    g_stats.relayFail++;
    return;
  }
  if (len > sizeof(g_relayJob.data)) len = sizeof(g_relayJob.data);
  memcpy(g_relayJob.data, data, len);
  g_relayJob.len = len;
  g_relayJob.attempts = 0;
  g_relayJob.valid = true;
  g_relayJob.nextAt = millis();
  Serial.printf("[relay] queued answer %u bytes\n", (unsigned)len);
  relay_queue_tick();
}

static void relay_queue_tick() {
  if (!g_relayJob.valid || millis() < g_relayJob.nextAt) return;
  if (g_sim.passthroughActive || millis() < g_rxWindowUntil) return;
  bool ok = WiFi.status() == WL_CONNECTED && !wifiSuspended
    ? relayViaWifi(g_relayJob.data, g_relayJob.len)
    : relayViaGprs(g_relayJob.data, g_relayJob.len);
  if (ok) {
    g_stats.relayOk++;
    g_stats.rxLast += " -> relayed";
    Serial.printf("[relay] answer %u bytes -> OK\n", (unsigned)g_relayJob.len);
    g_relayJob.valid = false;
    g_smsRx.setPollInterval(6000);
    if (g_netEff == "gprs") {
      g_smsJob.valid = false;
      g_rxWindowUntil = 0;
      wsNextTry = millis() + 1500;
    }
    return;
  }
  g_relayJob.attempts++;
  if (g_relayJob.attempts >= 4) {
    g_stats.relayFail++;
    g_stats.rxLast += " -> RELAY FAIL";
    Serial.printf("[relay] answer %u bytes -> FAIL after retries\n", (unsigned)g_relayJob.len);
    g_relayJob.valid = false;
    return;
  }
  g_relayJob.nextAt = millis() + 10000 * g_relayJob.attempts;
  Serial.printf("[relay] retry %u in %u ms\n", (unsigned)g_relayJob.attempts,
                (unsigned)(10000 * g_relayJob.attempts));
}

// ------------------------------- SIM health -------------------------------
static uint32_t simPollAt = 0;
static uint32_t simLastDiag = 0;
static bool simWasReg = false;
void sim_tick() {
  if (g_sim.passthroughActive) return;   // transparent mode: UART busy with TLS stream
  if (millis() < simPollAt) return;
  simPollAt = millis() + 15000;

  if (g_stats.simReady) {
    g_stats.simCsq = g_sim.csq();
    g_stats.simReg = g_sim.creg();
    if (simWasReg && !g_stats.simReg) {
      Serial.println("[sim] modem lost registration (power supply too weak?)");
    }
    simWasReg = g_stats.simReg;
    if (millis() - simLastDiag > 60000) {
      simLastDiag = millis();
      Serial.printf("[sim] csq=%d reg=%d up=%lus\n", (int)g_stats.simCsq, (int)g_stats.simReg, (unsigned long)(millis()/1000));
    }
  } else {
    bool ok = g_sim.at();
    if (ok) {
      g_stats.simReady = true;
      g_sim.setSmsPduMode();
      g_smsRx.begin();
      Serial.println("[sim] ready + SMS RX enabled");
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

  // The SIM800 draws ~2 A in short bursts (boot, network TX). Together with the
  // WiFi radio this can collapse the 3.3 V rail of the T-Call and corrupt the
  // ESP32 memory (heap assert / stack canary / WDT reset), so the modem is
  // powered and allowed to boot FIRST while the radio is still idle. The
  // access point comes up right after the modem finished booting.
  g_sim.begin(SIM_SERIAL, 115200, SIM800_RX_PIN, SIM800_TX_PIN, SIM800_PWR_PIN,
              SIM800_RST_PIN, SIM800_POWER_PIN);
  g_smsRx.setCallback(onFsoReply);
  wifi_init();
  WiFi.onEvent(onApStaEvent);
  web_setup();

  sim_tick();
  ws_setup();

  if (g_keyLen && ws_host.length() && g_cfg.jar_device_id.length()) {
    ws_armed = true;
    wsNextTry = millis() + 1500;
  }
  Serial.printf("[main] ready; key=%d bytes; ws_armed=%d\n", (int)g_keyLen, ws_armed);
}

void loop() {
  static uint32_t lastHb = 0;
  if (millis() - lastHb >= 1000) {
    lastHb = millis();
    Serial.printf("[hb] %s t=%lu heap=%u\n", g_step, (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
  }
  STEP("cli");   cli_tick();
  STEP("web");   web_handle();
  STEP("wifi");  wifi_tick();
  STEP("sim");   sim_tick();
  STEP("smsrx"); g_smsRx.tick();
  STEP("smsq");  sms_queue_tick();
  STEP("relay"); relay_queue_tick();
  STEP("ws");    ws_tick();
  delay(5);
}
