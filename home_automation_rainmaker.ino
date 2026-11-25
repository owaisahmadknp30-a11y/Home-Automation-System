#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include <WebServer.h>
#include <AceButton.h>
#include <Preferences.h>
using namespace ace_button;

// ================= USER CONFIG =================
#define SWITCH_IS_LATCHING 0   // 1 = toggle switches, 0 = push buttons

// Your WiFi (kept as-is; choose this same network in the RainMaker app during setup)
const char* ssid     = "--WiFi Name(4G)--";
const char* password = "--Password--";

// Fixed IP (unchanged)
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);        // needed so RainMaker cloud can be resolved
IPAddress dns2(1, 1, 1, 1);

// RainMaker provisioning
const char* service_name = "PROV_HomeAuto";
const char* pop          = "abcd1234";   // proof-of-possession, change if you like
// ===============================================

// Relay & Button pins (unchanged). Relay modules are active LOW.
const int relayPins[]  = {23, 19, 18, 5};
const int buttonPins[] = {13, 12, 14, 27};
const int numRelays = 4;
bool relayStates[numRelays] = {false, false, false, false};

const int statusLed  = 2;      // shows "restore" flag
const int resetPin   = 0;      // BOOT button: 3s = WiFi reset, 10s = factory reset

bool restoreFlag = false;
Preferences prefs;

// RainMaker objects
Node my_node;
Switch* rmSwitch[numRelays];
int relayIndex[numRelays] = {0, 1, 2, 3};

// Buttons
AceButton buttons[numRelays];

WebServer server(80);
bool serverStarted = false;

// ========== Storage (NVS) ==========
void saveRelayStates() {
  prefs.begin("home", false);
  for (int i = 0; i < numRelays; i++) prefs.putBool(("s" + String(i)).c_str(), relayStates[i]);
  prefs.end();
}

void saveRestoreFlag() {
  prefs.begin("home", false);
  prefs.putBool("flag", restoreFlag);
  prefs.end();
}

void loadStorage() {
  prefs.begin("home", true);
  restoreFlag = prefs.getBool("flag", false);
  if (restoreFlag) {
    for (int i = 0; i < numRelays; i++) relayStates[i] = prefs.getBool(("s" + String(i)).c_str(), false);
  }
  prefs.end();
}

// ========== Central relay control ==========
void setRelay(int i, bool state, bool save = true) {
  if (i < 0 || i >= numRelays) return;
  relayStates[i] = state;
  digitalWrite(relayPins[i], state ? LOW : HIGH);
  if (rmSwitch[i]) rmSwitch[i]->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, state);
  if (save && restoreFlag) saveRelayStates();
}

// ========== RainMaker callbacks ==========
void write_callback(Device* device, Param* param, const param_val_t val, void* priv_data, write_ctx_t* ctx) {
  int i = *((int*)priv_data);
  if (strcmp(param->getParamName(), ESP_RMAKER_DEF_POWER_NAME) == 0) {
    relayStates[i] = val.val.b;
    digitalWrite(relayPins[i], relayStates[i] ? LOW : HIGH);
    param->updateAndReport(val);
    if (restoreFlag) saveRelayStates();
  }
}

void sysProvEvent(arduino_event_t* sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.printf("\nProvisioning started: connect with RainMaker app. Name: %s  POP: %s\n", service_name, pop);
      WiFiProv.printQR(service_name, pop, "ble");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
      if (!serverStarted) { server.begin(); serverStarted = true; Serial.println("Web server started"); }
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      Serial.println("WiFi credentials received from app");
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      Serial.println("Provisioning failed - check password / reset and retry");
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("Provisioning successful");
      break;
    default: break;
  }
}

// ========== Physical buttons ==========
void buttonHandler(AceButton* button, uint8_t eventType, uint8_t buttonState) {
  int i = button->getId();
  if (eventType == AceButton::kEventPressed) {
    setRelay(i, !relayStates[i]);
  }
#if SWITCH_IS_LATCHING
  else if (eventType == AceButton::kEventReleased) {
    setRelay(i, !relayStates[i]);
  }
#endif
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);

  loadStorage();

  for (int i = 0; i < numRelays; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayStates[i] ? LOW : HIGH);
  }

  ButtonConfig* config = ButtonConfig::getSystemButtonConfig();
  config->setEventHandler(buttonHandler);
  for (int i = 0; i < numRelays; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    buttons[i].init(buttonPins[i], HIGH, i);
  }

  pinMode(statusLed, OUTPUT);
  digitalWrite(statusLed, restoreFlag);
  pinMode(resetPin, INPUT);

  // ---- RainMaker node + 4 switches ----
  my_node = RMaker.initNode("ESP32 Home Automation");
  const char* names[] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};
  for (int i = 0; i < numRelays; i++) {
    rmSwitch[i] = new Switch(names[i], &relayIndex[i], relayStates[i]);
    rmSwitch[i]->addCb(write_callback);
    my_node.addDevice(*rmSwitch[i]);
  }

  RMaker.enableTZService();
  RMaker.enableSchedule();      // schedules from the app = automation
  RMaker.start();

  // ---- Network ----
  WiFi.onEvent(sysProvEvent);
  // WiFi.config(local_IP, gateway, subnet, dns1, dns2);   // fixed IP kept
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
                          NETWORK_PROV_SECURITY_1, pop, service_name);

  // ---- Web routes ----
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/status", handleStatus);
  server.on("/alloff", handleAllOff);
  server.on("/eepromflag", handleEEPROMFlag);
}

