/* Médaillon du Dernier Accord — Wemos D1 mini (ESP8266) + anneau NeoPixel
   « Symphonie des Échos Noirs » — camp d'été 2026, Meute 6A St-Paul d'Aylmer.

   Même matériel qu'une lanterne. Reste éveillé et écoute les commandes. Son
   effet signature se déclenche au contact de la Baguette (finale, phase 4) :

     TRIGGER : « s'illumine » — comète qui se poursuit autour de l'anneau,
               dérive de couleur, montée vers un sommet doré/blanc brillant.
     STOP    : retour au repos (éteint).

   Protocole ESP-NOW (canal 9) : voir PROTOCOL.md à la racine du dépôt.
     - heartbeat : "HB|<seq>|<id>|<mode>|<batt_mv>"
     - commande  : "CMD|<NOM>|<arg>"   (unicast du master)
     - ack       : "ACK|<id>|<NOM>|<status>"

   L'identité et le mode de build sont injectés par platformio.ini :
     DEVICE_ID / DEVICE_NAME : un environnement par médaillon (plage 20–29).
     DEBUG_MODE=1 : série active (dev). 0 avant le camp.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <FastLED.h>

// Fallbacks si compilé hors des environnements nommés de platformio.ini.
#ifndef DEBUG_MODE
#define DEBUG_MODE 1
#endif
#ifndef DEVICE_ID
#define DEVICE_ID 20
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "Medaillon_1"
#endif

const bool DEBUG = DEBUG_MODE;

#define WIFI_CHANNEL 9    // canal imposé, doit correspondre au master

// ---- Hardware ----
#define LED_PIN D4
#define NUM_LEDS 12       // médaillon ring (adjust to your ring)
CRGB leds[NUM_LEDS];

// ---- Heartbeat ----
const unsigned long HEARTBEAT_INTERVAL_MS = 5000; // < master 10s presence timeout
unsigned long lastHeartbeatSent = 0;
uint32_t hbSeq = 0;

// ---- Battery divider (same as balise/lanterne) ----
const float ADC_REF_VOLTAGE = 3.2;
const int ADC_MAX = 1023;
const float R1 = 100000.0;
const float R2 = 220000.0;

// ---- Modes ----
enum Mode { MODE_OFF = 0, MODE_ILLUMINATE = 1 };
volatile Mode currentMode = MODE_OFF;
unsigned long modeStartMs = 0; // effect clock (reset on each command)

uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void setMode(Mode m) {
  modeStartMs = millis();
  currentMode = m;
  if (DEBUG) { Serial.print("Mode -> "); Serial.println((int)m); }
}

int readBatteryMv() {
  int raw = analogRead(A0);
  float vA0 = (raw * (ADC_REF_VOLTAGE / ADC_MAX));
  float battV = vA0 * ((R1 + R2) / R2);
  return int(battV * 1000.0);
}

// ---------------- Effects ----------------
void effectOff() {
  FastLED.clear();
  FastLED.show();
}

// Comet / dragon chasing its tail: a bright head with a fading tail circles the
// ring, its hue drifting. Over ~7s the whole thing brightens and desaturates
// toward a golden-white climax, then holds bright with a gentle shimmer.
void effectIlluminate() {
  const unsigned long BUILD_MS = 7000;
  unsigned long dt = millis() - modeStartMs;
  float prog = (dt < BUILD_MS) ? (float)dt / BUILD_MS : 1.0f;   // 0..1

  if (prog >= 1.0f) {
    // Summit: full bright golden-white glow with a soft shimmer.
    for (int i = 0; i < NUM_LEDS; i++) {
      uint8_t sh = 205 + (uint8_t)(50 * ((sinf(millis() / 130.0f + i * 0.7f) + 1.0f) / 2.0f));
      CRGB c = CRGB(255, 220, 150);
      c.nscale8_video(sh);
      leds[i] = c;
    }
    FastLED.show();
    return;
  }

  // Build-up: moving comet with a growing tail and rising intensity.
  float speed = 0.004f + prog * 0.018f;                 // ring turns faster as it builds
  float head = fmodf(millis() * speed, (float)NUM_LEDS);
  uint8_t hue = (uint8_t)(millis() / 22);               // gradual colour drift
  uint8_t sat = (uint8_t)(255 * (1.0f - prog * 0.8f));  // desaturate toward white
  float tailLen = 2.0f + prog * (NUM_LEDS - 2.0f);      // longer tail near climax
  uint8_t baseBright = (uint8_t)(70 + prog * 185);

  for (int i = 0; i < NUM_LEDS; i++) {
    float d = fabsf(i - head);
    d = min(d, (float)NUM_LEDS - d);                    // wrap-around distance
    float tail = max(0.0f, 1.0f - d / tailLen);
    CRGB c = CHSV(hue + i * 5, sat, 255);
    c.nscale8_video((uint8_t)(baseBright * tail));
    // warm-white core that grows with progress
    CRGB core = CRGB(255, 220, 150);
    core.nscale8_video((uint8_t)(prog * 255 * tail));
    c += core;
    leds[i] = c;
  }
  FastLED.show();
}

// ---------------- ESP-NOW send helpers ----------------
void sendToMac(const uint8_t *mac, const char* msg) {
  esp_now_send((uint8_t*)mac, (uint8_t*)msg, strlen(msg) + 1);
}
void sendBroadcast(const char* msg) {
  esp_now_send(broadcastAddress, (uint8_t*)msg, strlen(msg) + 1);
}
void sendAck(const uint8_t *mac, const char* cmd, const char* status) {
  char buf[64];
  snprintf(buf, sizeof(buf), "ACK|%u|%s|%s", (unsigned)DEVICE_ID, cmd, status);
  sendToMac(mac, buf);
  if (DEBUG) { Serial.print("Sent ACK -> "); Serial.println(buf); }
}
void sendHeartbeat() {
  int batt = readBatteryMv();
  char buf[80];
  snprintf(buf, sizeof(buf), "HB|%lu|%u|%u|%d",
           (unsigned long)hbSeq, (unsigned)DEVICE_ID, (unsigned)currentMode, batt);
  sendBroadcast(buf);
  lastHeartbeatSent = millis();
  hbSeq++;
  if (DEBUG) {
    Serial.print("HB seq="); Serial.print(hbSeq - 1);
    Serial.print(" mode="); Serial.print((int)currentMode);
    Serial.print(" batt="); Serial.println(batt);
  }
}

bool parseToField(const String &msg, int &toId, int &posAfterTo) {
  toId = -1; posAfterTo = -1;
  int idx = msg.indexOf("TO|");
  if (idx < 0) return false;
  int p = idx + 3;
  int p2 = msg.indexOf('|', p);
  if (p2 < 0) return false;
  toId = msg.substring(p, p2).toInt();
  posAfterTo = p2 + 1;
  return true;
}

// ---------------- ESP-NOW recv callback (SYS context: stay light) ----------------
void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
  // Bound the copy: don't trust a NUL terminator from the sender.
  char tmp[251];
  int n = (len < (int)sizeof(tmp) - 1) ? len : (int)sizeof(tmp) - 1;
  memcpy(tmp, incomingData, n);
  tmp[n] = 0;
  String msg = String(tmp);

  if (DEBUG) { Serial.print("RX: "); Serial.println(msg); }
  if (!msg.startsWith("CMD|")) return;

  int toId = -1, posAfterTo = -1;
  bool hasTo = parseToField(msg, toId, posAfterTo);
  if (hasTo && toId != DEVICE_ID) return;

  String payload = hasTo ? msg.substring(posAfterTo) : msg.substring(4);
  int p = payload.indexOf('|');
  String cmd = (p > 0) ? payload.substring(0, p) : payload;

  if (cmd == "TRIGGER") {
    setMode(MODE_ILLUMINATE); sendAck(mac, "TRIGGER", "OK");
  } else if (cmd == "STOP" || cmd == "FORCE_OFF") {
    setMode(MODE_OFF);        sendAck(mac, cmd.c_str(), "OK");
  } else {
    sendAck(mac, cmd.c_str(), "ERR");
  }
}

void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (DEBUG) { Serial.print("onDataSent status="); Serial.println(sendStatus); }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(WIFI_CHANNEL);
  if (esp_now_init() != 0) {
    if (DEBUG) Serial.println("ESP-NOW init failed, restarting");
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, NULL, 0);
}

void setup() {
  if (DEBUG) Serial.begin(115200);
  delay(50);
  if (DEBUG) {
    Serial.printf("\n%s (id=%u) — MAC: %s\n", DEVICE_NAME, (unsigned)DEVICE_ID,
                  WiFi.macAddress().c_str());
  }

  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
  FastLED.clear();
  FastLED.show();

  modeStartMs = millis();
  setupEspNow();
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // stay responsive

  if (DEBUG) Serial.println("Medaillon ready");
}

void loop() {
  if (millis() - lastHeartbeatSent >= HEARTBEAT_INTERVAL_MS) sendHeartbeat();

  switch (currentMode) {
    case MODE_ILLUMINATE: effectIlluminate(); break;
    case MODE_OFF:
    default:              effectOff(); delay(40); break;
  }

  delay(2);
}
