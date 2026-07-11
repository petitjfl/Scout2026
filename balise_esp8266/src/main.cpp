/* Balise de Veille — Wemos D1 mini (ESP8266) + anneau NeoPixel
   « Symphonie des Échos Noirs » — camp d'été 2026, Meute 6A St-Paul d'Aylmer.

   Les quatre balises forment le Cercle de Veille autour du camp : bleu pulsant
   faible la nuit (visible des tentes), deep-sleep le jour (économie batterie),
   et modes forcés pilotés par le master pendant les veillées et la finale.

   Protocole ESP-NOW (canal 9) : voir PROTOCOL.md à la racine du dépôt.
     - heartbeat : "HB|<seq>|<id>|<mode>|<batt_mv>"
     - commande  : "CMD|<NOM>|<arg>"   (unicast du master)
     - ack       : "ACK|<id>|<NOM>|<status>"

   L'identité et le mode de build sont injectés par platformio.ini :
     DEVICE_ID / DEVICE_NAME : un environnement par balise physique.
     DEBUG_MODE=1 : série active, pas de deep-sleep (dev). 0 avant le camp.
*/

#include <Arduino.h>
#ifdef BALISE_ESP32
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#else
#include <ESP8266WiFi.h>
#include <espnow.h>
#endif
#include <FastLED.h>
#include <EEPROM.h>

// Fallbacks si compilé hors des environnements nommés de platformio.ini.
#ifndef DEBUG_MODE
#define DEBUG_MODE 1
#endif
#ifndef DEVICE_ID
#define DEVICE_ID 1
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "Balise_1"
#endif

const bool DEBUG = DEBUG_MODE;
// Deep-sleep = LE JOUR seulement (économie batterie, les balises ne servent pas).
// La NUIT elles restent éveillées et allumées faiblement pour "veiller" sur les
// tentes, et rester réactives (la finale se joue le soir). La batterie tient 24 h.
const bool USE_DEEP_SLEEP = !DEBUG;

#define WIFI_CHANNEL 9  // canal imposé, doit correspondre au master

#ifndef LED_PIN
#define LED_PIN D4
#endif
#ifndef BATTERY_PIN
#define BATTERY_PIN A0
#endif
#define NUM_LEDS 6
CRGB leds[NUM_LEDS];

// Doit rester nettement sous le timeout de présence du master (10 s).
const unsigned long HEARTBEAT_INTERVAL_MS = 5000;

// Deep sleep / wake window parameters (production)
const unsigned long WAKE_WINDOW_MS = 2000;  // listen 2s after wake
const unsigned long WAKE_INTERVAL_S = 60;   // sleep 60s between windows

const unsigned long HEARTBEAT_TIMEOUT_MS = 15000;
const unsigned long GROUP_WINDOW_MS = 10000;
const int GROUP_MIN_SEEN = 2;

const unsigned long TIME_SYNC_TIMEOUT_MS = 600000;
unsigned long syncedEpoch = 0;
unsigned long syncedMillis = 0;

// Lecture batterie 18650 via diviseur EXTERNE sur A0 (le D1 mini de ce lot n'a
// pas de diviseur interne : l'ADC est en pleine échelle brute ~1,0 V).
//   +batterie --[ R_TOP 330k ]--(A0)--[ R_BOTTOM 100k ]-- GND
// Point milieu = Vbat * 100/(330+100) = Vbat * 0,2326  -> 0,977 V à 4,2 V (OK <1V).
// ADC_REF_VOLTAGE : pleine échelle réelle de l'ADC. Le "1,0 V" nominal de
// l'ESP8266 varie en pratique de ~0,95 à ~1,10 V -> À CALIBRER (voir readBatteryVolts).
const float ADC_REF_VOLTAGE = 1.0;      // ajuster après mesure au multimètre
const int   ADC_MAX = 1023;
const float R_TOP    = 330000.0;        // A0 <-330k- +batterie
const float R_BOTTOM = 100000.0;        // A0 <-100k- GND
const float BATT_DIVIDER = (R_TOP + R_BOTTOM) / R_BOTTOM;  // 4.30

