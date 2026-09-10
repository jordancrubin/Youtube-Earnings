/*
  main.cpp - ESP32 E-Paper YouTube Revenue Tracker
  Designed for Waveshare 4.2" E-Paper Display (Black/White)
  Tracks estimated revenue for specific YouTube videos/channels via Google API.
  Functionality requires WiFi connection and valid Google API credentials in listing.json.
  
  https://www.youtube.com/@rubin-tech
  2026 Jordan Rubin.
*/
// FIRMWARE pio run -t upload --upload-port esp32-epaper.local
// FILESYSTEM pio run -t uploadfs --upload-port esp32-epaper.local

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <epd/GxEPD2_420.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <vector>
#include "logo.h"

// ---------------------------------------------------------------------------
// SERIAL_ONLY_MODE:
//   1 = Bench/debug mode with NO e-paper connected. Skips all display I/O
//       (a missing display hangs the BUSY pin), prints every API call and
//       the collected data to serial (115200), and stays awake after the
//       cycle instead of deep sleeping.
//   0 = Normal operation with the Waveshare 4.2" display.
// ---------------------------------------------------------------------------
#define SERIAL_ONLY_MODE 0

// Pin definitions for NodeMCU ESP32S + Waveshare 4.2" Display
// BUSY -> 4, RST -> 16, DC -> 17, CS -> 5, CLK -> 18, DIN -> 23
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

// OTA button: wire GPIO14 -> GND (internal pull-up used).
// Hold for OTA_BTN_HOLD_MS to enter OTA/web-update mode.
// A press also wakes the device from deep sleep (ext1). GPIO14 is an
// RTC-domain pin, which is required for deep-sleep wake sources.
#define OTA_BTN_PIN 14
#define OTA_BTN_HOLD_MS 5000

// AP mode (WiFi setup portal). Entered when listing.json has no "wifi"
// section, or when the saved network can't be reached. The device becomes an
// access point and shows the SSID, mDNS name, and IP on the e-paper; a phone
// connects to the AP and opens the setup page to enter the real credentials.
#define AP_SSID     "Rubintech-Setup"
#define AP_PASSWORD "rubintech"
#define AP_MDNS     "esp32-epaper"

// Instantiate the display for Waveshare 4.2 inch (GDEW042T2)
GxEPD2_BW<GxEPD2_420, GxEPD2_420::HEIGHT> display(GxEPD2_420(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// WiFi and YouTube API credentials
String ssid = "";
String password = "";

// NTP Server settings
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -18000;   // UTC -5 (EST)
const int   daylightOffset_sec = 3600; // DST +1h

unsigned long lastStatsUpdate = 0;
unsigned long lastTimeUpdate = 0;
String debugInfo = "";
String configError = "";
bool lastUpdateSuccess = false;
String startupLog = "";
int bootLogY = 0;
bool stayAwake = false;
bool isUpdating = false;
bool setupMode = false;  // true while in the AP WiFi-setup portal (no deep sleep)
unsigned long otaBtnDownAt = 0;  // millis() when the OTA button was first seen LOW
bool otaBtnActive = false;       // true once the 5-second hold is registered
WebServer server(80);

struct VideoItem {
  String name;
  String id;
  String clientId;
  String clientSecret;
  String refreshToken;
  double revenue;
  String cost;
  bool updateFailed;
  String outputMode;
  String type = "video"; // "video" (default) or "group"
};
std::vector<VideoItem> videoList;

// ---------------------------------------------------------------------------
// OTA button + web UI
// ---------------------------------------------------------------------------

// Poll the OTA button (OTA_BTN_PIN, active LOW, internal pull-up).
// A continuous hold of OTA_BTN_HOLD_MS enters OTA/web-update mode.
void checkOtaButton() {
  if (setupMode) return;  // OTA button is unused while in the AP setup portal
  if (digitalRead(OTA_BTN_PIN) == LOW) {
    if (otaBtnDownAt == 0) {
      otaBtnDownAt = millis();
    }
    if (!otaBtnActive && (millis() - otaBtnDownAt) >= OTA_BTN_HOLD_MS) {
      otaBtnActive = true;
      stayAwake = true;
      Serial.println("OTA button held - OTA UPDATE MODE (web UI + OTA active)");
#if !SERIAL_ONLY_MODE
      display.setFullWindow();
      display.firstPage();
      do {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(10, 30);
        display.println("OTA UPDATE MODE");
        display.println("Web UI: " + WiFi.localIP().toString());
      } while (display.nextPage());
#endif
    }
  } else {
    otaBtnDownAt = 0;
  }
}

// Minimal HTML escaping for embedding JSON in the editor page.
String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  return s;
}

// Make a JSON string safe to embed inside a <script> tag: the only sequence
// that can terminate the tag early is "</", and "\/" is a valid JSON escape.
String jsonForScript(String s) {
  s.replace("</", "<\\/");
  return s;
}

// Read the channels section of listing.json as a JSON string.
// Returns "" if the file is missing, invalid, or has no channels section.
// (Whole document if it's a top-level array, else the "channels" member.)
String readChannelsJson() {
  File file = SPIFFS.open("/listing.json", "r");
  if (!file) return "";
  String jsonContent = file.readString();
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, jsonContent)) return "";
  String out;
  if (doc.is<JsonArray>()) {
    serializeJsonPretty(doc, out);
  } else {
    serializeJsonPretty(doc["channels"], out);
  }
  if (out == "null" || out.length() == 0) return "";
  return out;
}

