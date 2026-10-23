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
<div class=sub><span class=i>Панель состояния устройства</span> · <button type=button id=bLang style="padding:1px 10px;font-size:12px;border-radius:20px">English</button></div>
<div id=s><div class=card>Загрузка…</div></div>
</div>
<div class=actions>
<button id=bCheck class=i>Проверить интернет</button>
<button id=bWs class=i>Переподключить WS</button>
<button id=bNew class=i>Новый ID / ключ</button>
<button id=bReboot class="danger i">Перезагрузить</button>
<a href=/setup><button class=i>Настройки</button></a>
</div>
<div class=overlay id=ov><div class=modal>
<div class=ico>&#9888;&#65039;</div>
<h3 class=i>Сгенерировать новый ID и ключ?</h3>
<p class=i>Будет создана <b>новая пара «Device ID» и «Encryption key»</b>, а старые значения немедленно перестанут работать.<br><br>После перезагрузки устройства обязательно обновите <b>device_id</b> и <b>encryption_key</b> в <b>личном кабинете</b> на сайте — настройки автомобиля (SMS-конфигурация, provider=smsgateway).</p>
<div class=btns><button id=mNo class=i>Отмена</button><button id=mYes class="danger i">Сгенерировать и перезагрузить</button></div>
</div></div>
<script>
var E={
'Панель состояния устройства':'Device status panel',
'ESP32 TCU SMS — статус':'ESP32 TCU SMS — status',
'Проверить интернет':'Check internet','Переподключить WS':'Reconnect WS','Новый ID / ключ':'New ID / key','Перезагрузить':'Reboot','Настройки':'Settings',
'Сгенерировать новый ID и ключ?':'Generate new ID and key?',
'Будет создана <b>новая пара «Device ID» и «Encryption key»</b>, а старые значения немедленно перестанут работать.<br><br>После перезагрузки устройства обязательно обновите <b>device_id</b> и <b>encryption_key</b> в <b>личном кабинете</b> на сайте — настройки автомобиля (SMS-конфигурация, provider=smsgateway).':'A new <b>Device ID</b> / <b>Encryption key</b> pair will be created and the old values will stop working immediately.<br><br>After the device reboots, update <b>device_id</b> and <b>encryption_key</b> in the <b>personal cabinet</b> on the site — vehicle settings (SMS config, provider=smsgateway).',
'Отмена':'Cancel','Сгенерировать и перезагрузить':'Generate & reboot','Перезагрузка…':'Rebooting…',
'Ошибка связи с устройством: ':'Device unreachable: ',
'Подсказка':'Hint','Связь':'Link','Интернет':'Internet','Метод':'Method','Результат':'Result','Состояние':'State','Переподключений':'Reconnects',
'Реквизиты (JAR)':'Credentials (JAR)','Отправлено':'Sent','Последняя':'Last','От сервера':'From server','В SMS (CMGS)':'To SMS (CMGS)','Буфер':'Buffer','Прошивка':'Firmware',
'подключено':'connected','нет':'none','да':'yes','нет ответа':'no reply','не поднят':'not up',
'есть сеть, но интернет не проходит — проверьте check URL':'network OK, but internet fails — check the check URL',
'нет сети — откройте <a href=/setup><b>Настройки</b></a> и укажите SSID/пароль':'no network — open <a href=/setup><b>Settings</b></a> and enter SSID/password',
'сообщение хранится до прихода следующего':'message is kept until the next one arrives',
'ок':'ok','ош.':'err'
};
var orig=new Map();var origT=document.title;
function lang(){var s=localStorage.getItem('lang');if(s==='ru'||s==='en')return s;var l=(navigator.language||'').toLowerCase();return(l.indexOf('ru')===0||l.indexOf('uk')===0||l.indexOf('be')===0)?'ru':'en'}
function t(s){var e=E[s];return lang()==='en'&&e?e:s}
function cacheStatic(){document.querySelectorAll('.i').forEach(function(e){if(!orig.has(e))orig.set(e,e.innerHTML)})}
function applyI18n(){cacheStatic();document.documentElement.lang=lang;
 if(lang()==='en'){document.title=E[origT]||origT;document.querySelectorAll('.i').forEach(function(e){var o=orig.get(e);if(o&&E[o])e.innerHTML=E[o]})}
 else{document.title=origT;document.querySelectorAll('.i').forEach(function(e){var o=orig.get(e);if(o)e.innerHTML=o})}}