// Courbe de décharge mesurée (18650 1800 mAh). Interpolée linéairement par
// segments : plus fiable qu'une formule car la décharge Li-ion n'est pas linéaire.
struct BattPoint { float v; uint8_t pct; };
const BattPoint BATT_CURVE[] = {
  {2.85f,   0}, {3.35f,  20}, {3.47f,  40},
  {3.63f,  60}, {3.85f,  80}, {4.05f, 100}
};
const int BATT_CURVE_N = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

// 0..2 = jeu nocturne ; 3..8 = modes de la finale (voir changes.md / déroulé)
enum Mode {
  MODE_OFF = 0, MODE_IDLE = 1, MODE_GLITCH = 2,
  MODE_AMBER = 3,      // ambre fixe (repos avant "confirmation")
  MODE_BLUE = 4,       // bleu stable (balise qui "tient")
  MODE_ALERT = 5,      // ambre clignotant (le Windigo teste le périmètre)
  MODE_RAINBOW = 6,    // arc-en-ciel pulsant (accord final)
  MODE_BLUE_SLOW = 7,  // bleu profond, respiration très lente (fin)
  MODE_RECHARGE = 8    // flash blanc bref "rechargé par la Cloche" -> revient au bleu
};

// Luminosité de la veille nocturne (bleu pulsant faible, visible des tentes).
const uint8_t NIGHT_BRIGHTNESS = 45;
unsigned long modeStartMs = 0; // horloge d'effet (réinitialisée à chaque setMode)
volatile Mode currentMode = MODE_IDLE;
volatile bool forcedMode = false;
volatile bool glitchLock = false;

uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

unsigned long lastHeartbeatSent = 0;
unsigned long lastReceivedTimeSync = 0;
uint32_t hbSeq = 0;

// Set from the ESP-NOW recv callback (SYS context) to request an EEPROM write.
// The actual flash write MUST happen in loop(), never in the callback: writing
// flash from the WiFi/SYS context can conflict with the stack and crash the
// ESP8266 ("Soft WDT reset"). loop() flushes this flag via saveState().
volatile bool stateDirty = false;

const int MAX_PEERS = 12;
struct PeerInfo { uint8_t id; unsigned long lastSeen; };
PeerInfo peers[MAX_PEERS];

unsigned long nowEpoch() {
  if (syncedEpoch == 0) return 0;
  unsigned long delta = (millis() - syncedMillis) / 1000;
  return syncedEpoch + delta;
}

// Nuit = ~20h35 -> 5h30. Sans heure synchronisée, on suppose la nuit (la balise
// reste alors allumée/veille plutôt que de dormir et d'être éteinte au mauvais moment).
// NOTE : gmtime() est correct ici parce que la commande TIME du master transmet
// un epoch DÉJÀ décalé en heure locale (voir app.js / PROTOCOL.md).
bool isNightNow() {
  bool haveTime = (syncedEpoch != 0) && ((millis() - lastReceivedTimeSync) <= TIME_SYNC_TIMEOUT_MS);
  if (!haveTime) return true;
  time_t t = (time_t) nowEpoch();
  struct tm *tm = gmtime(&t);
  int hour = tm->tm_hour, min = tm->tm_min;
  if (hour > 20 || (hour == 20 && min >= 35)) return true;
  if (hour < 5  || (hour == 5  && min < 30)) return true;
  return false;
}

void setMode(Mode m, bool force=false) {
  modeStartMs = millis();
  if (!force && currentMode == m) return;
  currentMode = m;
  forcedMode = force;
  if (DEBUG) {
    Serial.print("Mode changed to ");
    Serial.print((int)m);
    if (force) Serial.print(" (forced)");
    Serial.println();
  }
}

