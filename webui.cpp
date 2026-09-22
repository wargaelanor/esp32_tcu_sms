#include "webui.h"
#include "cfg.h"
#include "sim800.h"
#include "portal.h"
#include <WiFi.h>
#include <HTTPClient.h>

WebServer web(80);
Stats g_stats;

static const char PAGE_STATUS[] PROGMEM = R"HTML(<!DOCTYPE html><html lang=ru><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 TCU SMS — статус</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--bd:#30363d;--tx:#e6edf3;--mut:#8b949e;--grn:#3fb950;--red:#f85149;--amb:#d29922;--blu:#58a6ff;--btn2:#1f6feb}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--tx);margin:0;padding:16px 12px 96px}
.wrap{max-width:560px;margin:0 auto}
h1{font-size:19px;margin:0 0 2px}
.sub{color:var(--mut);font-size:12px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:14px;margin-bottom:12px}
.card h2{margin:0 0 4px;font-size:12px;color:var(--blu);text-transform:uppercase;letter-spacing:.6px}
.row{display:flex;justify-content:space-between;gap:10px;padding:6px 0;font-size:13.5px;border-bottom:1px dashed #242a33}
.row:last-child{border-bottom:0}
.k{color:var(--mut);flex:0 0 auto}.v{text-align:right;word-break:break-all}
.mut{color:var(--mut)}
.mono{font-family:ui-monospace,Consolas,monospace;font-size:12px}
.pill{display:inline-flex;align-items:center;gap:6px;border-radius:20px;padding:2px 10px;font-size:12px;white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
.ok{background:#12331d;color:var(--grn)}.ok .dot{background:var(--grn)}
.bad{background:#3a1215;color:var(--red)}.bad .dot{background:var(--red)}
.warn{background:#3a2d12;color:var(--amb)}.warn .dot{background:var(--amb)}
.actions{position:fixed;left:0;right:0;bottom:0;background:var(--bg);border-top:1px solid var(--bd);padding:10px 12px;display:flex;gap:8px;justify-content:center;flex-wrap:wrap;z-index:5}
button{position:relative;overflow:hidden;border:1px solid var(--bd);background:#21262d;color:var(--tx);border-radius:10px;padding:11px 14px;font-size:13.5px;cursor:pointer;transition:transform .08s,filter .12s,box-shadow .12s;font-family:inherit;touch-action:manipulation;-webkit-tap-highlight-color:transparent;user-select:none}
button:hover{filter:brightness(1.15)}
button:active{transform:translateY(2px) scale(.97);filter:brightness(.85);box-shadow:inset 0 2px 6px rgba(0,0,0,.5)}
button.pri{background:var(--btn2);border-color:#1f6feb}
button.danger{background:#8b1a1d;border-color:#a83538}
a{color:var(--blu);text-decoration:none}
.rip{position:absolute;border-radius:50%;background:rgba(255,255,255,.35);transform:scale(0);animation:rip .5s ease-out;pointer-events:none}
@keyframes rip{to{transform:scale(1);opacity:0}}
.overlay{position:fixed;inset:0;background:rgba(0,0,0,.6);backdrop-filter:blur(2px);display:none;z-index:10;align-items:center;justify-content:center;padding:16px}
.overlay.on{display:flex}
.modal{background:#1b2028;border:1px solid var(--bd);border-radius:16px;max-width:380px;width:100%;padding:18px}
.modal .ico{font-size:34px;text-align:center;margin-bottom:6px}
.modal h3{margin:0 0 8px;font-size:16px;text-align:center;color:var(--amb)}
.modal p{margin:0 0 16px;font-size:13.5px;line-height:1.55;color:#c9d1d9}
.modal .btns{display:flex;gap:8px}
.modal .btns button{flex:1}
</style>
</head><body><div class=wrap>
<h1>ESP32 TCU SMS</h1>
<div class=sub>Панель состояния устройства</div>
<div id=s><div class=card>Загрузка…</div></div>
</div>
<div class=actions>
<button id=bCheck>Проверить интернет</button>
<button id=bWs>Переподключить WS</button>
<button id=bNew>Новый ID / ключ</button>
<button id=bReboot class=danger>Перезагрузить</button>
<a href=/setup><button>Настройки</button></a>
</div>
<div class=overlay id=ov><div class=modal>
<div class=ico>&#9888;&#65039;</div>
<h3>Сгенерировать новый ID и ключ?</h3>
<p>Будет создана <b>новая пара «Device ID» и «Encryption key»</b>, а старые значения немедленно перестанут работать.<br><br>После перезагрузки устройства обязательно обновите <b>device_id</b> и <b>encryption_key</b> в <b>личном кабинете</b> на сайте — настройки автомобиля (SMS-конфигурация, provider=smsgateway).</p>
<div class=btns><button id=mNo>Отмена</button><button id=mYes class=danger>Сгенерировать и перезагрузить</button></div>
</div></div>
<script>
document.addEventListener('pointerdown',function(e){var b=e.target.closest('button');if(!b)return;var r=b.getBoundingClientRect();var s=document.createElement('span');s.className='rip';var d=Math.max(r.width,r.height);s.style.width=d+'px';s.style.height=d+'px';s.style.left=(e.clientX-r.left-d/2)+'px';s.style.top=(e.clientY-r.top-d/2)+'px';b.appendChild(s);setTimeout(function(){s.remove()},520)});
function cls(v){document.getElementById('s').innerHTML=v}
function card(t,rows){var h='<div class=card><h2>'+t+'</h2>';for(var i=0;i<rows.length;i++)h+='<div class=row><span class=k>'+rows[i][0]+'</span><span class=v>'+rows[i][1]+'</span></div>';return h+'</div>'}
function pill(c,t){return '<span class="pill '+c+'"><span class=dot></span>'+t+'</span>'}
async function ref(){try{
 var r=await fetch('/api/status');var j=await r.json();var h='';
 if(j.wifi.sta&&!j.internet.ok&&j.net_mode==='wifi')h+=card('Подсказка',[['Wi-Fi','<span class=mut>есть сеть, но интернет не проходит — проверьте check URL</span>']]);
 if(!j.wifi.sta&&j.wifi.ap)h+=card('Подсказка',[['Wi-Fi','<span class=mut>нет сети — откройте <a href=/setup><b>Настройки</b></a> и укажите SSID/пароль</span>']]);
 h+=card('Связь',[['WebSocket',j.ws.connected?pill('ok','подключено'):pill('warn',j.ws.state)],['Интернет',j.internet.ok?pill('ok','OK'):pill('bad','FAIL')]]);
 h+=card('Wi-Fi',[['STA',j.wifi.sta?('<span class=ok>'+j.wifi.ssid+'</span> <span class=mut>('+j.wifi.ip+', rssi '+j.wifi.rssi+')</span>'):pill('bad','нет')],['AP',j.wifi.ap?('<span class=ok>'+j.wifi.apssid+'</span> <span class=mut>('+j.wifi.apip+')</span>'):'<span class=mut>нет</span>']]);
 h+=card('SIM800 / GPRS',[['SIM800',j.sim.ready?('<span class=ok>AT OK</span> <span class=mut>CSQ '+j.sim.csq+', CET '+((j.sim.reg)?'да':'нет')+'</span>'):pill('bad','нет ответа')],['GPRS',j.gprs.up?('<span class=ok>'+j.gprs.ip+'</span>'):pill('warn','не поднят')]]);
 h+=card('Интернет (проверка)',[['Метод',j.internet.method],['Результат',j.internet.ok?('<span class=ok>OK</span> <span class=mut>'+j.internet.info+'</span>'):('<span class=bad>FAIL</span> <span class=mut>'+j.internet.info+'</span>')]]);
 h+=card('WebSocket',[['Состояние',j.ws.connected?pill('ok','connected'):pill('warn',j.ws.state)],['Переподключений',j.ws.reconnects]]);
 h+=card('Реквизиты (JAR)',[['Device ID','<span class=mono>'+j.jar.did+'</span>'],['Encryption key','<span class=mono>'+j.jar.ekey+'</span>']]);
 h+=card('SMS',[['Отправлено','<span class=ok>'+j.sms.ok+'</span> ок / <span class=bad>'+j.sms.fail+'</span> ош.'],['Последняя',j.sms.last]]);
 h+='<div class=card><div class=row><span class=k>Прошивка</span><span class=v>'+j.fw+'</span></div></div>';
 cls(h);
 setTimeout(ref,2500);
}catch(e){cls('<div class=card>Ошибка связи с устройством: '+e+'</div>');setTimeout(ref,3000)}}
ref();
document.getElementById('bCheck').onclick=function(){fetch('/api/action?x=check').then(ref)};
document.getElementById('bWs').onclick=function(){fetch('/api/action?x=restart_ws').then(ref)};
document.getElementById('bReboot').onclick=function(){fetch('/api/action?x=reboot')};
var ov=document.getElementById('ov');
document.getElementById('bNew').onclick=function(){ov.classList.add('on')};
document.getElementById('mNo').onclick=function(){ov.classList.remove('on')};
document.getElementById('mYes').onclick=function(){var b=document.getElementById('mYes');b.textContent='Перезагрузка…';fetch('/api/action?x=regen_keys');b.style.opacity=.6};
</script>
</html>)HTML";

static const char PAGE_SETUP[] PROGMEM = R"HTML(<!DOCTYPE html><html lang=ru><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 TCU SMS — настройки</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--bd:#30363d;--tx:#e6edf3;--mut:#8b949e;--blu:#58a6ff;--btn2:#1f6feb}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--tx);margin:0;padding:16px 12px}
.wrap{max-width:560px;margin:0 auto}
h1{font-size:19px;margin:0 0 2px}
.sub{color:var(--mut);font-size:12px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:14px;margin-bottom:12px}
.card h2{margin:0 0 10px;font-size:12px;color:var(--blu);text-transform:uppercase;letter-spacing:.6px}
label{display:block;margin:8px 0 2px;font-size:12.5px;color:var(--mut)}
input[type=text],input[type=password],select{width:100%;background:#0d1117;border:1px solid var(--bd);border-radius:8px;color:var(--tx);padding:8px;box-sizing:border-box;font-size:13.5px}
input:focus,select:focus{outline:none;border-color:var(--btn2)}
select option{background:#161b22}
.ro{display:flex;gap:8px}.ro input{flex:1;font-family:ui-monospace,Consolas,monospace;font-size:12.5px;background:#0b0e13;color:var(--blu)}
.cpy{flex:0 0 auto;padding:0 12px}
.hint{font-size:12px;color:var(--mut);line-height:1.5;margin:0 0 6px}
.mut{color:var(--mut)}
a{color:var(--blu);text-decoration:none}
button{position:relative;overflow:hidden;border:1px solid var(--bd);background:#21262d;color:var(--tx);border-radius:10px;padding:11px 14px;font-size:13.5px;cursor:pointer;transition:transform .08s,filter .12s,box-shadow .12s;font-family:inherit;touch-action:manipulation;-webkit-tap-highlight-color:transparent;user-select:none}
button:hover{filter:brightness(1.15)}
button:active{transform:translateY(2px) scale(.97);filter:brightness(.85);box-shadow:inset 0 2px 6px rgba(0,0,0,.5)}
button.pri{background:var(--btn2);border-color:#1f6feb;width:100%;padding:13px;margin-top:4px}
.rip{position:absolute;border-radius:50%;background:rgba(255,255,255,.35);transform:scale(0);animation:rip .5s ease-out;pointer-events:none}
@keyframes rip{to{transform:scale(1);opacity:0}}
</style>
</head><body><div class=wrap>
<h1>Настройки устройства</h1>
<div class=sub>Заполните поля и нажмите «Сохранить и перезагрузить»</div>
<p><a href=/><button>← Назад</button></a></p>
<form method=POST action=/save>
<div class=card><h2>Wi-Fi (точка доступа)</h2>
<label>SSID (домашняя сеть, только 2.4 ГГц)</label><input name=wifi_ssid value="%ssid%">
<label>Пароль</label><input name=wifi_pass type=password value="%pass%">
<label>SSID точки доступа для настройки</label><input name=ap_ssid value="%apssid%">
<label>Пароль точки доступа</label><input name=ap_pass type=password value="%appass%">
</div>
<div class=card><h2>Интернет</h2>
<label>Способ связи</label>
<select name=net_mode onchange="nm(this.value)">
<option value=wifi %wn%>Wi-Fi</option>
<option value=gprs %gn%>SIM-карта (GPRS)</option>
</select>
<div id=wifiopt>
<label>Wi-Fi проверка: URL (нужен HTTP 200)</label><input name=check_url value="%checkurl%">
</div>
<div id=gprsopt>
<label>GPRS проверка: хост для пинга</label><input name=gprs_host value="%gprshost%">
<label>APN (точка доступа оператора)</label><input name=apn value="%apn%">
<label>APN логин</label><input name=apn_user value="%apnuser%">
<label>APN пароль</label><input name=apn_pass type=password value="%apnpass%">
</div>
</div>
<div class=card><h2>WebSocket relay (JAR)</h2>
<p class=hint>Device ID и Encryption key генерируются автоматически и не редактируются. Их нужно внести в настройки автомобиля в OpenCARWINGS (sms_config, provider=smsgateway). Новый ID/ключ создаются кнопкой на странице статуса.</p>
<label>Device ID</label>
<div class=ro><input name=jar_device_id id=did readonly value="%did%"><button type=button class=cpy onclick="copyVal(this,'did')">Копировать</button></div>
<label>Encryption key (hex)</label>
<div class=ro><input name=jar_enc_key id=ekey readonly value="%ekey%"><button type=button class=cpy onclick="copyVal(this,'ekey')">Копировать</button></div>
<label>WebSocket URL</label><input name=ws_url value="%wsurl%">
<label><input type=checkbox name=ws_autoreconnect value=1 %ar%> Авто-переподключение WebSocket</label>
</div>
<button class=pri type=submit>Сохранить и перезагрузить</button>
</form>
</div>
<script>
function nm(v){document.getElementById('wifiopt').style.display=(v==='wifi')?'':'none';
document.getElementById('gprsopt').style.display=(v==='gprs')?'':'none'}
nm(document.querySelector('select[name=net_mode]').value);
function copyVal(b,id){
var t=document.getElementById(id);t.focus();t.select();t.setSelectionRange(0,99999);
var ok=false;try{ok=document.execCommand('copy');if(ok)window.getSelection().removeAllRanges()}catch(e){}
if(!ok&&navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t.value)}
var o=b.textContent;b.textContent=ok?'Скопировано':'Выделено';setTimeout(function(){b.textContent=o},1500)}
document.addEventListener('pointerdown',function(e){var b=e.target.closest('button');if(!b)return;var r=b.getBoundingClientRect();var s=document.createElement('span');s.className='rip';var d=Math.max(r.width,r.height);s.style.width=d+'px';s.style.height=d+'px';s.style.left=(e.clientX-r.left-d/2)+'px';s.style.top=(e.clientY-r.top-d/2)+'px';b.appendChild(s);setTimeout(function(){s.remove()},520)});
</script>
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

void web_handle() {
  portal_tick();
  web.handleClient();
}

const char* FW_VER = "1.9";

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
      g_stats.internetInfo = "WiFi not connected";
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
  cfg_ensure_generated_jar();   // empty fields -> generate like on a fresh PC
  cfg_save();
  web.send(200, "text/html", "<html><body>Saved. Rebooting...</body></html>");
  delay(150);
  ESP.restart();
}

static void handle_root() { web.send_P(200, "text/html", PAGE_STATUS); }
static void handle_setup() { web.send(200, "text/html", page_setup()); }

// Captive portal: any request whose Host is not the device itself comes from
// an OS connectivity probe (after DNS answered with our AP IP) — 302 it to the
// status page so the page opens automatically on connect.
static bool captive_redirect() {
  if (!g_cfg.ap_enable) return false;
  String host = web.hostHeader();
  String ap   = WiFi.softAPIP().toString();
  String sta  = WiFi.localIP().toString();
  if (host.length() && host != ap && host != sta) {
    Serial.printf("[portal] captive probe Host=%s, uri=%s -> /\n",
                  host.c_str(), web.uri().c_str());
    String url = "http://" + ap + "/";
    web.sendHeader("Location", url, true);
    web.send(302, "text/html",
             "<!DOCTYPE html><html><head><meta http-equiv=refresh "
             "content=\"0; url=" + url + "\"></head><body>Redirecting to "
             "<a href=\"" + url + "\">" + url + "</a></body></html>");
    return true;
  }
  return false;
}

static void handle_captive_root() {
  if (captive_redirect()) return;
  handle_root();
}

static void handle_captive_setup() {
  if (captive_redirect()) return;
  handle_setup();
}

void web_setup() {
  portal_setup();
  web.on("/", handle_captive_root);
  web.on("/setup", handle_captive_setup);
  web.on("/save", HTTP_POST, handle_save);
  web.on("/api/status", handle_status);
  web.on("/api/action", handle_action);
  web.onNotFound([]() {
    if (captive_redirect()) return;
    web.sendHeader("Location", "/setup", true);
    web.send(302, "", "");
  });
  web.begin();
}