function setLangBtn(){var b=document.getElementById('bLang');if(b)b.textContent=lang()==='ru'?'English':'Русский'}
document.addEventListener('pointerdown',function(e){var b=e.target.closest('button');if(!b)return;var r=b.getBoundingClientRect();var s=document.createElement('span');s.className='rip';var d=Math.max(r.width,r.height);s.style.width=d+'px';s.style.height=d+'px';s.style.left=(e.clientX-r.left-d/2)+'px';s.style.top=(e.clientY-r.top-d/2)+'px';b.appendChild(s);setTimeout(function(){s.remove()},520)});
function cls(v){document.getElementById('s').innerHTML=v}
function short(v,n){v=String(v);return v.length<=n?v:v.slice(0,n)+'…'}
function card(t,rows){var h='<div class=card><h2>'+t+'</h2>';for(var i=0;i<rows.length;i++)h+='<div class=row><span class=k>'+rows[i][0]+'</span><span class=v>'+rows[i][1]+'</span></div>';return h+'</div>'}
function pill(c,t){return '<span class="pill '+c+'"><span class=dot></span>'+t+'</span>'}
async function ref(){try{
 var r=await fetch('/api/status');var j=await r.json();var h='';
 if(j.wifi.sta&&!j.internet.ok&&j.net_mode==='wifi')h+=card(t('Подсказка'),[['Wi-Fi','<span class=mut>'+t('есть сеть, но интернет не проходит — проверьте check URL')+'</span>']]);
 if(!j.wifi.sta&&j.wifi.ap)h+=card(t('Подсказка'),[['Wi-Fi','<span class=mut>'+t('нет сети — откройте <a href=/setup><b>Настройки</b></a> и укажите SSID/пароль')+'</span>']]);
 h+=card(t('Связь'),[['WebSocket',j.ws.connected?pill('ok',t('подключено')):pill('warn',j.ws.state)],['Интернет',j.internet.ok?pill('ok','OK'):pill('bad','FAIL')]]);
 h+=card('Wi-Fi',[['STA',j.wifi.sta?('<span class=ok>'+j.wifi.ssid+'</span> <span class=mut>('+j.wifi.ip+', rssi '+j.wifi.rssi+')</span>'):pill('bad',t('нет'))],['AP',j.wifi.ap?('<span class=ok>'+j.wifi.apssid+'</span> <span class=mut>('+j.wifi.apip+')</span>'):'<span class=mut>'+t('нет')+'</span>']]);
 h+=card('SIM800 / GPRS',[['SIM800',j.sim.ready?('<span class=ok>AT OK</span> <span class=mut>CSQ '+j.sim.csq+', '+t(j.sim.reg?'да':'нет')+'</span>'):pill('bad',t('нет ответа'))],['GPRS',j.gprs.up?('<span class=ok>'+j.gprs.ip+'</span>'):pill('warn',t('не поднят'))]]);
 h+=card(t('Интернет (проверка)'),[[t('Метод'),j.internet.method],[t('Результат'),j.internet.ok?('<span class=ok>OK</span> <span class=mut>'+j.internet.info+'</span>'):('<span class=bad>FAIL</span> <span class=mut>'+j.internet.info+'</span>')]]);
 h+=card('WebSocket',[[t('Состояние'),j.ws.connected?pill('ok','connected'):pill('warn',j.ws.state)],[t('Переподключений'),j.ws.reconnects]]);
 h+=card(t('Реквизиты (JAR)'),[['Device ID','<span class=mono>'+j.jar.did+'</span>'],['Encryption key','<span class=mono>'+j.jar.ekey+'</span>']]);
 h+=card('SMS',[[t('Отправлено'),'<span class=ok>'+j.sms.ok+'</span> '+t('ок')+' / <span class=bad>'+j.sms.fail+'</span> '+t('ош.')],[t('Последняя'),j.sms.last],[t('От сервера'),'<span class=mono>'+short(j.sms.in,90)+'</span>'],[t('В SMS (CMGS)'),'<span class=mono>'+short(j.sms.out,90)+'</span>'],[t('Буфер'),'<span class=mut>'+t('сообщение хранится до прихода следующего')+'</span>']]);
 h+='<div class=card><div class=row><span class=k>'+t('Прошивка')+'</span><span class=v>'+j.fw+'</span></div></div>';
 cls(h);
 setTimeout(ref,2500);
}catch(e){cls('<div class=card>'+t('Ошибка связи с устройством: ')+e+'</div>');setTimeout(ref,3000)}}
ref();
document.getElementById('bLang').onclick=function(){localStorage.setItem('lang',lang()==='ru'?'en':'ru');applyI18n();setLangBtn();ref()};
applyI18n();setLangBtn();
document.getElementById('bCheck').onclick=function(){fetch('/api/action?x=check').then(ref)};
document.getElementById('bWs').onclick=function(){fetch('/api/action?x=restart_ws').then(ref)};
document.getElementById('bReboot').onclick=function(){fetch('/api/action?x=reboot')};
var ov=document.getElementById('ov');
document.getElementById('bNew').onclick=function(){ov.classList.add('on')};
document.getElementById('mNo').onclick=function(){ov.classList.remove('on')};
document.getElementById('mYes').onclick=function(){var b=document.getElementById('mYes');b.textContent=t('Перезагрузка…');fetch('/api/action?x=regen_keys');b.style.opacity=.6};
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
<h1 class=i>Настройки устройства</h1>
<div class=sub><span class=i>Заполните поля и нажмите «Сохранить и перезагрузить»</span> · <button type=button id=bLang style="padding:1px 10px;font-size:12px;border-radius:20px">Русский</button></div>
<p><a href=/><button class=i>← Назад</button></a></p>
<form method=POST action=/save>
<div class=card><h2 class=i>Wi-Fi (точка доступа)</h2>
<label class=i>SSID (домашняя сеть, только 2.4 ГГц)</label><input name=wifi_ssid value="%ssid%">
<label class=i>Пароль</label><input name=wifi_pass type=password value="%pass%">
<label class=i>SSID точки доступа для настройки</label><input name=ap_ssid value="%apssid%">
<label class=i>Пароль точки доступа</label><input name=ap_pass type=password value="%appass%">
</div>
<div class=card><h2 class=i>Интернет</h2>
<label class=i>Способ связи</label>
<select name=net_mode onchange="nm(this.value)">
<option value=wifi %wn% class=i>Wi-Fi</option>
<option value=gprs %gn% class=i>SIM-карта (GPRS)</option>
</select>
<div id=wifiopt>
<label class=i>Wi-Fi проверка: URL (нужен HTTP 200)</label><input name=check_url value="%checkurl%">
</div>
<div id=gprsopt>
<label class=i>GPRS проверка: хост для пинга</label><input name=gprs_host value="%gprshost%">
<label class=i>APN (точка доступа оператора)</label><input name=apn value="%apn%">
<label class=i>APN логин</label><input name=apn_user value="%apnuser%">
<label class=i>APN пароль</label><input name=apn_pass type=password value="%apnpass%">
</div>
</div>
<div class=card><h2 class=i>WebSocket relay (JAR)</h2>
<p class="hint i">Device ID и Encryption key генерируются автоматически и не редактируются. Их нужно внести в настройки автомобиля в OpenCARWINGS (sms_config, provider=smsgateway). Новый ID/ключ создаются кнопкой на странице статуса.</p>
<label class=i>Device ID</label>
<div class=ro><input name=jar_device_id id=did readonly value="%did%"><button type=button class="cpy i" onclick="copyVal(this,'did')">Копировать</button></div>
<label class=i>Encryption key (hex)</label>
<div class=ro><input name=jar_enc_key id=ekey readonly value="%ekey%"><button type=button class="cpy i" onclick="copyVal(this,'ekey')">Копировать</button></div>
<label class=i>WebSocket URL</label><input name=ws_url value="%wsurl%">
<label><input type=checkbox name=ws_autoreconnect value=1 %ar%><span class=i> Авто-переподключение WebSocket</span></label>
<label class=i>URL ответа FSO</label><input name=relay_url value="%relayurl%">
<label><input type=checkbox name=relay_enabled value=1 %relayenabled%><span class=i> Передавать ответы FSO на сервер</span></label>
</div>
<button class="pri i" type=submit>Сохранить и перезагрузить</button>
</form>
</div>
<script>
var E={
'Настройки устройства':'Device settings',
'Заполните поля и нажмите «Сохранить и перезагрузить»':'Fill the fields and press “Save & reboot”',
'← Назад':'← Back',
'Wi-Fi (точка доступа)':'Wi-Fi (access point)',
'SSID (домашняя сеть, только 2.4 ГГц)':'SSID (home network, 2.4 GHz only)',
'Пароль':'Password',
'SSID точки доступа для настройки':'AP SSID',
'Пароль точки доступа':'AP password',
'Интернет':'Internet',
'Способ связи':'Connection type',
'Wi-Fi':'Wi-Fi',
'SIM-карта (GPRS)':'SIM card (GPRS)',
'Wi-Fi проверка: URL (нужен HTTP 200)':'Wi-Fi check: URL (needs HTTP 200)',
'GPRS проверка: хост для пинга':'GPRS check: ping host',
'APN (точка доступа оператора)':'APN (operator point)',
'APN логин':'APN login',
'APN пароль':'APN password',
'WebSocket relay (JAR)':'WebSocket relay (JAR)',
'Device ID и Encryption key генерируются автоматически и не редактируются. Их нужно внести в настройки автомобиля в OpenCARWINGS (sms_config, provider=smsgateway). Новый ID/ключ создаются кнопкой на странице статуса.':'Device ID and Encryption key are generated automatically and cannot be edited. Enter them in the vehicle settings in OpenCARWINGS (sms_config, provider=smsgateway). New ID/key are created with the button on the status page.',
'Encryption key (hex)':'Encryption key (hex)',
'WebSocket URL':'WebSocket URL',
'Авто-переподключение WebSocket':'Auto-reconnect WebSocket',
'URL ответа FSO':'FSO reply URL','Передавать ответы FSO на сервер':'Relay FSO replies to server',
'Сохранить и перезагрузить':'Save & reboot',
'Копировать':'Copy',
'Скопировано':'Copied','Выделено':'Selected'
};
var orig=new Map();var origT=document.title;
function lang(){var s=localStorage.getItem('lang');if(s==='ru'||s==='en')return s;var l=(navigator.language||'').toLowerCase();return(l.indexOf('ru')===0||l.indexOf('uk')===0||l.indexOf('be')===0)?'ru':'en'}
function t(s){var e=E[s];return lang()==='en'&&e?e:s}
function cacheStatic(){document.querySelectorAll('.i').forEach(function(e){if(!orig.has(e))orig.set(e,e.innerHTML)})}
function applyI18n(){cacheStatic();document.documentElement.lang=lang;
 if(lang()==='en'){document.title=E[origT]||origT;document.querySelectorAll('.i').forEach(function(e){var o=orig.get(e);if(o&&E[o])e.innerHTML=E[o]})}
 else{document.title=origT;document.querySelectorAll('.i').forEach(function(e){var o=orig.get(e);if(o)e.innerHTML=o})}}