// Tension batterie en volts. Moyenne quelques lectures : l'A0 de l'ESP8266 est
// bruité (et davantage avec le WiFi actif).
float readBatteryVolts() {
  long acc = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) { acc += analogRead(BATTERY_PIN); delay(2); }
  float raw = acc / (float)N;
  float vA0 = raw * (ADC_REF_VOLTAGE / ADC_MAX);
  return vA0 * BATT_DIVIDER;
}

int readBatteryMv() {
  return int(readBatteryVolts() * 1000.0f + 0.5f);
}

// Pourcentage 0..100 interpolé sur la courbe mesurée (segments linéaires).
uint8_t batteryPercent(float v) {
  if (v <= BATT_CURVE[0].v)              return 0;
  if (v >= BATT_CURVE[BATT_CURVE_N-1].v) return 100;
  for (int i = 1; i < BATT_CURVE_N; i++) {
    if (v < BATT_CURVE[i].v) {
      const BattPoint &a = BATT_CURVE[i-1];
      const BattPoint &b = BATT_CURVE[i];
      float t = (v - a.v) / (b.v - a.v);
      return (uint8_t)(a.pct + t * (b.pct - a.pct) + 0.5f);
    }
  }
  return 100;
}
uint8_t readBatteryPercent() { return batteryPercent(readBatteryVolts()); }

void effectIdleBreath(uint8_t brightness) {
  static uint32_t t0 = millis();
  uint32_t t = millis() - t0;
  float phase = (t % 4000) / 4000.0;
  // Narrow bell-shaped pulse: it reaches the same visible peak, but spends
  // most of each cycle near darkness instead of averaging a sine wave at 50%.
  // Raising the smooth 0..1 sine envelope to the fourth power keeps the pulse
  // fluid while reducing its theoretical average output to ~27% of the peak.
  float envelope = (sinf(phase * 2.0f * PI - PI/2.0f) + 1.0f) / 2.0f;
  float val = envelope * envelope;
  val *= val;
  uint8_t b = uint8_t(val * brightness);
  for (int i=0;i<NUM_LEDS;i++) leds[i] = CRGB(0, 0, b);
  FastLED.show();
}

void effectGlitch() {
  // Tube neon bleu defectueux : tenues instables, micro-coupures, rares
  // extinctions franches et reprises blanc/teal. Entierement non bloquant afin
  // que l'ESP-NOW reste reactif pendant les rafales.
  static unsigned long nextChangeAt = 0;
  static unsigned long effectStartedAt = 0;
  unsigned long now = millis();

  if (effectStartedAt != modeStartMs) {
    effectStartedAt = modeStartMs;
    nextChangeAt = now;
  }
  if ((long)(now - nextChangeAt) < 0) return;

  uint8_t event = random8(100);
  unsigned long duration;
  FastLED.clear();

  if (event < 48) {
    uint8_t level = random8(75, 175);
    CRGB blue = CRGB(random8(0, 10), random8(35, 85), 255);
    blue.nscale8_video(level);
    fill_solid(leds, NUM_LEDS, blue);
    duration = random(90, 650);
  } else if (event < 66) {
    duration = random(18, 95);       // micro-coupure
  } else if (event < 76) {
    duration = random(180, 900);     // vrai moment de noir
  } else if (event < 89) {
    fill_solid(leds, NUM_LEDS, CRGB(random(170, 256), 255, 255));
    duration = random(20, 85);       // claquement blanc froid
  } else {
    int start = random(0, NUM_LEDS);
    int len = random(1, NUM_LEDS);
    CRGB teal = CRGB(0, random(170, 256), random(150, 240));
    for (int i=0; i<len; i++) leds[(start + i) % NUM_LEDS] = teal;
    duration = random(25, 150);      // arc teal incomplet
  }

  if (event < 66 && random8(100) < 35) leds[random(0, NUM_LEDS)] = CRGB::Black;
  FastLED.show();
  nextChangeAt = now + duration;
}