// Shared page scaffold: head, styles, header, nav links.
// Pages add their content (wrapped in <div class='card'>) and close with
// "</div></body></html>" (the final </div> closes .wrap).
String webPage(String title, String active) {
  String html;
  html.reserve(4096);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + title + " - Rubintech Config manager</title>";
  html += "<style>";
  html += ":root{--bg:#eef1f5;--card:#fff;--ink:#1f2933;--muted:#6b7280;--line:#dde3ea;--accent:#2563eb;--accent-d:#1d4ed8;--danger:#dc2626;--ok:#15803d;--r:10px}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;background:var(--bg);color:var(--ink);font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;font-size:15px;line-height:1.5}";
  html += ".wrap{max-width:1080px;margin:0 auto;padding:20px 16px 48px}";
  html += "header{display:flex;align-items:center;gap:16px;background:linear-gradient(135deg,#1e3a8a,#2563eb);color:#fff;border-radius:var(--r);padding:14px 22px;margin-bottom:14px;box-shadow:0 4px 14px rgba(30,58,138,.28)}";
  html += ".logo{height:52px;width:auto;flex:none;background:#fff;border-radius:10px;padding:5px 9px;box-shadow:0 1px 4px rgba(0,0,0,.25)}";
  html += "header h1{margin:0;font-size:22px;letter-spacing:.3px}";
  html += "header p{margin:4px 0 0;font-size:13px;opacity:.85}";
  html += ".nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}";
  html += ".nav a{padding:7px 16px;border-radius:999px;background:#fff;border:1px solid var(--line);color:var(--ink);text-decoration:none;font-size:14px;font-weight:600;transition:all .15s ease}";
  html += ".nav a:hover{border-color:var(--accent);color:var(--accent);transform:translateY(-1px)}";
  html += ".nav a.on{background:var(--accent);border-color:var(--accent);color:#fff}";
  html += ".card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:16px 18px;margin-bottom:16px;box-shadow:0 1px 3px rgba(15,23,42,.06);overflow-x:auto}";
  html += "table{border-collapse:separate;border-spacing:0;width:100%;font-size:14px}";
  html += "th{background:#f8fafc;color:var(--muted);text-transform:uppercase;font-size:11px;letter-spacing:.7px;text-align:left;padding:10px 12px;border-bottom:2px solid var(--line)}";
  html += "td{padding:8px 12px;border-bottom:1px solid var(--line);vertical-align:middle}";
  html += "tbody tr:last-child td{border-bottom:none}";
  html += "tbody tr{transition:background .1s}";
  html += "tbody tr:hover{background:#f8fafc}";
  html += "th:first-child,td:first-child{border-radius:8px 0 0 8px}";
  html += "th:last-child,td:last-child{border-radius:0 8px 8px 0}";
  html += "input,select{width:100%;padding:8px 10px;font-size:14px;font-family:inherit;border:1px solid var(--line);border-radius:8px;background:#fff;color:var(--ink);transition:border-color .15s,box-shadow .15s}";
  html += "input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(37,99,235,.15)}";
  html += "#ch{width:auto;min-width:220px;font-weight:600}";
  html += "textarea{width:100%;padding:12px;font-size:13px;line-height:1.55;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;background:#0f172a;color:#e2e8f0;border:1px solid var(--line);border-radius:8px;resize:vertical}";
  html += "textarea:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(37,99,235,.15)}";
  html += "button{font:inherit;font-size:14px;font-weight:600;padding:8px 16px;border-radius:8px;border:1px solid var(--line);background:#fff;color:var(--ink);cursor:pointer;transition:all .15s ease}";
  html += "button:hover{border-color:var(--accent);color:var(--accent);transform:translateY(-1px)}";
  html += "button:active{transform:translateY(0)}";
  html += ".btn-primary{background:var(--accent);border-color:var(--accent);color:#fff}";
  html += ".btn-primary:hover{background:var(--accent-d);border-color:var(--accent-d);color:#fff}";
  html += ".del{padding:4px 11px;color:var(--danger);font-weight:700}";
  html += ".del:hover{background:var(--danger);border-color:var(--danger);color:#fff}";
  html += ".savebar{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:14px 0 0}";
  html += "#status{font-size:14px;font-weight:500}";
  html += "#status:empty{display:none}";
  html += "#status.ok{color:var(--ok)}";
  html += "#status.err{color:var(--danger)}";
  html += "#empty{color:var(--muted);font-style:italic}";
  html += ".hint{margin:0 0 12px;color:var(--muted);font-size:13px}";
  html += "code{background:#eef2f7;border:1px solid var(--line);padding:1px 6px;border-radius:5px;font-size:13px}";
  html += "a{color:var(--accent)}";
  html += "</style></head><body>";
  html += "<div class='wrap'>";
  html += "<header><img class='logo' src='/logo' alt='' onerror='this.remove()'>";
  html += "<div class='htext'><h1>Rubintech Config manager</h1>";
  html += "<p>WiFi &amp; YouTube channel configuration</p></div></header>";
  html += "<nav class='nav'>";
  html += String("<a href='/'") + (active == "accounts" ? " class='on'" : "") + ">Accounts</a>";
  html += String("<a href='/listings'") + (active == "listings" ? " class='on'" : "") + ">Listings</a>";
  html += String("<a href='/raw'") + (active == "raw" ? " class='on'" : "") + ">Raw JSON</a>";
  html += "</nav>";
  return html;
}

// Save / Save & Reboot buttons + status line (shared by all pages).
String saveButtons() {
  return "<p class='savebar'><button class='btn-primary' onclick='doSave(0)'>Save</button> "
         "<button onclick='doSave(1)'>Save &amp; Reboot</button> "
         "<span id='status'></span></p>";
}

// Shared JS: posts the full channels array (var state) to /save.
// Pages may define beforeSave() to validate/commit before posting.
String webCommonJs() {
  String js;
  js += "function doSave(reboot){";
  js += "var st=document.getElementById('status');";
  js += "try{if(typeof beforeSave==='function')beforeSave();}";
  js += "catch(e){st.textContent='Error: '+e.message;st.className='err';return;}";
  js += "st.textContent='Saving...';";
  js += "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},";
  js += "body:'json='+encodeURIComponent(JSON.stringify(state))+'&reboot='+reboot})";
  js += ".then(function(r){return r.text().then(function(t){return{ok:r.ok,t:t};});})";
  js += ".then(function(x){st.textContent=(x.ok?'Saved: ':'Error: ')+x.t;st.className=x.ok?'ok':'err';})";
  js += ".catch(function(e){st.textContent='Request failed: '+e;st.className='err';});";
  js += "}";
  return js;
}

// GET / : accounts page - one row per channel (name + OAuth credentials).
void handleRoot() {
  String channels = readChannelsJson();
  if (channels.length() == 0) {
    server.send(500, "text/plain", "listing.json missing or invalid");
    return;
  }
  String html = webPage("accounts", "accounts");
  html += "<div class='card'>";
  html += "<p>OAuth accounts. Each row is one <code>channels[]</code> entry;";
  html += " its videos are edited on the <a href='/listings'>Listings</a> page.</p>";
  html += "<p class='hint'>Need credentials? <a href='https://developers.google.com/youtube/v3/getting-started' target='_blank' rel='noopener'>Click here for more info on creating OAuth access for this application</a>.</p>";
  html += "<table><tr><th>#</th><th>Channel</th><th>Client ID</th>";
  html += "<th>Client Secret</th><th>Refresh Token</th><th></th></tr>";
  html += "<tbody id='rows'></tbody></table>";
  html += "<button onclick='addChannel()'>+ Add account</button>";
  html += saveButtons();
  html += "</div>";
  html += "<script type='application/json' id='data'>" + jsonForScript(channels) + "</script>";
  html += "<script>";
  html += webCommonJs();
  html += "var state=JSON.parse(document.getElementById('data').textContent);";
  html += "function render(){";
  html += "var tb=document.getElementById('rows');tb.innerHTML='';";
  html += "state.forEach(function(ch,i){";
  html += "var tr=document.createElement('tr');";
  html += "var td=document.createElement('td');td.textContent=i+1;tr.appendChild(td);";
  html += "['CHANNEL','CLIENTID','CLIENTSECRET','REFRESHTOKEN'].forEach(function(k){";
  html += "var td=document.createElement('td');";
  html += "var inp=document.createElement('input');";
  html += "inp.value=(ch[k]||'');";
  html += "inp.addEventListener('input',function(){ch[k]=inp.value;});";
  html += "td.appendChild(inp);tr.appendChild(td);";
  html += "});";
  html += "var td=document.createElement('td');";
  html += "var b=document.createElement('button');b.className='del';b.textContent='x';";
  html += "b.onclick=function(){if(confirm('Remove account '+(ch.CHANNEL||('#'+(i+1)))+'?')){state.splice(i,1);render();}};";
  html += "td.appendChild(b);tr.appendChild(td);";
  html += "tb.appendChild(tr);";
  html += "});";
  html += "}";
  html += "function addChannel(){state.push({CHANNEL:'',CLIENTID:'',CLIENTSECRET:'',REFRESHTOKEN:'',LISTING:{}});render();}";
  html += "function beforeSave(){";
  html += "state.forEach(function(ch,i){";
  html += "if(!ch.CHANNEL||!String(ch.CHANNEL).trim())throw new Error('Account '+(i+1)+' has no name');";
  html += "});";
  html += "}";
  html += "render();";
  html += "</script></div></body></html>";
  server.send(200, "text/html", html);
}

