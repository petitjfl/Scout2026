/* Lanterne des Portées — Wemos D1 mini (ESP8266) + anneau NeoPixel
   « Symphonie des Échos Noirs » — camp d'été 2026, Meute 6A St-Paul d'Aylmer.

   Même matériel qu'une balise mais ne suit PAS le cycle bleu jour/nuit : la
   lanterne est une source de lumière. Elle reste éveillée, écoute les commandes
   en continu et conserve son dernier mode (persisté en EEPROM).

   Modes : OFF, CANDLE (bougie chaude, défaut), ALERT (pulsation rouge),
   WHITE (blanc chaud brillant — jamais bleuté), REVELATION (ombres musicales,
   finale phase 2), WINDIGO (fondu au noir puis clignote rouge, phase 3).

   Protocole ESP-NOW (canal 9) : voir PROTOCOL.md à la racine du dépôt.
     - heartbeat : "HB|<seq>|<id>|<mode>|<batt_mv>"
     - commande  : "CMD|<NOM>|<arg>"   (unicast du master)
     - ack       : "ACK|<id>|<NOM>|<status>"

   L'identité et le mode de build sont injectés par platformio.ini :
     DEVICE_ID / DEVICE_NAME : un environnement par lanterne (plage 10–19).
     DEBUG_MODE=1 : série active (dev). 0 avant le camp.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <FastLED.h>
#include <EEPROM.h>

// Fallbacks si compilé hors des environnements nommés de platformio.ini.
#ifndef DEBUG_MODE
#define DEBUG_MODE 1
#endif
#ifndef DEVICE_ID
#define DEVICE_ID 10
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "Lanterne_1"
#endif

const bool DEBUG = DEBUG_MODE;

#define WIFI_CHANNEL 9    // canal imposé, doit correspondre au master

// ---- Hardware (same wiring as a balise) ----
#define LED_PIN D4
#define NUM_LEDS 6
CRGB leds[NUM_LEDS];

// ---- Brightness caps per mode (0..255) ----
const uint8_t CANDLE_MAX = 200;   // candle never goes to harsh full white
const uint8_t WHITE_LEVEL = 255;  // bright white
const uint8_t ALERT_MAX = 230;

// Warm white tint (R,G,B). High R, medium G, low B -> warm, never blue.
const CRGB WARM_WHITE = CRGB(255, 165, 70);
// Candle base color (deep warm orange).
const CRGB CANDLE_COLOR = CRGB(255, 100, 20);

// ---- Heartbeat ----
// Must be well under the master's 10s presence timeout.
const unsigned long HEARTBEAT_INTERVAL_MS = 5000;
unsigned long lastHeartbeatSent = 0;
uint32_t hbSeq = 0;

// ---- Battery divider (same as balise) ----
const float ADC_REF_VOLTAGE = 3.2;
const int ADC_MAX = 1023;
const float R1 = 100000.0;
const float R2 = 220000.0;

// ---- Modes ----
// 0..3 = usage courant ; 4..5 = modes de la finale.
enum Mode {
  MODE_OFF = 0, MODE_CANDLE = 1, MODE_ALERT = 2, MODE_WHITE = 3,
  MODE_REVELATION = 4, // ombres musicales dansantes (phase 2)
  MODE_WINDIGO = 5     // fondu lent vers le noir puis clignote rouge (phase 3)
};
const int MODE_MAX = MODE_WINDIGO;
volatile Mode currentMode = MODE_CANDLE;
unsigned long modeStartMs = 0; // horloge d'effet (réinitialisée à chaque commande)

uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Set from the ESP-NOW recv callback (SYS context) to request an EEPROM write.
// NEVER write flash inside the callback on the ESP8266: it can conflict with
// the WiFi stack and crash ("Soft WDT reset"). loop() flushes this flag.
volatile bool stateDirty = false;

// ---------------- EEPROM persistence ----------------
// layout: 0..3 magic (uint32), 4 mode (uint8)
const uint32_t EEPROM_MAGIC = 0x1A47E001;
const int EEPROM_SIZE = 32;

void saveState() {
  if (DEBUG) Serial.println("Saving state to EEPROM");
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = EEPROM_MAGIC;
  EEPROM.put(0, magic);
  uint8_t m = (uint8_t)currentMode;
  EEPROM.put(4, m);
  EEPROM.commit();
  EEPROM.end();
}

void loadState() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == EEPROM_MAGIC) {
    uint8_t m = MODE_CANDLE;
    EEPROM.get(4, m);
    if (m > MODE_MAX) m = MODE_CANDLE; // guard against garbage
    currentMode = (Mode)m;
    if (DEBUG) { Serial.print("Loaded mode="); Serial.println((int)currentMode); }
  } else {
    if (DEBUG) Serial.println("No EEPROM magic, default CANDLE");
    currentMode = MODE_CANDLE;
  }
  EEPROM.end();
}

void setMode(Mode m) {
  modeStartMs = millis(); // reset effect clock on every command (allows re-trigger)
  if (currentMode == m) return;
  currentMode = m;
  stateDirty = true; // persist new mode (written by loop, not here)
  if (DEBUG) { Serial.print("Mode -> "); Serial.println((int)m); }
}

// ---------------- Battery ----------------
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

// "Révélation" : lueur chaude/mystique qui se déplace autour de l'anneau
// (les ombres musicales dansent).
void effectRevelation() {
  uint32_t t = millis();
  for (int i = 0; i < NUM_LEDS; i++) {
    float ph = t / 650.0f + i * 0.95f;
    float v = (sinf(ph) + 1.0f) / 2.0f;            // 0..1 déphasé par LED
    CRGB c = blend(CRGB(255, 120, 30), CRGB(120, 0, 150), (uint8_t)(v * 140));
    c.nscale8_video((uint8_t)(30 + v * 190));
    leds[i] = c;
  }
  FastLED.show();
}

// "Windigo proche" : fondu lent vers le noir (~4 s) puis clignotement rouge.
void effectWindigo() {
  unsigned long dt = millis() - modeStartMs;
  const unsigned long FADE_MS = 4000;
  if (dt < FADE_MS) {
    uint8_t b = 255 - (uint8_t)(255UL * dt / FADE_MS);
    for (int i = 0; i < NUM_LEDS; i++) { leds[i] = WARM_WHITE; leds[i].nscale8_video(b); }
  } else {
    uint32_t t = (dt - FADE_MS) % 900;
    float v = (sinf((t / 900.0f) * 2.0f * PI - PI / 2.0f) + 1.0f) / 2.0f;
    uint8_t b = (uint8_t)(15 + v * 230);
    for (int i = 0; i < NUM_LEDS; i++) { leds[i] = CRGB::Red; leds[i].nscale8_video(b); }
  }
  FastLED.show();
}

// Warm candle: per-LED brightness flicker that drifts toward random targets.
void effectCandle() {
  static uint8_t level[NUM_LEDS];
  static uint8_t target[NUM_LEDS];
  static uint32_t lastStep = 0;
  static bool init = false;
  if (!init) {
    for (int i = 0; i < NUM_LEDS; i++) { level[i] = 140; target[i] = 180; }
    init = true;
  }
  uint32_t nowMs = millis();
  if (nowMs - lastStep >= 35) {        // ~28 fps flicker
    lastStep = nowMs;
    for (int i = 0; i < NUM_LEDS; i++) {
      // occasionally pick a new flicker target
      if (random8() < 40) target[i] = random8(90, CANDLE_MAX);
      // ease current level toward target
      if (level[i] < target[i])      level[i] += min<uint8_t>(8, target[i] - level[i]);
      else if (level[i] > target[i]) level[i] -= min<uint8_t>(8, level[i] - target[i]);
      leds[i] = CANDLE_COLOR;
      leds[i].nscale8_video(level[i]);
    }
    FastLED.show();
  }
}

// Red breathing pulse for alerts.
void effectAlert() {
  static uint32_t t0 = millis();
  uint32_t t = (millis() - t0) % 1200;          // 1.2s period
  float phase = t / 1200.0f;
  float val = (sinf(phase * 2.0f * PI - PI / 2.0f) + 1.0f) / 2.0f;
  uint8_t b = (uint8_t)(20 + val * (ALERT_MAX - 20));
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Red;
    leds[i].nscale8_video(b);
  }
  FastLED.show();
}

// Steady bright warm white.
void effectWhite() {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = WARM_WHITE;
    leds[i].nscale8_video(WHITE_LEVEL);
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

// Optional "TO|<id>|" addressing prefix (kept for protocol compatibility).
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

  // We only act on commands. HB/ACK from others are ignored.
  if (!msg.startsWith("CMD|")) return;

  int toId = -1, posAfterTo = -1;
  bool hasTo = parseToField(msg, toId, posAfterTo);
  if (hasTo && toId != DEVICE_ID) {
    if (DEBUG) Serial.println("CMD not for me, ignored");
    return;
  }

  String payload = hasTo ? msg.substring(posAfterTo) : msg.substring(4);
  int p = payload.indexOf('|');
  String cmd = (p > 0) ? payload.substring(0, p) : payload;
  String arg = (p > 0) ? payload.substring(p + 1) : "";

  if (cmd == "SETMODE") {
    int m = arg.toInt();
    if (m < MODE_OFF || m > MODE_MAX) { sendAck(mac, "SETMODE", "ERR"); return; }
    setMode((Mode)m);
    sendAck(mac, "SETMODE", "OK");
  } else if (cmd == "FORCE_OFF") {
    setMode(MODE_OFF);   sendAck(mac, "FORCE_OFF", "OK");
  } else if (cmd == "FORCE_CANDLE") {
    setMode(MODE_CANDLE); sendAck(mac, "FORCE_CANDLE", "OK");
  } else if (cmd == "FORCE_ALERT") {
    setMode(MODE_ALERT);  sendAck(mac, "FORCE_ALERT", "OK");
  } else if (cmd == "FORCE_GLITCH" || cmd == "FINAL") {
    // The balises "glitch"; the lantern's equivalent for the finale is the
    // red ALERT pulse. Accept the balise command so one "glitch all" / finale
    // triggers every device at once. Ack with the received name so the master
    // matches its pending command.
    setMode(MODE_ALERT);  sendAck(mac, cmd.c_str(), "OK");
  } else if (cmd == "FORCE_WHITE") {
    setMode(MODE_WHITE);  sendAck(mac, "FORCE_WHITE", "OK");
  } else if (cmd == "FORCE_REVELATION") {
    setMode(MODE_REVELATION); sendAck(mac, "FORCE_REVELATION", "OK");
  } else if (cmd == "FORCE_WINDIGO") {
    setMode(MODE_WINDIGO);    sendAck(mac, "FORCE_WINDIGO", "OK");
  } else {
    sendAck(mac, cmd.c_str(), "ERR");  // unknown command for a lantern
  }
}

void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (DEBUG) { Serial.print("onDataSent status="); Serial.println(sendStatus); }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(WIFI_CHANNEL);       // set channel before init (ESP8266)
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

  loadState();      // restore last mode
  modeStartMs = millis();
  setupEspNow();

  // A lantern is mains/visible: no deep sleep so it always shows light and
  // listens for commands reliably. Disable WiFi modem sleep for snappy RX.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  if (DEBUG) Serial.println("Lanterne ready");
}

void loop() {
  // Flush any EEPROM write requested by the ESP-NOW callback (safe here).
  if (stateDirty) { stateDirty = false; saveState(); }

  // Periodic heartbeat so the master sees us as present.
  if (millis() - lastHeartbeatSent >= HEARTBEAT_INTERVAL_MS) sendHeartbeat();

  // Render current mode.
  switch (currentMode) {
    case MODE_OFF:        effectOff();    delay(50); break;
    case MODE_CANDLE:     effectCandle();            break; // self-timed internally
    case MODE_ALERT:      effectAlert();             break;
    case MODE_WHITE:      effectWhite();  delay(20); break;
    case MODE_REVELATION: effectRevelation();        break;
    case MODE_WINDIGO:    effectWindigo();           break;
    default:              effectCandle();            break;
  }

  delay(2); // keep the ESP8266 cooperative
}