// ---------------- Effets de la finale ----------------
const CRGB AMBER_COLOR = CRGB(255, 90, 0);

void effectAmber() { // ambre fixe
  for (int i=0;i<NUM_LEDS;i++) leds[i] = AMBER_COLOR;
  FastLED.show();
}

void effectBlueStable() { // bleu stable, plein
  for (int i=0;i<NUM_LEDS;i++) leds[i] = CRGB(0, 40, 255);
  FastLED.show();
}

void effectAlertBlink() { // ambre clignotant (le Windigo teste le périmètre)
  bool on = ((millis() / 350) % 2) == 0; // ~1.4 Hz
  CRGB c = on ? AMBER_COLOR : CRGB::Black;
  for (int i=0;i<NUM_LEDS;i++) leds[i] = c;
  FastLED.show();
}

void effectRainbow() { // arc-en-ciel pulsant (accord final)
  uint8_t base = (uint8_t)(millis() / 12);           // rotation de teinte
  float pulse = (sinf(millis() / 500.0f) + 1.0f) / 2.0f;
  uint8_t bright = (uint8_t)(120 + pulse * 135);
  for (int i=0;i<NUM_LEDS;i++) {
    leds[i] = CHSV(base + i * (256 / NUM_LEDS), 255, bright);
  }
  FastLED.show();
}

void effectBlueDeepSlow() { // bleu profond, respiration très lente (fin)
  float phase = (millis() % 6000) / 6000.0f;         // cycle 6s
  float val = (sinf(phase * 2.0f * PI - PI/2.0f) + 1.0f) / 2.0f;
  uint8_t b = (uint8_t)(20 + val * 120);
  for (int i=0;i<NUM_LEDS;i++) { leds[i] = CRGB(0, 20, 255); leds[i].nscale8_video(b); }
  FastLED.show();
}

// Flash blanc bref ("rechargée par la Cloche", soir 5) puis retour auto au bleu.
void effectRecharge() {
  const unsigned long DUR = 1600;
  unsigned long dt = millis() - modeStartMs;
  if (dt >= DUR) { setMode(MODE_BLUE, true); return; } // revient au bleu stable
  float x = (float)dt / DUR;                            // 0..1
  float env = (x < 0.15f) ? (x / 0.15f) : (1.0f - (x - 0.15f) / 0.85f); // pic à 15%
  CRGB c = blend(CRGB(0, 40, 255), CRGB(255, 255, 255), (uint8_t)(env * 255));
  c.nscale8_video((uint8_t)(60 + env * 195));
  for (int i=0;i<NUM_LEDS;i++) leds[i] = c;
  FastLED.show();
}

void sendToMac(const uint8_t *mac, const char* msg) {
  esp_now_send((uint8_t*)mac, (uint8_t*)msg, strlen(msg)+1);
}
void sendBroadcast(const char* msg) {
  esp_now_send(broadcastAddress, (uint8_t*)msg, strlen(msg)+1);
}
void sendAck(const uint8_t *mac, const char* cmd, const char* status) {
  char buf[64];
  snprintf(buf, sizeof(buf), "ACK|%u|%s|%s", (unsigned)DEVICE_ID, cmd, status);
  sendToMac(mac, buf);
  if (DEBUG) {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    Serial.print("Sent ACK to ");
    Serial.print(macStr);
    Serial.print(" -> ");
    Serial.println(buf);
  }
}
void sendHeartbeat() {
  float volts = readBatteryVolts();          // une seule lecture ADC par HB
  int batt = int(volts * 1000.0f + 0.5f);
  uint8_t pct = batteryPercent(volts);
  char buf[80];
  snprintf(buf, sizeof(buf), "HB|%lu|%u|%u|%d|%u",
           (unsigned long)hbSeq, (unsigned)DEVICE_ID, (unsigned)currentMode, batt, pct);
  sendBroadcast(buf);
  lastHeartbeatSent = millis();
  hbSeq++;
  if (DEBUG) {
    Serial.print("HB sent seq=");
    Serial.print(hbSeq-1);
    Serial.print(" mode=");
    Serial.print((int)currentMode);
    Serial.print(" batt=");
    Serial.print(batt);
    Serial.print("mV (");
    Serial.print(volts, 2);
    Serial.print("V, ");
    Serial.print(pct);
    Serial.println("%)");
  }
}