// GET /listings : per-account table of LISTING entries
// (NAME / ID / COST / TYPE / OUTPUT). TYPE is a video|group dropdown;
// saving rebuilds that channel's LISTING object (unknown keys in other
// channels are untouched; a row with no COST/TYPE/OUTPUT is written back
// as a plain string ID, matching the legacy format).
void handleListings() {
  String channels = readChannelsJson();
  if (channels.length() == 0) {
    server.send(500, "text/plain", "listing.json missing or invalid");
    return;
  }
  String html = webPage("listings", "listings");
  html += "<div class='card'>";
  html += "<p>Videos and groups per account. <code>Type</code> selects how revenue is queried:";
  html += " <code>video</code> = single video ID, <code>group</code> = YouTube group ID.</p>";
  html += "<p>Account: <select id='ch'></select></p>";
  html += "<table><tr><th>Name</th><th>ID</th><th>Cost</th><th>Type</th><th>Output</th><th></th></tr>";
  html += "<tbody id='rows'></tbody></table>";
  html += "<button onclick='addRow()'>+ Add listing</button>";
  html += "<p id='empty' style='display:none'>No accounts yet. Add one on the <a href='/'>Accounts</a> page.</p>";
  html += saveButtons();
  html += "</div>";
  html += "<script type='application/json' id='data'>" + jsonForScript(channels) + "</script>";
  html += "<script>";
  html += webCommonJs();
  html += "var state=JSON.parse(document.getElementById('data').textContent);";
  html += "var sel=0,rows=[];";
  html += "function commitRows(){";
  html += "if(!state[sel])return;";
  html += "var ch=state[sel],L={};";
  html += "rows.forEach(function(r){";
  html += "var n=r.name.trim();if(!n)return;";
  html += "if(r.cost===''&&r.type!=='group'&&r.output===''){L[n]=r.id;return;}";
  html += "var o={ID:r.id};";
  html += "if(r.cost!=='')o.COST=r.cost;";
  html += "if(r.type==='group')o.TYPE='group';";
  html += "if(r.output!=='')o.OUTPUT=r.output;";
  html += "L[n]=o;";
  html += "});";
  html += "ch.LISTING=L;";
  html += "}";
  html += "function loadRows(){";
  html += "var ch=state[sel];";
  html += "var L=(ch&&ch.LISTING&&typeof ch.LISTING==='object')?ch.LISTING:{};";
  html += "rows=Object.keys(L).map(function(name){";
  html += "var v=L[name];";
  html += "if(typeof v==='object'&&v){";
  html += "return{name:name,id:v.ID||'',cost:(v.COST==null?'':String(v.COST)),type:(v.TYPE||'video'),output:v.OUTPUT||''};";
  html += "}";
  html += "return{name:name,id:String(v==null?'':v),cost:'',type:'video',output:''};";
  html += "});";
  html += "}";
  html += "function renderSelect(){";
  html += "var s=document.getElementById('ch');s.innerHTML='';";
  html += "state.forEach(function(ch,i){";
  html += "var o=document.createElement('option');";
  html += "o.value=String(i);o.textContent=ch.CHANNEL||('account '+(i+1));";
  html += "if(i===sel)o.selected=true;";
  html += "s.appendChild(o);";
  html += "});";
  html += "}";
  html += "function render(){";
  html += "renderSelect();";
  html += "var tb=document.getElementById('rows');tb.innerHTML='';";
  html += "if(!state[sel]){document.getElementById('empty').style.display='block';return;}";
  html += "document.getElementById('empty').style.display='none';";
  html += "rows.forEach(function(r,i){";
  html += "var tr=document.createElement('tr');";
  html += "function cell(tag){";
  html += "var td=document.createElement('td');";
  html += "var el=document.createElement(tag);";
  html += "td.appendChild(el);tr.appendChild(td);";
  html += "return el;";
  html += "}";
  html += "var n=cell('input');n.value=r.name;";
  html += "n.addEventListener('input',function(){r.name=n.value;});";
  html += "var id=cell('input');id.value=r.id;";
  html += "id.addEventListener('input',function(){r.id=id.value;});";
  html += "var c=cell('input');c.value=r.cost;";
  html += "c.addEventListener('input',function(){r.cost=c.value;});";
  html += "var t=cell('select');";
  html += "['video','group'].forEach(function(v){";
  html += "var o=document.createElement('option');";
  html += "o.value=v;o.textContent=v;if(r.type===v)o.selected=true;";
  html += "t.appendChild(o);";
  html += "});";
  html += "t.addEventListener('change',function(){r.type=t.value;});";
  html += "var o2=cell('input');o2.value=r.output;";
  html += "o2.addEventListener('input',function(){r.output=o2.value;});";
  html += "var td=document.createElement('td');";
  html += "var b=document.createElement('button');b.className='del';b.textContent='x';";
  html += "b.onclick=function(){rows.splice(i,1);render();};";
  html += "td.appendChild(b);tr.appendChild(td);";
  html += "tb.appendChild(tr);";
  html += "});";
  html += "}";
  html += "document.getElementById('ch').addEventListener('change',function(e){";
  html += "commitRows();";
  html += "sel=parseInt(e.target.value,10)||0;";
  html += "loadRows();render();";
  html += "});";
  html += "function addRow(){if(!state[sel])return;rows.push({name:'',id:'',cost:'',type:'video',output:''});render();}";
  html += "function beforeSave(){";
  html += "if(!state[sel])return;";
  html += "var seen={};";
  html += "rows.forEach(function(r,i){";
  html += "var n=r.name.trim();";
  html += "if(!n)throw new Error('Row '+(i+1)+' has no name');";
  html += "if(seen[n])throw new Error('Duplicate name: '+n);";
  html += "seen[n]=1;";
  html += "});";
  html += "commitRows();";
  html += "}";
  html += "loadRows();render();";
  html += "</script></div></body></html>";
  server.send(200, "text/html", html);
}

// GET /raw : raw JSON editor for the channels section (advanced).
void handleRaw() {
  String channels = readChannelsJson();
  if (channels.length() == 0) {
    server.send(500, "text/plain", "listing.json missing or invalid");
    return;
  }
  String html = webPage("raw JSON", "raw");
  html += "<div class='card'>";
  html += "<p>Edit the channels array below. Only <code>channels</code> is changed;";
  html += " <code>wifi</code> and any other top-level keys are preserved.</p>";
  html += "<textarea id='ta' rows='20' cols='80' spellcheck='false'>";
  html += htmlEscape(channels);
  html += "</textarea>";
  html += saveButtons();
  html += "</div>";
  html += "<script>";
  html += "function doSave(reboot){";
  html += "var ta=document.getElementById('ta');var st=document.getElementById('status');";
  html += "try{var v=JSON.parse(ta.value);if(!Array.isArray(v)){st.textContent='Not a JSON array';st.className='err';return;}}";
  html += "catch(e){st.textContent='Invalid JSON: '+e.message;st.className='err';return;}";
  html += "st.textContent='Saving...';";
  html += "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},";
  html += "body:'json='+encodeURIComponent(ta.value)+'&reboot='+reboot})";
  html += ".then(function(r){return r.text().then(function(t){return{ok:r.ok,t:t};})})";
  html += ".then(function(x){st.textContent=(x.ok?'Saved: ':'Error: ')+x.t;st.className=x.ok?'ok':'err';})";
  html += ".catch(function(e){st.textContent='Request failed: '+e;st.className='err';});";
  html += "}";
  html += "</script></div></body></html>";
  server.send(200, "text/html", html);
}

