/* Master ESP8266 de test — une balise, interface HTTP légère.
   Compatible avec le protocole ESP-NOW du master ESP32, canal 9. */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <espnow.h>

#define WIFI_CHANNEL 9
#define AP_SSID "MASTER_TEST"
#define AP_PASS "masterpass"

ESP8266WebServer server(80);

struct BeaconState {
  bool known;
  uint8_t mac[6];
  uint8_t id;
  int mode;
  int batteryMv;
  uint32_t sequence;
  unsigned long lastSeenMs;
  char lastAck[32];
};

BeaconState beacon = {};

// Callback ESP-NOW -> loop. Une seule case évite String/HTTP dans le contexte
// WiFi et suffit pour un master de banc avec une balise.
volatile bool rxReady = false;
volatile uint8_t rxLen = 0;
uint8_t rxMac[6];
char rxData[251];

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Master test</title><style>
body{font:16px system-ui;background:#071426;color:#eaf6ff;margin:0;padding:18px}
main{max-width:620px;margin:auto}h1{font-size:1.5rem}#state{background:#10243d;
padding:14px;border-radius:12px;margin:14px 0;line-height:1.6}button{border:0;
border-radius:9px;padding:12px 14px;margin:4px;background:#1677ff;color:white;
font-weight:700}.off{background:#38485c}.glitch{background:#00a5ad}.white{background:#dcefff;color:#123}
#msg{min-height:1.4em;color:#76e6e8}</style></head><body><main>
<h1>Master ESP8266 — banc balise</h1><div id="state">Recherche d'une balise…</div>
<div><button onclick="send('FORCE_IDLE')">Normal</button>
<button class="glitch" onclick="send('FORCE_GLITCH')">Glitch</button>
<button onclick="send('FORCE_BLUE')">Bleu</button>
<button onclick="send('FORCE_BLUE_SLOW')">Bleu lent</button>
<button class="white" onclick="send('FORCE_RECHARGE')">Recharge</button>
<button onclick="send('FORCE_AMBER')">Ambre</button>
<button onclick="send('FORCE_ALERT')">Alerte</button>
<button onclick="send('FORCE_RAINBOW')">Arc-en-ciel</button>
<button class="off" onclick="send('FORCE_OFF')">Éteindre</button></div>
<p id="msg"></p><script>
const modes=['Off','Idle','Glitch','Ambre','Bleu','Alerte','Arc-en-ciel','Bleu lent','Recharge'];
async function poll(){try{let s=await(await fetch('/status')).json();
document.querySelector('#state').innerHTML=s.known?`Balise <b>${s.id}</b> — ${modes[s.mode]||s.mode}<br>
Batterie: ${(s.battery_mv/1000).toFixed(2)} V — vue il y a ${Math.round(s.age_ms/1000)} s<br>Dernier ACK: ${s.ack||'—'}`:
'Recherche d\'une balise…';}catch(e){}setTimeout(poll,1000)}
async function send(c){let r=await fetch('/cmd?name='+encodeURIComponent(c),{method:'POST'});
document.querySelector('#msg').textContent=await r.text();setTimeout(()=>msg.textContent='',2500)}poll();
</script></main></body></html>)HTML";

String macText(const uint8_t *mac) {
  char out[18];
  snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(out);
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (rxReady) return;
  uint8_t safeLen = min((int)len, 250);
  memcpy(rxMac, mac, 6);
  memcpy(rxData, data, safeLen);
  rxData[safeLen] = '\0';
  rxLen = safeLen;
  rxReady = true;
}

void onSent(uint8_t *, uint8_t status) {
  if (status != 0) strncpy(beacon.lastAck, "envoi radio échoué", sizeof(beacon.lastAck));
}

bool ensurePeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist((uint8_t*)mac)) return true;
  return esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, nullptr, 0) == 0;
}

void processPacket() {
  if (!rxReady) return;
  noInterrupts();
  char msg[251];
  uint8_t mac[6];
  uint8_t len = rxLen;
  memcpy(msg, rxData, len + 1);
  memcpy(mac, rxMac, 6);
  rxReady = false;
  interrupts();

  if (strncmp(msg, "HB|", 3) == 0) {
    unsigned long seq;
    int id, mode, batt;
    if (sscanf(msg, "HB|%lu|%d|%d|%d", &seq, &id, &mode, &batt) == 4) {
      memcpy(beacon.mac, mac, 6);
      beacon.known = true;
      beacon.id = id;
      beacon.mode = mode;
      beacon.batteryMv = batt;
      beacon.sequence = seq;
      beacon.lastSeenMs = millis();
      ensurePeer(mac);
    }
  } else if (strncmp(msg, "ACK|", 4) == 0) {
    char id[5] = {}, command[17] = {}, status[8] = {};
    if (sscanf(msg, "ACK|%4[^|]|%16[^|]|%7s", id, command, status) >= 2) {
      snprintf(beacon.lastAck, sizeof(beacon.lastAck), "%s: %s", command, status);
    }
  }
}

void handleStatus() {
  String json = "{\"known\":";
  json += beacon.known ? "true" : "false";
  if (beacon.known) {
    json += ",\"id\":" + String(beacon.id);
    json += ",\"mode\":" + String(beacon.mode);
    json += ",\"battery_mv\":" + String(beacon.batteryMv);
    json += ",\"age_ms\":" + String(millis() - beacon.lastSeenMs);
    json += ",\"ack\":\"" + String(beacon.lastAck) + "\"";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleCommand() {
  if (!beacon.known) { server.send(409, "text/plain", "Aucune balise détectée"); return; }
  String command = server.arg("name");
  const char *allowed[] = {"FORCE_IDLE", "FORCE_GLITCH", "FORCE_BLUE",
    "FORCE_BLUE_SLOW", "FORCE_RECHARGE", "FORCE_AMBER", "FORCE_ALERT",
    "FORCE_RAINBOW", "FORCE_OFF"};
  bool valid = false;
  for (const char *item : allowed) if (command == item) valid = true;
  if (!valid) { server.send(400, "text/plain", "Commande invalide"); return; }

  String payload = "CMD|" + command;
  int result = ensurePeer(beacon.mac) ?
    esp_now_send(beacon.mac, (uint8_t*)payload.c_str(), payload.length() + 1) : -1;
  if (result == 0) {
    snprintf(beacon.lastAck, sizeof(beacon.lastAck), "%s: envoyé", command.c_str());
    server.send(200, "text/plain", "Commande envoyée");
  } else server.send(503, "text/plain", "Échec de l'envoi ESP-NOW");
}

void setup() {
  Serial.begin(115200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
  WiFi.disconnect();
  wifi_set_channel(WIFI_CHANNEL);

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/cmd", HTTP_POST, handleCommand);
  server.begin();
  Serial.print("MASTER_TEST ready: http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  processPacket();
  server.handleClient();
  delay(2);
}