bool parseToField(const String &msg, int &toId, int &posAfterTo) {
  toId = -1; posAfterTo = -1;
  int idx = msg.indexOf("TO|");
  if (idx < 0) return false;
  int p = idx + 3;
  int p2 = msg.indexOf('|', p);
  if (p2 < 0) return false;
  String sid = msg.substring(p, p2);
  toId = sid.toInt();
  posAfterTo = p2 + 1;
  return true;
}

// ---------------- EEPROM persistence ----------------
// layout:
// 0..3   : magic (uint32_t)
// 4      : glitchLock (uint8_t)
// 8..11  : syncedEpoch (uint32_t)
const uint32_t EEPROM_MAGIC = 0xB1A5C0DE;
const int EEPROM_SIZE = 64;

void saveState() {
  if (DEBUG) Serial.println("Saving state to EEPROM");
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = EEPROM_MAGIC;
  EEPROM.put(0, magic);
  uint8_t g = glitchLock ? 1 : 0;
  EEPROM.put(4, g);
  EEPROM.put(8, syncedEpoch);
  EEPROM.commit();
  EEPROM.end();
}

void loadState() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic = 0;
  EEPROM.get(0, magic);
  if (magic == EEPROM_MAGIC) {
    uint8_t g = 0;
    EEPROM.get(4, g);
    glitchLock = (g != 0);
    EEPROM.get(8, syncedEpoch);
    if (DEBUG) {
      Serial.print("Loaded state: glitchLock=");
      Serial.print(glitchLock);
      Serial.print(" syncedEpoch=");
      Serial.println(syncedEpoch);
    }
  } else {
    if (DEBUG) Serial.println("No EEPROM magic, fresh start");
    glitchLock = false;
    syncedEpoch = 0;
  }
  EEPROM.end();
}