// GET /logo : serve the Rubintech logo PNG from SPIFFS (data/logo.png).
void handleLogo() {
  File f = SPIFFS.open("/logo.png", "r");
  if (!f) {
    server.send(404, "text/plain", "logo.png missing - run: pio run -t uploadfs");
    return;
  }
  server.streamFile(f, "image/png");
}

// POST /save : persist the edited channels section, preserving other keys.
void handleSave() {
  String json = server.arg("json");
  bool reboot = (server.arg("reboot") == "1");

  JsonDocument posted;
  DeserializationError err = deserializeJson(posted, json);
  if (err || !posted.is<JsonArray>()) {
    server.send(400, "text/plain", String("Invalid JSON or not an array: ") + err.c_str());
    return;
  }

  // Read the existing file to preserve top-level keys other than "channels".
  JsonDocument newDoc;
  bool originalIsObject = false;
  File file = SPIFFS.open("/listing.json", "r");
  if (file) {
    String existing = file.readString();
    file.close();
    JsonDocument oldDoc;
    if (!deserializeJson(oldDoc, existing) && oldDoc.is<JsonObject>()) {
      originalIsObject = true;
      JsonObject obj = newDoc.to<JsonObject>();
      for (JsonPair kv : oldDoc.as<JsonObject>()) {
        if (strcmp(kv.key().c_str(), "channels") == 0) continue;
        obj[kv.key().c_str()] = kv.value();
      }
      JsonArray chans = obj["channels"].to<JsonArray>();
      for (JsonObject c : posted.as<JsonArray>()) {
        chans.add(c);
      }
    }
  }
  if (!originalIsObject) {
    // Legacy top-level array (or missing/corrupt file): write the posted array.
    newDoc.set(posted);
  }

  File out = SPIFFS.open("/listing.json", "w");
  if (!out) {
    server.send(500, "text/plain", "Could not open listing.json for writing");
    return;
  }
  size_t written = serializeJsonPretty(newDoc, out);
  out.close();
  if (written == 0) {
    server.send(500, "text/plain", "Failed to write listing.json");
    return;
  }

  Serial.println("listing.json updated via web UI (" + String(written) + " bytes)");
  if (reboot) {
    server.send(200, "text/plain", "Saved. Rebooting...");
    delay(200);
    ESP.restart();
  } else {
    server.send(200, "text/plain", "Saved. (Config applies after reboot.)");
  }
}

// ---------------------------------------------------------------------------
// AP WiFi-setup mode
//
// Entered when listing.json has no "wifi" section (or the saved network can't
// be reached). The device becomes an access point and shows the SSID, mDNS
// name, and IP on the e-paper. A phone connects to the AP and opens the setup
// page to enter the real WiFi credentials; saving them reboots into normal
// mode.
// ---------------------------------------------------------------------------

// Draw the setup instructions on the e-paper (no-op in SERIAL_ONLY_MODE).
void showSetupScreen(const IPAddress &ip) {
#if !SERIAL_ONLY_MODE
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextSize(2);
    display.setCursor(10, 18);
    display.println("WiFi Setup Mode");
    display.drawLine(10, 50, display.width() - 10, 50, GxEPD_BLACK);

    display.setTextSize(1);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 72);
    display.println("1) Connect to this WiFi network:");
    display.setFont(&FreeSansBold9pt7b);
    display.println("   " + String(AP_SSID));
    display.setFont(&FreeSans9pt7b);
    display.println("   Password: " + String(AP_PASSWORD));

    display.setCursor(10, 150);
    display.println("2) Open this address in a browser:");
    display.setFont(&FreeSansBold9pt7b);
    display.println("   " + String(AP_MDNS) + ".local");
    display.setFont(&FreeSans9pt7b);
    display.println("   or  " + ip.toString());

    display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
  } while (display.nextPage());
#endif
}