// ========== Loop ==========
void loop() {
  if (serverStarted) server.handleClient();

  for (int i = 0; i < numRelays; i++) buttons[i].check();

  // BOOT button: hold 3s = WiFi reset, hold 10s = factory reset
  static uint32_t pressStart = 0;
  if (digitalRead(resetPin) == LOW) {
    if (pressStart == 0) pressStart = millis();
  } else if (pressStart) {
    uint32_t held = millis() - pressStart;
    pressStart = 0;
    if (held > 10000) { Serial.println("Factory reset"); RMakerFactoryReset(2); }
    else if (held > 3000) { Serial.println("WiFi reset"); RMakerWiFiReset(2); }
  }

  digitalWrite(statusLed, restoreFlag);
}

// ========== Web handlers ==========
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head>
<title>ESP32 Home Automation</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body{font-family:Arial,sans-serif;margin:0;background:#f0f0f0}
 h2{text-align:center;padding:20px 0;color:#333}
 .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:15px;padding:0 20px}
 .card{background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,.2);text-align:center}
 .btn{padding:12px 20px;border:none;border-radius:8px;font-size:16px;cursor:pointer;width:100%;color:#fff}
 .on{background:#28a745}.off{background:#6c757d}
 .wide{margin:20px;padding:15px;color:#fff;border:none;border-radius:8px;font-size:18px;width:calc(100% - 40px);cursor:pointer}
 .footer{text-align:center;padding:15px;font-size:14px;color:#666}
</style></head><body>
<h2>ESP32 Home Automation</h2>
<div class="grid">
 <div class="card"><button id="btn0" class="btn off" onclick="t(0)">Relay 1</button></div>
 <div class="card"><button id="btn1" class="btn off" onclick="t(1)">Relay 2</button></div>
 <div class="card"><button id="btn2" class="btn off" onclick="t(2)">Relay 3</button></div>
 <div class="card"><button id="btn3" class="btn off" onclick="t(3)">Relay 4</button></div>
</div>
<button class="wide" style="background:#007bff" onclick="f('/alloff')">ALL OFF</button>
<button class="wide" style="background:#6c757d" onclick="f('/eepromflag')">Restore on boot: <span id="flagState">--</span></button>
<div class="footer">Created by Tech Studycell</div>
<script>
function f(u){fetch(u).then(r=>r.json()).then(render)}
function t(i){f('/toggle?relay='+i)}
function render(d){
 for(let i=0;i<4;i++){
  let b=document.getElementById('btn'+i), on=d['relay'+i];
  b.className='btn '+(on?'on':'off');
  b.textContent='Relay '+(i+1)+(on?' (ON)':' (OFF)');
 }
 document.getElementById('flagState').textContent=d.eeprom?'ON':'OFF';
}
function poll(){fetch('/status').then(r=>r.json()).then(render)}
setInterval(poll,2000);poll();
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleStatus() {
  String json = "{";
  for (int i = 0; i < numRelays; i++)
    json += "\"relay" + String(i) + "\":" + (relayStates[i] ? "true" : "false") + ",";
  json += "\"eeprom\":" + String(restoreFlag ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleToggle() {
  if (server.hasArg("relay")) {
    int r = server.arg("relay").toInt();
    if (r >= 0 && r < numRelays) setRelay(r, !relayStates[r]);
  }
  handleStatus();
}

void handleAllOff() {
  for (int i = 0; i < numRelays; i++) setRelay(i, false, false);
  if (restoreFlag) saveRelayStates();
  handleStatus();
}

void handleEEPROMFlag() {
  restoreFlag = !restoreFlag;
  saveRestoreFlag();
  if (restoreFlag) saveRelayStates();   // store current state when enabling
  handleStatus();
}