// ---------------- Callbacks ----------------
#ifdef BALISE_ESP32
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
#else
void onDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
#endif
  // Bound the copy: don't trust a NUL terminator from the sender.
  char tmp[251];
  int n = (len < (int)sizeof(tmp) - 1) ? len : (int)sizeof(tmp) - 1;
  memcpy(tmp, incomingData, n);
  tmp[n] = 0;
  String msg = String(tmp);
  if (DEBUG) {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    Serial.print("RX from ");
    Serial.print(macStr);
    Serial.print(" : ");
    Serial.println(msg);
  }

  if (msg.startsWith("HB|")) {
    int p1 = msg.indexOf('|', 3);
    if (p1 > 0) {
      int p2 = msg.indexOf('|', p1+1);
      if (p2 > 0) {
        String sid = msg.substring(p1+1, p2);
        uint8_t sidv = sid.toInt();
        bool found = false;
        for (int i=0;i<MAX_PEERS;i++) {
          if (peers[i].id == sidv) { peers[i].lastSeen = millis(); found = true; break; }
        }
        if (!found) {
          for (int i=0;i<MAX_PEERS;i++) {
            if (peers[i].id == 0) { peers[i].id = sidv; peers[i].lastSeen = millis(); break; }
          }
        }
      }
    }
    return;
  }

  if (msg.startsWith("ACK|")) {
    // ACK handling left minimal; master will parse ACKs
    return;
  }

  if (msg.startsWith("CMD|")) {
    int toId=-1, posAfterTo=-1;
    bool hasTo = parseToField(msg, toId, posAfterTo);
    if (hasTo && toId != DEVICE_ID) {
      if (DEBUG) Serial.println("CMD not for me, ignored");
      return;
    }

    String payload = hasTo ? msg.substring(posAfterTo) : msg.substring(4);
    int p = payload.indexOf('|');
    String cmd = (p>0) ? payload.substring(0,p) : payload;
    String arg = (p>0) ? payload.substring(p+1) : "";

    if (DEBUG) {
      Serial.print("CMD received: ");
      Serial.print(cmd);
      if (arg.length()) { Serial.print(" | arg="); Serial.print(arg); }
      Serial.println();
    }

    if (cmd == "SETMODE") {
      int m = arg.toInt();
      setMode((Mode)m, false);
      sendAck(mac, "SETMODE", "OK");
    } else if (cmd == "FORCE_GLITCH") {
      setMode(MODE_GLITCH, true);
      sendAck(mac, "FORCE_GLITCH", "OK");
    } else if (cmd == "FORCE_AMBER") {
      setMode(MODE_AMBER, true);      sendAck(mac, "FORCE_AMBER", "OK");
    } else if (cmd == "FORCE_BLUE") {
      setMode(MODE_BLUE, true);       sendAck(mac, "FORCE_BLUE", "OK");
    } else if (cmd == "FORCE_ALERT") {
      setMode(MODE_ALERT, true);      sendAck(mac, "FORCE_ALERT", "OK");
    } else if (cmd == "FORCE_RAINBOW") {
      setMode(MODE_RAINBOW, true);    sendAck(mac, "FORCE_RAINBOW", "OK");
    } else if (cmd == "FORCE_BLUE_SLOW") {
      setMode(MODE_BLUE_SLOW, true);  sendAck(mac, "FORCE_BLUE_SLOW", "OK");
    } else if (cmd == "FORCE_RECHARGE") {
      setMode(MODE_RECHARGE, true);   sendAck(mac, "FORCE_RECHARGE", "OK");
    } else if (cmd == "FORCE_IDLE") {
      setMode(MODE_IDLE, true);
      glitchLock = false;
      stateDirty = true; // flush to EEPROM from loop(), not here
      sendAck(mac, "FORCE_IDLE", "OK");
    } else if (cmd == "FORCE_OFF") {
      setMode(MODE_OFF, true);
      sendAck(mac, "FORCE_OFF", "OK");
    } else if (cmd == "GLITCH_LOCK") {
      int v = arg.toInt();
      glitchLock = (v != 0);
      stateDirty = true; // flush to EEPROM from loop(), not here
      sendAck(mac, "GLITCH_LOCK", "OK");
    } else if (cmd == "TIME") {
      unsigned long epoch = (unsigned long) arg.toInt();
      if (epoch > 1600000000UL) {
        syncedEpoch = epoch; syncedMillis = millis(); lastReceivedTimeSync = millis();
        stateDirty = true; // flush to EEPROM from loop(), not here
        sendAck(mac, "TIME", "OK");
      } else sendAck(mac, "TIME", "ERR");
    } else {
      sendAck(mac, "UNKNOWN", "ERR");
    }
  }
}

