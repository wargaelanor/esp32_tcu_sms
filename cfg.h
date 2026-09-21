#pragma once
#include <Arduino.h>
#include <LittleFS.h>

// Line-based key=value config stored in LittleFS /cfg.txt
struct Config {
  // WiFi
  String wifi_ssid;
  String wifi_pass;
  String ap_ssid = "ESP32-TCU-SMS";
  String ap_pass = "tcu12345";
  bool   ap_enable = true;
  // Internet: "wifi" or "gprs"
  String net_mode = "wifi";
  String check_url = "http://138.16.224.7/";
  String gprs_host = "138.16.224.7";
  // GPRS APN
  String apn = "internet";
  String apn_user;
  String apn_pass;
  // JAR/relay fields
  String jar_device_id;
  String jar_enc_key;      // hex, may contain spaces, 16/24/32 bytes
  String ws_url = "ws://138.16.224.7/ws/smsgateway/";
  bool   ws_autoreconnect = true;
  // Stats helpers
  bool   loaded = false;
  bool   dirty  = false;
};

extern Config g_cfg;

void cfg_load();
bool cfg_save();
void cfg_update(const String& key, const String& val);
String cfg_status();

// JAR-like auto-generation of device_id (8 random bytes hex) and encryption key
// (16 random bytes, AES-128, spaced like JAR). Returns true if anything was set.
bool cfg_ensure_generated_jar();
String cfg_gen_hex(size_t nbytes);        // lowercase hex, no spaces
String cfg_gen_key_spaced();              // 16 bytes -> "XXXX XXXX XXXX XXXX"