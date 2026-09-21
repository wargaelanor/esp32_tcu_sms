#include "webui.h"
#include "cfg.h"
#include "sim800.h"
#include <WiFi.h>
#include <HTTPClient.h>

WebServer web(80);
Stats g_stats;

static const char PAGE_STATUS[] PROGMEM = R"HTML(<!DOCTYPE html><html lang=ru><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 TCU SMS</title>
<style>body{font-family:monospace;max-width:640px;margin:12px auto;padding:0 8px;background:#111;color:#ddd}
h1{font-size:18px}table{width:100%;border-collapse:collapse;font-size:13px}
td,th{border:1px solid #333;padding:4px 6px;text-align:left}
.ok{color:#6f6}.bad{color:#f66}.warn{color:#fc6}
button{background:#233;color:#fff;border:1px solid #466;padding:8px 12px;margin:4px;font-size:13px}
button:active{background:#344}</style>
<h1>ESP32 TCU SMS — статус</h1>
<div id=s>Загрузка…</div>
<p>
<button onclick="fetch('/api/action?x=check')">Проверить интернет</button>
<button onclick="if(confirm('Сгенерировать новые Device ID и ключ?'))fetch('/api/action?x=regen_keys')">Новый ID/ключ</button>
<button onclick="fetch('/api/action?x=restart_ws')">Переподключить WS</button>
<button onclick="fetch('/api/action?x=reboot')">REBOOT</button>
<a href=/setup><button>Настройка</button></a>
</p>
<script>
function cls(v){var el=document.getElementById('s');el.innerHTML=v}
async function ref(){try{
 var r=await fetch('/api/status');var j=await r.json();
var h='<table>';
  function row(k,v){return '<tr><td>'+k+'</td><td>'+v+'</td></tr>'}
  h+=row('Прошивка', j.fw);
  h+=row('Интернет через', j.net_mode);
  if(j.wifi.sta && !j.internet.ok && j.net_mode==='wifi') h+=row('Подсказка','<span class=warn>Есть WiFi, нет интернета — проверь URL проверки</span>');
  if(!j.wifi.sta && j.wifi.ap) h+=row('Подсказка','<span class=warn>WiFi не подключён: открой <a href=/setup><b>Настройка</b></a>, впиши SSID и пароль своей сети</span>');
 h+=row('WiFi STA', j.wifi.sta? '<span class=ok>'+j.wifi.ssid+'</span> ('+j.wifi.ip+', rssi '+j.wifi.rssi+')':'<span class=bad>нет</span>');
 h+=row('WiFi AP', j.wifi.ap? '<span class=ok>'+j.wifi.apssid+'</span> ('+j.wifi.apip+')':'нет');
 h+=row('SIM800', j.sim.ready? '<span class=ok>AT OK</span>, CSQ '+j.sim.csq+', CET '+((j.sim.reg)?'да':'нет'):'<span class=bad>нет ответа</span>');
 h+=row('GPRS', j.gprs.up? '<span class=ok>'+j.gprs.ip+'</span>':'<span class=warn>down</span>');
 h+=row('Интернет (проверка)', j.internet.ok? '<span class=ok>OK</span> '+j.internet.info:'<span class=bad>FAIL</span> '+j.internet.info);
h+=row('WebSocket', j.ws.connected? '<span class=ok>connected</span>':'<span class=warn>'+j.ws.state+'</span>');
  h+='<tr><td>WS переподключений</td><td>'+j.ws.reconnects+'</td></tr>';
  h+=row('Device ID', j.jar.did);
  h+=row('Encryption key', j.jar.ekey);
  h+=row('SMS отправлено', '<span class=ok>'+j.sms.ok+'</span> ок / <span class=bad>'+j.sms.fail+'</span> ошиб');
 h+=row('Последняя SMS', j.sms.last);
 h+='</table>';
 cls(h);
 setTimeout(ref, 2500);
}catch(e){cls('ошибка связи с ESP32: '+e);setTimeout(ref,3000)}}
ref();
</script>
</html>)HTML";

static const char PAGE_SETUP[] PROGMEM = R"HTML(<!DOCTYPE html><html lang=ru><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 TCU SMS — настройка</title>
<style>body{font-family:monospace;max-width:640px;margin:12px auto;background:#111;color:#ddd}
h2{font-size:16px;border-top:1px solid #333;padding-top:8px}
label{display:block;margin:6px 0 2px;font-size:12px;color:#9bc}
input{width:100%;background:#1c1c1c;border:1px solid #333;color:#eee;padding:6px;box-sizing:border-box}
select{width:100%;background:#1c1c1c;border:1px solid #333;color:#eee;padding:6px}
button{background:#233;border:1px solid #466;color:#fff;padding:10px 16px;margin-top:12px;font-size:14px}</style>
<script>
function nm(v){document.getElementById('wifiopt').style.display=(v==='wifi')?'':'none';
document.getElementById('gprsopt').style.display=(v==='gprs')?'':'none'}
</script>
<form method=POST action=/save>
<h2>WiFi</h2>
<label>SSID</label><input name=wifi_ssid value="%ssid%">
<label>Пароль</label><input name=wifi_pass value="%pass%">
<label>AP SSID (конфиг-режим)</label><input name=ap_ssid value="%apssid%">
<label>AP пароль</label><input name=ap_pass value="%appass%">
<h2>Интернет</h2>
<label>Способ подключения к интернету</label>
<select name=net_mode onchange="nm(this.value)">
<option value=wifi %wn%>WiFi</option>
<option value=gprs %gn%>SIM-карта (GPRS)</option>
</select>
<div id="wifiopt">
<label>Проверка WiFi: URL (GET, ожидается HTTP 200)</label><input name=check_url value="%checkurl%">
</div>
<div id="gprsopt">
<label>Проверка GPRS: хост ping (AT+CIPPING)</label><input name=gprs_host value="%gprshost%">
<label>APN (точка доступа)</label><input name=apn value="%apn%">
<label>APN user</label><input name=apn_user value="%apnuser%">
<label>APN pass</label><input name=apn_pass value="%apnpass%">
</div>
<h2>Поля как в JAR (WebSocket relay)</h2>
<p style="font-size:12px;color:#9bc">Device ID и Encryption key генерируются автоматически при первом старте (как на новом компьютере с JAR). Их нужно прописать в настройках машины в OpenCARWINGS (sms_config, provider=smsgateway). Нельзя указывать чужие значения — у каждого устройства пара уникальна.</p>
<label>Device ID</label><input name=jar_device_id value="%did%">
<label>Encryption key (hex, можно с пробелами)</label><input name=jar_enc_key value="%ekey%">
<label>WebSocket URL</label><input name=ws_url value="%wsurl%">
<label><input type=checkbox name=ws_autoreconnect value=1 %ar%> Auto-reconnect WebSocket</label>
<p><button type=submit>Сохранить и перезагрузить</button></p>
</form>
</html>)HTML";

static String page_setup() {
  String p = FPSTR(PAGE_SETUP);
  p.replace("%ssid%",    g_cfg.wifi_ssid);
  p.replace("%pass%",    g_cfg.wifi_pass);
  p.replace("%apssid%",  g_cfg.ap_ssid);
  p.replace("%appass%",  g_cfg.ap_pass);
  p.replace("%wn%",      g_cfg.net_mode == "wifi" ? "selected" : "");
  p.replace("%gn%",      g_cfg.net_mode == "gprs" ? "selected" : "");
  p.replace("%checkurl%",g_cfg.check_url);
  p.replace("%gprshost%",g_cfg.gprs_host);
  p.replace("%apn%",     g_cfg.apn);
  p.replace("%apnuser%", g_cfg.apn_user);
  p.replace("%apnpass%", g_cfg.apn_pass);
  p.replace("%did%",     g_cfg.jar_device_id);
  p.replace("%ekey%",    g_cfg.jar_enc_key);
  p.replace("%wsurl%",   g_cfg.ws_url);
  p.replace("%ar%",      g_cfg.ws_autoreconnect ? "checked" : "");
  return p;
}

static String jesc(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

static String wifi_status_json() {
  String apip = WiFi.softAPIP().toString();
  String o = "{";
  o += "\"sta\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  o += "\"ssid\":\"" + jesc(g_cfg.wifi_ssid) + "\",";
  o += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
  o += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -999) + ",";
  o += "\"ap\":" + String(WiFi.softAPgetStationNum() > 0 ? "true" : "false") + ",";
  o += "\"apssid\":\"" + jesc(g_cfg.ap_ssid) + "\",";
  o += "\"apip\":\"" + apip + "\"";
  o += "}";
  return o;
}

void web_handle() { web.handleClient(); }

const char* FW_VER = "1.5";

void web_default_behavior() {}

static void handle_status() {
  String o = "{";
  o += "\"fw\":\"" + String(FW_VER) + "\",";
  o += "\"net_mode\":\"" + g_cfg.net_mode + "\",";
  o += "\"wifi\":" + wifi_status_json() + ",";
  o += "\"sim\":{\"ready\":" + String(g_stats.simReady ? "true" : "false") +
       ",\"csq\":" + String(g_stats.simCsq) +
       ",\"reg\":" + String(g_stats.simReg ? "true" : "false") + "},";
  o += "\"gprs\":{\"up\":" + String(g_stats.gprsUp ? "true" : "false") +
       ",\"ip\":\"" + jesc(g_stats.gprsIp) + "\"},";
  o += "\"internet\":{\"ok\":" + String(g_stats.internetOk ? "true" : "false") +
       ",\"method\":\"" + g_stats.internetMethod + "\",\"info\":\"" + jesc(g_stats.internetInfo) + "\"},";
  o += "\"ws\":{\"connected\":" + String(g_stats.wsConnected ? "true" : "false") +
       ",\"state\":\"" + g_stats.wsState + "\",\"reconnects\":" + String(g_stats.wsReconnects) + "},";
  o += "\"jar\":{\"did\":\"" + jesc(g_cfg.jar_device_id) + "\",\"ekey\":\"" + jesc(g_cfg.jar_enc_key) + "\"},";
  o += "\"sms\":{\"ok\":" + String(g_stats.smsOk) +
       ",\"fail\":" + String(g_stats.smsFail) +
       ",\"last\":\"" + jesc(g_stats.smsLast) + "\"}";
  o += "}";
  web.send(200, "application/json", o);
}

void internet_check_now() {
  g_stats.internetInfo = "...";
  if (g_cfg.net_mode == "gprs") {
    g_stats.internetMethod = "gprs";
    int sent = 0, rcvd = 0, rtt = 0;
    uint32_t t0 = millis();
    bool ok = g_sim.pingHost(g_cfg.gprs_host, &sent, &rcvd, &rtt);
    if (ok) {
      g_stats.internetInfo = "CIPPING " + g_cfg.gprs_host + " +" + String(rcvd) + "/" + String(sent) + " rtt " + String(rtt) + "ms (" + String(millis() - t0) + "ms)";
      g_stats.internetOk = true;
    } else {
      g_stats.internetInfo = "CIPPING " + g_cfg.gprs_host + " FAIL";
      g_stats.internetOk = false;
    }
  } else {
    g_stats.internetMethod = "wifi";
    if (WiFi.status() != WL_CONNECTED) {
      g_stats.internetOk = false;
      g_stats.internetInfo = "WiFi не подключен";
      return;
    }
    HTTPClient http;
    web.handleClient();
    http.setTimeout(5000);
    http.begin(g_cfg.check_url);
    uint32_t t0 = millis();
    int code = http.GET();
    if (code > 0) {
      g_stats.internetOk = (code == HTTP_CODE_OK);
      g_stats.internetInfo = "HTTP " + String(code) + " (" + String(millis() - t0) + "ms) " + g_cfg.check_url;
    } else {
      g_stats.internetOk = false;
      g_stats.internetInfo = "GET error " + String(code);
    }
    http.end();
  }
}

static void handle_action() {
  String x = web.arg("x");
  if (x == "check")          internet_check_now();
  else if (x == "restart_ws") { extern void ws_force_reconnect(); ws_force_reconnect(); }
  else if (x == "regen_keys") {
    g_cfg.jar_device_id = cfg_gen_hex(8);
    g_cfg.jar_enc_key = cfg_gen_key_spaced();
    cfg_save();
    web.send(200, "text/plain", "ok");
    delay(100);
    ESP.restart();
  }
  else if (x == "reboot")    ESP.restart();
  web.send(200, "text/plain", "ok");
}

static void handle_save() {
  for (int i = 0; i < web.args(); i++) {
    cfg_update(web.argName(i), web.arg(i));
  }
  cfg_ensure_generated_jar();   // пустые поля -> сгенерировать как на новом компе
  cfg_save();
  web.send(200, "text/html", "<html><body>Сохранено. Перезагрузка...</body></html>");
  delay(150);
  ESP.restart();
}

static void handle_root() { web.send_P(200, "text/html", PAGE_STATUS); }
static void handle_setup() { web.send(200, "text/html", page_setup()); }

void web_setup() {
  web.on("/", handle_root);
  web.on("/setup", handle_setup);
  web.on("/save", HTTP_POST, handle_save);
  web.on("/api/status", handle_status);
  web.on("/api/action", handle_action);
  web.onNotFound([]() {
    web.sendHeader("Location", "/setup", true);
    web.send(302, "", "");
  });
  web.begin();
}