// GET / (setup mode): simple WiFi configuration page.
void handleSetupPage() {
  String html;
  html.reserve(3072);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>WiFi Setup - Rubintech</title>";
  html += "<style>";
  html += ":root{--bg:#eef1f5;--card:#fff;--ink:#1f2933;--muted:#6b7280;--line:#dde3ea;--accent:#2563eb;--accent-d:#1d4ed8;--ok:#15803d;--danger:#dc2626;--r:10px}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;background:var(--bg);color:var(--ink);font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;font-size:15px;line-height:1.5}";
  html += ".wrap{max-width:520px;margin:0 auto;padding:24px 16px 48px}";
  html += "header{background:linear-gradient(135deg,#1e3a8a,#2563eb);color:#fff;border-radius:var(--r);padding:16px 22px;margin-bottom:16px;box-shadow:0 4px 14px rgba(30,58,138,.28)}";
  html += "header h1{margin:0;font-size:22px}";
  html += "header p{margin:5px 0 0;font-size:13px;opacity:.85}";
  html += ".card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:18px;box-shadow:0 1px 3px rgba(15,23,42,.06)}";
  html += "label{display:block;font-weight:600;font-size:13px;margin:14px 0 6px;color:var(--muted)}";
  html += "input{width:100%;padding:10px 12px;font-size:15px;font-family:inherit;border:1px solid var(--line);border-radius:8px;background:#fff;color:var(--ink)}";
  html += "input:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(37,99,235,.15)}";
  html += "button{font:inherit;font-size:15px;font-weight:600;padding:11px 18px;border-radius:8px;border:none;background:var(--accent);color:#fff;cursor:pointer;margin-top:18px;width:100%}";
  html += "button:hover{background:var(--accent-d)}";
  html += "button:disabled{opacity:.6;cursor:default}";
  html += "#status{display:block;margin-top:12px;font-size:14px;font-weight:500;min-height:1em}";
  html += "#status.ok{color:var(--ok)}";
  html += "#status.err{color:var(--danger)}";
  html += ".hint{margin:0;color:var(--muted);font-size:13px}";
  html += "</style></head><body>";
  html += "<div class='wrap'>";
  html += "<header><h1>WiFi Setup</h1><p>Rubintech e-paper display</p></header>";
  html += "<div class='card'>";
  html += "<p class='hint'>Enter your WiFi details. The device saves them and reboots to connect.</p>";
  html += "<form onsubmit='saveWifi(event)'>";
  html += "<label for='ssid'>Network name (SSID)</label>";
  html += "<input id='ssid' name='ssid' autocomplete='off' required>";
  html += "<label for='password'>Password</label>";
  html += "<input id='password' name='password' type='password' autocomplete='off'>";
  html += "<button type='submit' id='btn'>Save &amp; Connect</button>";
  html += "<span id='status'></span>";
  html += "</form>";
  html += "</div></div>";
  html += "<script>";
  html += "function saveWifi(e){";
  html += "e.preventDefault();";
  html += "var st=document.getElementById('status');";
  html += "var btn=document.getElementById('btn');";
  html += "btn.disabled=true;st.textContent='Saving...';st.className='';";
  html += "var body='ssid='+encodeURIComponent(document.getElementById('ssid').value)";
  html += "+'&password='+encodeURIComponent(document.getElementById('password').value);";
  html += "fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})";
  html += ".then(function(r){return r.text().then(function(t){return{ok:r.ok,t:t};});})";
  html += ".then(function(x){st.textContent=(x.ok?'Saved: ':'Error: ')+x.t;st.className=x.ok?'ok':'err';});";
  html += ".catch(function(err){st.textContent='Request failed: '+err;st.className='err';btn.disabled=false;});";
  html += "}";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

// POST /wifi : save the WiFi credentials into listing.json (preserving the
// channels section and any other top-level keys), then reboot to connect.
void handleWifiSave() {
  String newSsid = server.arg("ssid");
  String newPass = server.arg("password");
  newSsid.trim();

  if (newSsid.length() == 0) {
    server.send(400, "text/plain", "SSID is required");
    return;
  }

  // Load the existing file (if any) so we can preserve everything except wifi.
  JsonDocument existing;
  bool haveExisting = false;
  bool existingIsObject = false;
  File file = SPIFFS.open("/listing.json", "r");
  if (file) {
    String content = file.readString();
    file.close();
    if (!deserializeJson(existing, content)) {
      haveExisting = true;
      existingIsObject = existing.is<JsonObject>();
    }
  }

  // Build the new document: an object with the saved wifi + preserved keys.
  JsonDocument newDoc;
  JsonObject obj = newDoc.to<JsonObject>();
  if (haveExisting) {
    if (existingIsObject) {
      for (JsonPair kv : existing.as<JsonObject>()) {
        if (strcmp(kv.key().c_str(), "wifi") == 0) continue;
        obj[kv.key().c_str()] = kv.value();
      }
    }
    else {
      // Legacy top-level array: wrap it under "channels".
      JsonArray chans = obj["channels"].to<JsonArray>();
      for (JsonObject c : existing.as<JsonArray>()) {
        chans.add(c);
      }
    }
  }

  JsonObject wifi = obj["wifi"].to<JsonObject>();
  wifi["ssid"] = newSsid;
  wifi["password"] = newPass;

  File out = SPIFFS.open("/listing.json", "w");
  if (!out) {
    server.send(500, "text/plain", "Could not open listing.json for writing");
    return;
  }
  size_t written = serializeJsonPretty(newDoc, out);
  out.close();
  if (written == 0) {
    server.send(500, "text/plain", "Failed to write listing.json");
    return;
  }

  Serial.println("WiFi config saved via setup page (" + String(written) + " bytes)");
  server.send(200, "text/plain", "Saved. Rebooting to connect...");
  delay(500);
  ESP.restart();
}

// Enter AP setup mode: start the access point + mDNS, show the instructions
// on the display, and serve the setup page.
void enterApMode() {
  setupMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();

  MDNS.begin(AP_MDNS);
  MDNS.addService("http", "tcp", 80);

  showSetupScreen(ip);

  server.on("/", HTTP_GET, handleSetupPage);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.begin();

  Serial.println("AP mode: SSID=" + String(AP_SSID) + " pass=" + String(AP_PASSWORD) + " ip=" + ip.toString());
  Serial.println("Setup page: http://" + ip.toString() + "/  or  http://" + String(AP_MDNS) + ".local/");
}

void setup() {
  Serial.begin(115200);
  delay(100); 

#if !SERIAL_ONLY_MODE
  // Configure pins
  pinMode(EPD_CS, OUTPUT);
  pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_RST, OUTPUT);
  pinMode(EPD_BUSY, INPUT);

  // Initialize SPI (Standard ESP32 VSPI: CLK=18, MISO=19, MOSI=23, CS=5)
  SPI.begin(18, 19, 23, 5);

  // Initialize the display
  display.init(115200);
  display.setRotation(0); // 0 = Landscape
  display.setTextColor(GxEPD_BLACK);

  // Show Logo
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    // Draw the bitmap centered (assuming 400x300 image for 4.2" display)
    // Arguments: x, y, bitmap_array, width, height, color
    display.drawBitmap(0, 0, logoBitmap, 400, 300, GxEPD_BLACK);
  } while (display.nextPage());
  delay(2000);

  display.setFont(&FreeSans9pt7b); // Reset font for logs
  display.setTextSize(1);

  // Clear screen initially for boot log
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
#endif

  auto logToDisplay = [&](String msg, bool newline = true) {
    if (newline) Serial.println(msg); else Serial.print(msg);
    startupLog += msg + (newline ? "\n" : "");
#if !SERIAL_ONLY_MODE
    display.setPartialWindow(0, 0, display.width(), display.height());
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      display.setCursor(0, 20);
      display.println(startupLog);
      bootLogY = display.getCursorY();
    } while (display.nextPage());
