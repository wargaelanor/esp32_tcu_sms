#include "cfg.h"
#include "esp_random.h"

Config g_cfg;

static const char HEXD[] = "0123456789abcdef";

static String toHexSpaced(const uint8_t* b, size_t n, bool spacedGroup) {
  String s;
  for (size_t i = 0; i < n; i++) {
    if (spacedGroup && i && (i % 4) == 0) s += ' ';
    s += HEXD[(b[i] >> 4) & 0xF];
    s += HEXD[b[i] & 0xF];
  }
  return s;
}

String cfg_gen_hex(size_t nbytes) {
  uint8_t b[32];
  if (nbytes > sizeof(b)) nbytes = sizeof(b);
  esp_fill_random(b, nbytes);
  return toHexSpaced(b, nbytes, false);
}

String cfg_gen_key_spaced() {
  uint8_t b[16];
  esp_fill_random(b, 16);
  return toHexSpaced(b, 16, true);
}

bool cfg_ensure_generated_jar() {
  bool changed = false;
  if (!g_cfg.jar_device_id.length()) { g_cfg.jar_device_id = cfg_gen_hex(8); changed = true; }
  if (!g_cfg.jar_enc_key.length())    { g_cfg.jar_enc_key = cfg_gen_key_spaced(); changed = true; }
  if (changed) cfg_save();
  return changed;
}

static String trim(const String& s) {
  int a = 0, b = s.length();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) b--;
  return s.substring(a, b);
}

void cfg_load() {
  g_cfg = Config();
  if (!LittleFS.begin(true)) return;
  File f = LittleFS.open("/cfg.txt", "r");
  if (!f) { g_cfg.loaded = true; return; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = trim(line.substring(0, eq));
    String v = trim(line.substring(eq + 1));
    if      (k == "wifi_ssid")       g_cfg.wifi_ssid       = v;
    else if (k == "wifi_pass")       g_cfg.wifi_pass       = v;
else if (k == "ap_ssid")         g_cfg.ap_ssid = v;
    else if (k == "ap_pass")         g_cfg.ap_pass = v;
    else if (k == "ap_enable")       g_cfg.ap_enable = (v == "1");
    else if (k == "net_mode")        g_cfg.net_mode        = v;
    else if (k == "check_url")       g_cfg.check_url       = v;
    else if (k == "gprs_host")       g_cfg.gprs_host       = v;
    else if (k == "apn")             g_cfg.apn             = v;
    else if (k == "apn_user")        g_cfg.apn_user        = v;
    else if (k == "apn_pass")        g_cfg.apn_pass        = v;
    else if (k == "jar_device_id")   g_cfg.jar_device_id   = v;
    else if (k == "jar_enc_key")     g_cfg.jar_enc_key     = v;
    else if (k == "ws_url")          g_cfg.ws_url          = v;
    else if (k == "ws_autoreconnect") g_cfg.ws_autoreconnect = (v == "1");
  }
  f.close();
  g_cfg.loaded = true;
  cfg_ensure_generated_jar();   // first start: generate own device_id + key just like the JAR
}

bool cfg_save() {
  if (!LittleFS.begin(true)) return false;
  File f = LittleFS.open("/cfg.txt", "w");
  if (!f) return false;
  f.println("wifi_ssid="      + g_cfg.wifi_ssid);
  f.println("wifi_pass="      + g_cfg.wifi_pass);
  f.println("ap_ssid="        + g_cfg.ap_ssid);
  f.println("ap_pass="        + g_cfg.ap_pass);
  f.println("ap_enable="      + String(g_cfg.ap_enable ? "1" : "0"));
  f.println("net_mode="       + g_cfg.net_mode);
  f.println("check_url="      + g_cfg.check_url);
  f.println("gprs_host="      + g_cfg.gprs_host);
  f.println("apn="            + g_cfg.apn);
  f.println("apn_user="       + g_cfg.apn_user);
  f.println("apn_pass="       + g_cfg.apn_pass);
  f.println("jar_device_id="  + g_cfg.jar_device_id);
  f.println("jar_enc_key="    + g_cfg.jar_enc_key);
  f.println("ws_url="         + g_cfg.ws_url);
  f.println("ws_autoreconnect=" + String(g_cfg.ws_autoreconnect ? "1" : "0"));
  f.close();
  g_cfg.dirty = false;
  return true;
}

void cfg_update(const String& key, const String& val) {
  if      (key == "wifi_ssid")       g_cfg.wifi_ssid        = val;
  else if (key == "wifi_pass")       g_cfg.wifi_pass        = val;
  else if (key == "ap_ssid")         g_cfg.ap_ssid          = val;
  else if (key == "ap_pass")         g_cfg.ap_pass          = val;
  else if (key == "ap_enable")       g_cfg.ap_enable        = (val == "1");
  else if (key == "net_mode")        g_cfg.net_mode         = val;
  else if (key == "check_url")       g_cfg.check_url        = val;
  else if (key == "gprs_host")       g_cfg.gprs_host        = val;
  else if (key == "apn")             g_cfg.apn              = val;
  else if (key == "apn_user")        g_cfg.apn_user         = val;
  else if (key == "apn_pass")        g_cfg.apn_pass         = val;
  else if (key == "jar_device_id")   g_cfg.jar_device_id    = val;
  else if (key == "jar_enc_key")     g_cfg.jar_enc_key      = val;
  else if (key == "ws_url")          g_cfg.ws_url           = val;
  else if (key == "ws_autoreconnect") g_cfg.ws_autoreconnect = (val == "1");
  g_cfg.dirty = true;
}

String cfg_status() {
  return g_cfg.net_mode + "|" + (g_cfg.wifi_ssid.length() ? "sta" : "none") + "|" +
         (g_cfg.jar_device_id.length() ? g_cfg.jar_device_id : "-") + "|" +
         (g_cfg.jar_enc_key.length() ? "key:set" : "key:EMPTY");
}