function setLangBtn(){var b=document.getElementById('bLang');if(b)b.textContent=lang()==='ru'?'English':'Русский'}
document.getElementById('bLang').onclick=function(){localStorage.setItem('lang',lang()==='ru'?'en':'ru');applyI18n();setLangBtn()};
applyI18n();setLangBtn();
function nm(v){document.getElementById('wifiopt').style.display=(v==='wifi')?'':'none';
document.getElementById('gprsopt').style.display=(v==='gprs')?'':'none'}
nm(document.querySelector('select[name=net_mode]').value);
function copyVal(b,id){
var t=document.getElementById(id);t.focus();t.select();t.setSelectionRange(0,99999);
var ok=false;try{ok=document.execCommand('copy');if(ok)window.getSelection().removeAllRanges()}catch(e){}
if(!ok&&navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t.value)}
var o=b.textContent;b.textContent=t(ok?'Скопировано':'Выделено');setTimeout(function(){b.textContent=o},1500)}
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
  p.replace("%relayurl%", g_cfg.relay_url);
  p.replace("%relayenabled%", g_cfg.relay_enabled ? "checked" : "");
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

const char* FW_VER = "2.1.4";

void web_default_behavior() {}

static void handle_status() {
  String o = "{";
  o += "\"fw\":\"" + String(FW_VER) + "\",";
  o += "\"net_mode\":\"" + g_cfg.net_mode + "\",";
  extern String net_eff_name();
  o += "\"net_eff\":\"" + net_eff_name() + "\",";
  o += "\"wifi\":" + wifi_status_json() + ",";
  o += "\"sim\":{\"ready\":" + String(g_stats.simReady ? "true" : "false") +
       ",\"csq\":" + String(g_stats.simCsq) +
       ",\"reg\":" + String(g_stats.simReg ? "true" : "false") +
       ",\"rx_parts\":" + String(g_stats.rxParts) +
       ",\"rx_msgs\":" + String(g_stats.rxMsgs) +
       ",\"relay_ok\":" + String(g_stats.relayOk) +
       ",\"relay_fail\":" + String(g_stats.relayFail) +
       ",\"rx_last\":\"" + jesc(g_stats.rxLast) + "\"},";
  o += "\"gprs\":{\"up\":" + String(g_stats.gprsUp ? "true" : "false") +
       ",\"ip\":\"" + jesc(g_stats.gprsIp) + "\"},";
  o += "\"internet\":{\"ok\":" + String(g_stats.internetOk ? "true" : "false") +
       ",\"method\":\"" + g_stats.internetMethod + "\",\"info\":\"" + jesc(g_stats.internetInfo) + "\"},";
  o += "\"ws\":{\"connected\":" + String(g_stats.wsConnected ? "true" : "false") +
       ",\"state\":\"" + g_stats.wsState + "\",\"reconnects\":" + String(g_stats.wsReconnects) + "},";
  o += "\"jar\":{\"did\":\"" + jesc(g_cfg.jar_device_id) + "\",\"ekey\":\"" + jesc(g_cfg.jar_enc_key) + "\"},";
  o += "\"sms\":{\"ok\":" + String(g_stats.smsOk) +
       ",\"fail\":" + String(g_stats.smsFail) +
       ",\"last\":\"" + jesc(g_stats.smsLast) + "\"" +
       ",\"in\":\"" + jesc(g_stats.smsIn) + "\"" +
       ",\"out\":\"" + jesc(g_stats.smsOut) + "\"}";
  o += "}";
  web.send(200, "application/json", o);
}

void internet_check_now() {
  g_stats.internetInfo = "...";
  if (g_cfg.net_mode == "gprs" || g_sim.passthroughActive) {
    g_stats.internetMethod = "gprs";
    if (g_sim.passthroughActive) {
      g_stats.internetOk = g_stats.wsConnected;
      g_stats.internetInfo = g_stats.wsConnected
        ? "GPRS TCP/WebSocket connected (AT diagnostics unavailable in data mode)"
        : "GPRS TCP data mode active, WebSocket not connected";
      return;
    }
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
  g_cfg.ws_autoreconnect = false;
  g_cfg.relay_enabled = false;
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