#endif
  };

  logToDisplay("Initializing...");

  // Mount SPIFFS and read listing.json
  if (SPIFFS.begin(true)) {
    logToDisplay("SPIFFS Mounted.");
    File file = SPIFFS.open("/listing.json", "r");
    if (file) {
      logToDisplay("Reading listing.json...");
      // Read file content into a String first to avoid stream issues
      String jsonContent = file.readString();
      file.close();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, jsonContent);
      if (!error) {
        JsonArray arr;
        if (doc.is<JsonArray>()) {
          arr = doc.as<JsonArray>();
        } else {
          if (doc["wifi"]) {
            ssid = doc["wifi"]["ssid"].as<String>();
            password = doc["wifi"]["password"].as<String>();
          }
          arr = doc["channels"];
        }

        for (JsonObject obj : arr) {
          String cId = obj["CLIENTID"].as<String>(); cId.trim();
          String cSecret = obj["CLIENTSECRET"].as<String>(); cSecret.trim();
          String rToken = obj["REFRESHTOKEN"].as<String>(); rToken.trim();
          JsonObject listing = obj["LISTING"];
          
          for (JsonPair kv : listing) {
            VideoItem item;
            item.name = String(kv.key().c_str());
            item.name.trim();
            
            if (kv.value().is<JsonObject>()) {
              JsonObject v = kv.value().as<JsonObject>();
              item.id = v["ID"].as<String>();
              item.cost = v["COST"].as<String>();
              if (v.containsKey("TYPE")) {
                item.type = v["TYPE"].as<String>();
                item.type.trim();
                item.type.toLowerCase();
              }
              if (v.containsKey("OUTPUT")) {
                item.outputMode = v["OUTPUT"].as<String>();
                item.outputMode.trim();
              }
            }
            else {
              item.id = kv.value().as<String>();
              item.cost = "";
            }
            item.id.trim();
            item.clientId = cId;
            item.clientSecret = cSecret;
            item.refreshToken = rToken;
            item.revenue = 0.0;
            item.updateFailed = false;
            videoList.push_back(item);
            Serial.print("Loaded video: "); Serial.println(item.name);
          }
        }
        logToDisplay("Loaded " + String(videoList.size()) + " videos.");
      }
      else {
        logToDisplay("JSON Parse Error!");
        configError = "JSON Parse Error";
      }
    }
    else {
      logToDisplay("listing.json missing!");
      configError = "Missing listing.json";
    }
  }
  else {
    logToDisplay("SPIFFS Mount Failed!");
    configError = "SPIFFS Mount Fail";
  }

  if (videoList.empty()) {
    logToDisplay("No videos configured.");
    logToDisplay("(Add via web UI)");
  }

  // -----------------------------------------------------------------------
  // WiFi: connect using the saved config. Tries up to 10 times (a "." is
  // shown on the display per attempt); if all fail it reboots to retry.
  // With no config at all it falls back to the AP setup portal.
  // -----------------------------------------------------------------------
  bool hasWifiConfig = (ssid.length() > 0);
  bool wifiConnected = false;

  if (hasWifiConfig) {
    logToDisplay("Connecting to WiFi", false);
    WiFi.mode(WIFI_STA);
    wifiConnected = false;
    // Up to 10 connection attempts. Each attempt does a fresh WiFi.begin()
    // and waits up to 10s; a "." is appended to the display after every
    // attempt so the user can see progress. If all 10 fail, give up and
    // reboot so the whole cycle starts over.
    const int WIFI_MAX_ATTEMPTS = 10;
    for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS && !wifiConnected; attempt++) {
      if (attempt > 1) {
        Serial.println("WiFi attempt " + String(attempt) + "/" + String(WIFI_MAX_ATTEMPTS));
        WiFi.disconnect();
        delay(100);
      }
      WiFi.begin(ssid.c_str(), password.c_str());
      int waited = 0;
      while (WiFi.status() != WL_CONNECTED && waited < 20) { // up to 10s per attempt
        delay(500);
        waited++;
      }
      wifiConnected = (WiFi.status() == WL_CONNECTED);
      logToDisplay(".", false); // one dot per attempt
    }
    if (wifiConnected) {
      logToDisplay(" [Connected]");
      logToDisplay("IP: " + WiFi.localIP().toString());
    }
    else {
      logToDisplay(" [Failed]");
      logToDisplay("Gave up after 10");
      logToDisplay("attempts. Rebooting...");
      delay(2000);
      ESP.restart();
    }
  }
  else {
    logToDisplay("No WiFi config in");
    logToDisplay("listing.json.");
    logToDisplay("Setup mode");
    logToDisplay("starting...");
  }

  if (wifiConnected) {
    // ---- Normal mode: web UI + NTP + OTA + stats ----
    server.on("/", HTTP_GET, handleRoot);
    server.on("/listings", HTTP_GET, handleListings);
    server.on("/raw", HTTP_GET, handleRaw);
    server.on("/logo", HTTP_GET, handleLogo);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    logToDisplay("Web UI: http://" + WiFi.localIP().toString() + "/");

    // NTP Sync
    logToDisplay("Syncing Time...", false);
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo, 5000)) {
      logToDisplay(" [Error]");
      delay(1000);
      int lastIdx = startupLog.lastIndexOf("Syncing Time...");
      if (lastIdx >= 0) startupLog = startupLog.substring(0, lastIdx);
      logToDisplay("Syncing Time...", false);
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    }
    logToDisplay(" [Synced]");

    // Check for Boot button (GPIO 0) or OTA button (OTA_BTN_PIN) to stay awake
    pinMode(0, INPUT_PULLUP);
    pinMode(OTA_BTN_PIN, INPUT_PULLUP);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0
        || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1
        || digitalRead(0) == LOW
        || digitalRead(OTA_BTN_PIN) == LOW) {
      stayAwake = true;
      logToDisplay("OTA UPDATE MODE", true);
      logToDisplay("Staying awake...", true);
      logToDisplay("Staying awake for 5 minutes.", true);
    }

    // OTA Setup
    ArduinoOTA.setHostname("esp32-epaper");

    ArduinoOTA.onStart([]() {
      isUpdating = true;
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      }
      else { // U_SPIFFS
        type = "filesystem";
        SPIFFS.end(); // Unmount SPIFFS to prevent corruption during update
      }
      Serial.println("Start updating " + type);
#if !SERIAL_ONLY_MODE
      display.setFullWindow();
      display.firstPage();
      do {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(10, 30);
        display.println("OTA Update...");
        display.println(type);
      } while (display.nextPage());
#endif
    });

    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });

    ArduinoOTA.begin();
  }
  else {
    // ---- AP setup mode: no usable WiFi config ----
    enterApMode();
  }
}

