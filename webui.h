#pragma once
#include <Arduino.h>
#include <WebServer.h>

struct Stats {
  uint32_t wsConnects = 0;
  uint32_t wsReconnects = 0;
  bool     wsConnected = false;
  String   wsState = "idle";
  String   wsLastErr = "";
  uint32_t smsOk = 0;
  uint32_t smsFail = 0;
  String   smsLast = "";
  uint32_t lastSmsTs = 0;
  uint32_t simInitAttempts = 0;
  bool     simReady = false;
  int      simCsq = -1;
  bool     simReg = false;
  bool     gprsUp = false;
  String   gprsIp = "";
  bool     internetOk = false;
  String   internetMethod = "-";
  String   internetInfo = "-";
};

extern Stats g_stats;

// Runs callback; kept inline so webui doesn't depend on app globals.
void web_default_behavior();

extern WebServer web;

void web_setup();
void web_handle();

// Internet availability check (blocking). Fills stats.
void internet_check_now();

extern const char* FW_VER;