#ifdef BALISE_ESP32
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t sendStatus) {
#else
void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
#endif
  if (DEBUG) {
    char macStr[18];
    // FIXED: correct format specifiers
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
    Serial.print("onDataSent to ");
    Serial.print(macStr);
    Serial.print(" status=");
    Serial.println(sendStatus);
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#ifdef BALISE_ESP32
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    if (DEBUG) Serial.println("ESP-NOW init failed, restarting");
    ESP.restart();
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
#else
  // For ESP8266, set channel explicitly before esp_now_init
  wifi_set_channel(WIFI_CHANNEL);
  if (esp_now_init() != 0) {
    if (DEBUG) Serial.println("ESP-NOW init failed, restarting");
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  // add broadcast peer on WIFI_CHANNEL
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, NULL, 0);
#endif
}

void setup() {
  if (DEBUG) Serial.begin(115200);
  delay(50);
  if (DEBUG) {
    Serial.printf("\n%s (id=%u) — MAC: %s\n", DEVICE_NAME, (unsigned)DEVICE_ID,
                  WiFi.macAddress().c_str());
  }
  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);
  FastLED.clear(); FastLED.show();
  for (int i=0;i<MAX_PEERS;i++) { peers[i].id = 0; peers[i].lastSeen = 0; }

  // load persisted state (glitchLock, syncedEpoch)
  loadState();

  setupEspNow();
  setMode(MODE_IDLE, false);
  lastHeartbeatSent = 0;
  lastReceivedTimeSync = 0;

  // Quand elle est éveillée (la nuit / en finale) : radio sans veille pour un
  // ESP-NOW réactif et des LEDs stables. Le jour, la boucle repasse en deep-sleep.
#ifdef BALISE_ESP32
  esp_wifi_set_ps(WIFI_PS_NONE);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
}

void loop() {
  unsigned long now = millis();

  // Flush any EEPROM write requested by the ESP-NOW callback (safe here).
  if (stateDirty) { stateDirty = false; saveState(); }

  bool night = isNightNow();

  // Deep-sleep UNIQUEMENT le jour (et jamais en mode forcé / finale). La nuit,
  // la balise reste éveillée pour veiller (bleu faible) et répondre en temps réel.
  if (!DEBUG && USE_DEEP_SLEEP && !forcedMode && !night) {
    // Réveil : HB, on écoute WAKE_WINDOW_MS, puis on se rendort.
    sendHeartbeat();
    unsigned long start = millis();
    while (millis() - start < WAKE_WINDOW_MS) {
      delay(10); // les callbacks ESP-NOW tournent en asynchrone
    }
    saveState();
#ifdef BALISE_ESP32
    esp_deep_sleep(WAKE_INTERVAL_S * 1000000ULL);
#else
    ESP.deepSleep(WAKE_INTERVAL_S * 1000000ULL);
#endif
    // ne revient pas
  } else {
    // Éveillée : heartbeat périodique.
    if (now - lastHeartbeatSent >= HEARTBEAT_INTERVAL_MS) sendHeartbeat();
  }

  // peer cleanup
  for (int i=0;i<MAX_PEERS;i++) {
    if (peers[i].id != 0 && (now - peers[i].lastSeen) > HEARTBEAT_TIMEOUT_MS) {
      peers[i].id = 0; peers[i].lastSeen = 0;
    }
  }

  int seenCount = 0;
  for (int i=0;i<MAX_PEERS;i++) {
    if (peers[i].id != 0 && (now - peers[i].lastSeen) <= GROUP_WINDOW_MS) seenCount++;
  }

  if (!forcedMode) {
    // Nuit -> veille bleu faible ; jour -> éteinte (et de toute façon en deep-sleep).
    setMode(night ? MODE_IDLE : MODE_OFF, false);
  }

  if (currentMode == MODE_GLITCH && !forcedMode) {
    if (!glitchLock) {
      if (seenCount >= GROUP_MIN_SEEN) {
        setMode(MODE_IDLE, false);
      }
    }
  }

  switch (currentMode) {
    case MODE_IDLE:      effectIdleBreath(NIGHT_BRIGHTNESS); break; // veille faible
    case MODE_GLITCH:    effectGlitch();        break;
    case MODE_AMBER:     effectAmber();         break;
    case MODE_BLUE:      effectBlueStable();    break;
    case MODE_ALERT:     effectAlertBlink();    break;
    case MODE_RAINBOW:   effectRainbow();       break;
    case MODE_BLUE_SLOW: effectBlueDeepSlow();  break;
    case MODE_RECHARGE:  effectRecharge();      break;
    case MODE_OFF:
    default:             FastLED.clear(); FastLED.show(); delay(50); break;
  }

  delay(10);
}