void drawDisplay(bool partial) {
  const char* monthNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  struct tm timeinfo;
  char timeStr[64] = "Time Sync Error";
  bool timeSynced = getLocalTime(&timeinfo);
  if (timeSynced) {
    sprintf(timeStr, "%02d %s %d %02d:%02d", timeinfo.tm_mday, monthNames[timeinfo.tm_mon], timeinfo.tm_year + 1900, timeinfo.tm_hour, timeinfo.tm_min);
  }

  if (partial) {
    // Only update the top 30 pixels for the time
    display.setPartialWindow(0, 0, display.width(), 30);
  }
  else {
    display.setFullWindow();
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 20);
    display.println(timeStr);

    // Draw Next Update Time (Top Right)
    if (timeSynced) {
      struct tm nextUpdate = timeinfo;
      nextUpdate.tm_hour += 12;
      mktime(&nextUpdate); // Normalize time (handles day rollover)

      char datePart[32];
      sprintf(datePart, "%02d %s %02d:%02d", nextUpdate.tm_mday, monthNames[nextUpdate.tm_mon], nextUpdate.tm_hour, nextUpdate.tm_min);
      
      // Measure "NEXT" in Bold
      display.setFont(&FreeSansBold9pt7b);
      int16_t b_tbx, b_tby; uint16_t b_tbw, b_tbh;
      display.getTextBounds("NEXT", 0, 0, &b_tbx, &b_tby, &b_tbw, &b_tbh);

      // Measure datePart in Normal
      display.setFont(&FreeSans9pt7b);
      int16_t n_tbx, n_tby; uint16_t n_tbw, n_tbh;
      display.getTextBounds(datePart, 0, 0, &n_tbx, &n_tby, &n_tbw, &n_tbh);
      
      // Check overlap with current time string on the left
      int16_t l_tbx, l_tby; uint16_t l_tbw, l_tbh;
      display.getTextBounds(timeStr, 10, 20, &l_tbx, &l_tby, &l_tbw, &l_tbh);
      
      int totalWidth = b_tbw + n_tbw + 8; // +8 for space
      int cursorX = display.width() - totalWidth - 10;

      if (cursorX < (10 + l_tbw + 20)) { // If overlap or too close, use arrow
        cursorX = display.width() - n_tbw - 10;
        
        // Draw Arrow ->
        int ax = cursorX - 18; int ay = 14;
        display.drawLine(ax, ay, ax + 12, ay, GxEPD_BLACK);
        display.drawLine(ax + 12, ay, ax + 8, ay - 3, GxEPD_BLACK);
        display.drawLine(ax + 12, ay, ax + 8, ay + 3, GxEPD_BLACK);
        
        display.setCursor(cursorX, 20);
        display.print(datePart);
      } else {
        display.setCursor(cursorX, 20);
        display.setFont(&FreeSansBold9pt7b);
        display.print("NEXT");
        display.print(" ");
        display.setFont(&FreeSans9pt7b);
        display.print(datePart);
      }
    }

    // Draw Header Border
    display.drawRect(0, 0, display.width(), 30, GxEPD_BLACK);

    if (!partial) {
      display.setCursor(10, 45); display.print("Name");
      display.setCursor(220, 45); display.print("Cost");
      display.setCursor(320, 45); display.print("Earn");

      int yPos = 70;
      for (const auto &video : videoList) {
        double costVal = video.cost.toDouble();
        if (video.revenue > costVal) {
          display.setFont(&FreeSansBold9pt7b);
        }
        else {
          display.setFont(&FreeSans9pt7b);
        }

        display.setCursor(10, yPos);
        display.print(video.name);

        if (video.updateFailed) {
          display.setCursor(200, yPos);
          display.print("X");
        }

        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds(video.cost, 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor(290 - tbw, yPos);
        display.print(video.cost);

        String revStr;
        if (video.outputMode == "percentage") {
          double c = video.cost.toDouble();
          if (c != 0.0) {
            revStr = String((video.revenue / c) * 100.0, 2) + "%";
          }
          else {
            revStr = String(video.revenue, 2);
          }
        } else {
          revStr = String(video.revenue, 2);
        }
        display.getTextBounds(revStr, 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor(390 - tbw, yPos);
        display.print(revStr);

        yPos += 30;
      }
    }
    display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
  } while (display.nextPage());
}

String getAccessToken(String cId, String cSecret, String rToken) {
  time_t now;
  time(&now);
  if (now < 100000) {
    Serial.println("Warning: Time not synced, retrying NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    delay(1000);
  }

  for (int i = 0; i < 3; i++) {
    if (i > 0) {
      Serial.println("Retrying... Forcing WiFi reconnect");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid.c_str(), password.c_str());
    }
    else if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      WiFi.reconnect();
    }

    if (WiFi.status() != WL_CONNECTED) {
      int wifiRetries = 0;
      while (WiFi.status() != WL_CONNECTED && wifiRetries < 20) {
        delay(500);
        wifiRetries++;
      }
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000); // TCP connection timeout
    client.setHandshakeTimeout(30000); // SSL handshake timeout
    HTTPClient http;
    
    Serial.print("Free Heap: "); Serial.println(ESP.getFreeHeap());
    
    // Exchange refresh token for access token
    if (http.begin(client, "https://oauth2.googleapis.com/token")) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      String requestBody = "client_id=" + cId + 
                           "&client_secret=" + cSecret + 
                           "&refresh_token=" + rToken + 
                           "&grant_type=refresh_token";
      int httpCode = http.POST(requestBody);
      if (httpCode == 200) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        String token = doc["access_token"].as<String>();
        if (!error && token.length() > 0) {
          // Bench debugging: print the issued token value (1h-lived, user-approved).
          Serial.print("Token issued: ");
          Serial.println(token);
          http.end();
          return token;
        }
        Serial.print("Token response missing access_token: ");
        Serial.println(payload);
        http.end();
        return "";
      }
      String errBody = http.getString();
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, errBody) && errDoc["error"].is<String>()) {
        // Clean single-line error, e.g.: Token error: invalid_grant - Bad Request (HTTP 400)
        Serial.print("Token error: ");
        Serial.print(errDoc["error"].as<String>());
        if (errDoc["error_description"].is<String>()) {
          Serial.print(" - ");
          Serial.print(errDoc["error_description"].as<String>());
        }
        Serial.print(" (HTTP ");
        Serial.print(httpCode);
        Serial.println(")");
      }
      else {
        Serial.print("Token HTTP Error: "); Serial.println(httpCode);
        Serial.println(errBody);
      }
      http.end();
    }
    else {
      Serial.println("Token connection failed");
    }
    delay(1000);
    client.stop();
    delay(2000);
  }
  return "";
}

// Resolve a YouTube Analytics group to its member video IDs.
// GET /v2/groupItems?groupId=<id> -> items[].resource.id, comma-joined.
// ok=false on transport/parse failure; ok=true with "" for an empty group.
String getGroupVideoIds(String groupId, String accessToken, bool &ok) {
  ok = false;
  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation
  client.setTimeout(15000); // TCP connection timeout
  client.setHandshakeTimeout(30000); // SSL handshake timeout
  HTTPClient http;

  String url = "https://youtubeanalytics.googleapis.com/v2/groupItems?groupId=" + groupId;
  Serial.print("GroupItems URL: ");
  Serial.println(url);

  if (http.begin(client, url)) {
    http.addHeader("Authorization", "Bearer " + accessToken);
    int httpCode = http.GET();
    Serial.print("groupItems HTTP Code: ");
    Serial.println(httpCode);

    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        String ids = "";
        int count = 0;
        JsonArray items = doc["items"];
        for (JsonObject item : items) {
          String vid = item["resource"]["id"].as<String>();
          vid.trim();
          if (vid.length() > 0) {
            if (ids.length() > 0) ids += ",";
            ids += vid;
            count++;
          }
        }
        Serial.print("Group ");
        Serial.print(groupId);
        Serial.print(" contains ");
        Serial.print(count);
        Serial.println(" video(s).");
        ok = true;
        http.end();
        return ids;
      }
      Serial.println("groupItems JSON parse error");
    }
    else {
      Serial.print("groupItems Error: ");
      Serial.println(http.getString());
    }
    http.end();
  }
  else {
    Serial.println("groupItems connection failed");
  }
  return "";
}

void fetchStats() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Fetching Stats...");
    debugInfo = "";
    bool currentUpdateSuccess = true;
    Serial.print("Total Videos to fetch: "); Serial.println(videoList.size());
    String currentAccessToken = "";
    String currentRefreshToken = "";

    // Get current date for endDate parameter
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    if (timeinfo.tm_year + 1900 < 2020) {
      Serial.println("Time invalid (pre-2020). Forcing NTP sync...");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      delay(2000);
      if (!getLocalTime(&timeinfo)) return;
    }

    // Workaround for Google Issue #474612194: Group analytics return 500 if data isn't ready.
    // Backdate by 2 days to ensure stability.
    time_t now;
    time(&now);
    now -= (2 * 86400);
    localtime_r(&now, &timeinfo);

    char dateBuf[12];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
    Serial.print("endDate: ");
    Serial.println(dateBuf);

    for (auto &video : videoList) {
      Serial.print("Processing video: "); Serial.println(video.name);
      video.updateFailed = true;
      
#if !SERIAL_ONLY_MODE
      if (lastStatsUpdate == 0) {
        int pollingY = bootLogY + 25; // Leave a blank line (approx 25px)
        // Define a partial window for just this line
        display.setPartialWindow(0, pollingY - 20, display.width(), 30);
        display.firstPage();
        do {
          display.fillScreen(GxEPD_WHITE); // Clear previous name
          display.setCursor(0, pollingY);
          display.print("Polling: " + video.name);
        } while (display.nextPage());
      }
#endif

      // Check if we need to switch tokens (new channel)
      if (video.refreshToken != currentRefreshToken || currentAccessToken == "") {
        Serial.println("Requesting new Access Token...");
        currentRefreshToken = video.refreshToken;
        currentAccessToken = getAccessToken(video.clientId, video.clientSecret, currentRefreshToken);
      }
      
      if (currentAccessToken == "") {
        Serial.println("Failed to get token for " + video.name);
        continue;
      }

      // Analytics API: Get estimated revenue from 2006 (YouTube start) to today.
      // Video vs group is explicit in the config ("TYPE": "group"); the legacy
      // "GR-" prefix on ID or NAME still works for old configs.
      String idsParam = "ids=channel==MINE";
      bool isGroup = (video.type == "group");
      String groupId = "";
      if (isGroup) {
        groupId = video.id.startsWith("GR-") ? video.id.substring(3) : video.id;
      }
      else if (video.id.startsWith("GR-")) {
        isGroup = true;
        groupId = video.id.substring(3);
      }
      else if (video.name.startsWith("GR-")) {
        isGroup = true;
        groupId = video.id;
      }

      // Groups query the group directly (filters=group==); videos query the
      // video (filters=video==).
      String filterParam = isGroup
        ? ("&filters=group==" + groupId)
        : ("&filters=video==" + video.id);

      String url = "https://youtubeanalytics.googleapis.com/v2/reports?" + idsParam + "&metrics=estimatedRevenue&startDate=2006-01-01&endDate=" + String(dateBuf) + filterParam;

      Serial.print("URL: "); Serial.println(url);

      // Up to 3 attempts: transport failures (negative codes, e.g. -11 read
      // timeout) and HTTP 5xx are retried with backoff. Each attempt uses a
      // fresh TCP/TLS connection; a definitive response (200, 401, 403, ...)
      // ends the loop.
      const int MAX_ATTEMPTS = 3;
      int httpCode = 0;
      String payload = "";
      bool haveResponse = false;
      for (int attempt = 1; attempt <= MAX_ATTEMPTS && !haveResponse; attempt++) {
        if (attempt > 1) {
          Serial.print("Retry "); Serial.print(video.name);
          Serial.print(" (attempt "); Serial.print(attempt);
          Serial.print(" of "); Serial.print(MAX_ATTEMPTS);
          Serial.println(")");
          delay(2000 * (attempt - 1)); // backoff: 2s, then 4s
        }
        WiFiClientSecure client;
        client.setInsecure(); // Skip certificate validation
        client.setTimeout(15000); // TCP connect/read timeout
        client.setHandshakeTimeout(30000); // SSL handshake timeout
        HTTPClient http;

        if (!http.begin(client, url)) {
          Serial.println("Connection failed");
          httpCode = -2;
          continue;
        }
        http.addHeader("Authorization", "Bearer " + currentAccessToken);
        httpCode = http.GET();
        Serial.print("HTTP Code for "); Serial.print(video.name); Serial.print(": ");
        Serial.println(httpCode);
        if (httpCode < 0 || httpCode >= 500) {
          // Transport error (-11 read timeout, ...) or server error: drop the
          // connection and retry.
          http.end();
          continue;
        }
        haveResponse = true;
        payload = http.getString();
        Serial.println(payload);
        http.end();
      }

      if (haveResponse && httpCode == 200) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
          // Response format: { "rows": [ [ 123.45 ] ] }
          if (doc.containsKey("rows") && doc["rows"].size() > 0) {
            video.revenue = doc["rows"][0][0];
          }
          else {
            video.revenue = 0.0;
          }
          video.updateFailed = false;
          Serial.print("Stats: "); Serial.print(video.name);
          Serial.print(" | Cost: "); Serial.print(video.cost);
          Serial.print(" | Rev: "); Serial.println(video.revenue);
        }
      }
      else {
        debugInfo = "API Error: " + String(httpCode);
        // Attempt to parse error message
        currentUpdateSuccess = false;
      }
      delay(500); // Small delay between requests
    }
    lastUpdateSuccess = currentUpdateSuccess;
  }
}

#if SERIAL_ONLY_MODE
// Serial table of the collected data (replaces the e-paper layout in bench mode).
void printSummary() {
  const char* monthNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  struct tm timeinfo;
  Serial.println("\n==================== SUMMARY ====================");
  if (getLocalTime(&timeinfo)) {
    char timeStr[64];
    sprintf(timeStr, "%02d %s %d %02d:%02d", timeinfo.tm_mday, monthNames[timeinfo.tm_mon], timeinfo.tm_year + 1900, timeinfo.tm_hour, timeinfo.tm_min);
    Serial.print("Time: ");
    Serial.println(timeStr);
    struct tm nextUpdate = timeinfo;
    nextUpdate.tm_hour += 12;
    mktime(&nextUpdate); // Normalize time (handles day rollover)
    char nextStr[32];
    sprintf(nextStr, "%02d %s %02d:%02d", nextUpdate.tm_mday, monthNames[nextUpdate.tm_mon], nextUpdate.tm_hour, nextUpdate.tm_min);
    Serial.print("Next update: ");
    Serial.println(nextStr);
  }
  else {
    Serial.println("Time: (not synced)");
  }
  Serial.println("----------------------------------------------------");
  Serial.printf("%-18s %-10s %-12s %s\n", "NAME", "COST", "EARN", "STATUS");
  for (const auto &video : videoList) {
    String revStr;
    if (video.outputMode == "percentage") {
      double c = video.cost.toDouble();
      if (c != 0.0) {
        revStr = String((video.revenue / c) * 100.0, 2) + "%";
      }
      else {
        revStr = String(video.revenue, 2);
      }
    }
    else {
      revStr = String(video.revenue, 2);
    }
    Serial.printf("%-18s %-10s %-12s %s\n", video.name.c_str(), video.cost.c_str(), revStr.c_str(), video.updateFailed ? "X" : "OK");
  }
  Serial.println("====================================================");
}
#endif

void loop() {
  ArduinoOTA.handle();
  checkOtaButton();
  server.handleClient();

  if (setupMode) {
    // Stay in the AP setup portal until WiFi is saved (which reboots).
    delay(50);
    return;
  }

  if (stayAwake) {
    if (!isUpdating && millis() > 300000) { // 5 minutes timeout
      Serial.println("OTA Timeout. Rebooting...");
      ESP.restart();
    }
    return;
  }

  // Perform stats update
  fetchStats();
  lastStatsUpdate = millis(); // Update timestamp for countdown calculation

#if SERIAL_ONLY_MODE
  printSummary();
  Serial.println("\nSerial-only mode: cycle complete. Staying awake (no deep sleep).");
  Serial.println("Reset the board to run another cycle.");
  for (;;) { ArduinoOTA.handle(); checkOtaButton(); server.handleClient(); delay(1000); } // Bench: idle, keep OTA + web UI alive
#else
  drawDisplay(false);         // Full refresh

  // Enter Deep Sleep
  Serial.println("Entering deep sleep for 12 hours...");
  const uint64_t TIME_TO_SLEEP = 43200000ULL * 1000; // 12 hours in microseconds
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP);
  // Wake from deep sleep when the OTA button (GPIO14, active LOW) is pressed.
  // ext1 has no "any LOW" mode, so the mask is a single pin with ALL_LOW.
  // (GPIO0 remains a power-on/reset button, as in the original firmware.)
  uint64_t wakeMask = (1ULL << (uint64_t)OTA_BTN_PIN);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
#endif
}