/* Balise D1 mini - Deep sleep wake-windows + single DEBUG_MODE flag + EEPROM persistence
   Channel forced to WIFI_CHANNEL (9)
   DEBUG_MODE = true pour debug (pas de deep sleep)
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <FastLED.h>
#include <EEPROM.h>

// Build mode single flag: change this before flashing
#define DEBUG_MODE true   // true = dev (serial on, no sleep). false = prod (serial off, sleep enabled)

const bool DEBUG = DEBUG_MODE;
const bool USE_DEEP_SLEEP = !DEBUG;

#define WIFI_CHANNEL 9  // FORCED CHANNEL

// Device identity
const uint8_t DEVICE_ID = 4;
const char* DEVICE_NAME = "Balise_Ouest";

#define LED_PIN D4
#define NUM_LEDS 6
CRGB leds[NUM_LEDS];

// Heartbeat / timing defaults (HEARTBEAT_INTERVAL_MS will be derived)
unsigned long HEARTBEAT_INTERVAL_MS = 3000; // dev default
const unsigned long HB_DAY_MS   = 60UL * 60UL * 1000UL; // 1h day
const unsigned long HB_NIGHT_MS = 2UL * 60UL * 1000UL;  // 2min night

// Deep sleep / wake window parameters (production)
const unsigned long WAKE_WINDOW_MS = 2000;  // listen 2s after wake
const unsigned long WAKE_INTERVAL_S = 60;   // sleep 60s between windows

const unsigned long HEARTBEAT_TIMEOUT_MS = 15000;
const unsigned long GROUP_WINDOW_MS = 10000;
const int GROUP_MIN_SEEN = 2;

const unsigned long TIME_SYNC_TIMEOUT_MS = 600000;
unsigned long syncedEpoch = 0;
unsigned long syncedMillis = 0;

const float ADC_REF_VOLTAGE = 3.2;
const int ADC_MAX = 1023;
const float R1 = 100000.0;
const float R2 = 220000.0;

enum Mode { MODE_OFF = 0, MODE_IDLE = 1, MODE_GLITCH = 2 };
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

void dbg(const String &s) { if (DEBUG) Serial.println(s); }

unsigned long nowEpoch() {
  if (syncedEpoch == 0) return 0;
  unsigned long delta = (millis() - syncedMillis) / 1000;
  return syncedEpoch + delta;
}

void setMode(Mode m, bool force=false) {
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

int readBatteryMv() {
  int raw = analogRead(A0);
  float vA0 = (raw * (ADC_REF_VOLTAGE / ADC_MAX));
  float battV = vA0 * ((R1 + R2) / R2);
  return int(battV * 1000.0);
}

void effectIdleBreath(uint8_t brightness) {
  static uint32_t t0 = millis();
  uint32_t t = millis() - t0;
  float phase = (t % 4000) / 4000.0;
  float val = (sin(phase * 2.0 * PI - PI/2.0) + 1.0) / 2.0;
  uint8_t b = uint8_t(val * brightness);
  for (int i=0;i<NUM_LEDS;i++) leds[i] = CRGB(0, 0, b);
  FastLED.show();
}

void effectGlitch() {
  static uint32_t lastBase = 0;
  if (millis() - lastBase > 80) {
    lastBase = millis();
    int c = random(0,3);
    CRGB baseColor = (c==0) ? CRGB::Red : (c==1) ? CRGB(255,160,0) : CRGB(0,120,255);
    uint8_t baseIntensity = random(30, 120);
    for (int i=0;i<NUM_LEDS;i++) leds[i] = baseColor.nscale8_video(baseIntensity);
    FastLED.show();
  }
  static unsigned long nextFlashAt = 0;
  if (millis() >= nextFlashAt) {
    int flashDur = random(20, 200);
    int bursts = random(1, 5);
    for (int b=0;b<bursts;b++) {
      int start = random(0, NUM_LEDS);
      int len = random(1, min(6, NUM_LEDS));
      CRGB color = (random(0,3)==0) ? CRGB::Red : (random(0,2)==0 ? CRGB(255,160,0) : CRGB(0,120,255));
      uint8_t intensity = random(120, 255);
      for (int i=0;i<len;i++) {
        int idx = (start + i) % NUM_LEDS;
        leds[idx] = color.nscale8_video(intensity);
      }
      FastLED.show();
      delay(random(20, flashDur));
      for (int f=0; f<2; f++) {
        for (int i=0;i<NUM_LEDS;i++) leds[i].nscale8(180);
        FastLED.show();
        delay(20);
      }
    }
    nextFlashAt = millis() + random(50, 2000);
  }
}

void sendToMac(const uint8_t *mac, const char* msg) {
  esp_now_send((uint8_t*)mac, (uint8_t*)msg, strlen(msg)+1);
}
void sendBroadcast(const char* msg) {
  esp_now_send(broadcastAddress, (uint8_t*)msg, strlen(msg)+1);
}
void sendAck(const uint8_t *mac, const char* cmd, const char* status) {
  char buf[64];
  snprintf(buf, sizeof(buf), "ACK|%u|%s|%s", DEVICE_ID, cmd, status);
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
  int batt = readBatteryMv();
  char buf[80];
  snprintf(buf, sizeof(buf), "HB|%lu|%u|%u|%d", (unsigned long)hbSeq, DEVICE_ID, (uint8_t)currentMode, batt);
  sendBroadcast(buf);
  lastHeartbeatSent = millis();
  hbSeq++;
  if (DEBUG) {
    Serial.print("HB sent seq=");
    Serial.print(hbSeq-1);
    Serial.print(" mode=");
    Serial.print((int)currentMode);
    Serial.print(" batt=");
    Serial.println(batt);
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
void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
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

void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
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
}

void setup() {
  if (DEBUG) Serial.begin(115200);
  delay(50);
  if (DEBUG) {
    Serial.print("Local MAC: ");
    Serial.println(WiFi.macAddress());
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

  // configure sleep behavior derived from DEBUG_MODE
  if (!DEBUG) {
    if (USE_DEEP_SLEEP) {
      if (DEBUG) Serial.println("Production: deep sleep enabled");
    } else {
      WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
      if (DEBUG) Serial.println("Production: light sleep enabled");
    }
  } else {
    HEARTBEAT_INTERVAL_MS = 3000;
    if (DEBUG) Serial.println("Dev mode: sleep disabled, HB every 3s");
  }
}

void loop() {
  unsigned long now = millis();

  // Flush any EEPROM write requested by the ESP-NOW callback (safe here).
  if (stateDirty) { stateDirty = false; saveState(); }

  // determine day/night and set heartbeat interval accordingly (only when not forced)
  if (!forcedMode) {
    unsigned long epochNow = nowEpoch();
    bool haveTime = (syncedEpoch != 0) && ((millis() - lastReceivedTimeSync) <= TIME_SYNC_TIMEOUT_MS);
    bool isNight = true;
    if (haveTime) {
      time_t t = (time_t) epochNow;
      struct tm *tm = gmtime(&t);
      int hour = tm->tm_hour; int min = tm->tm_min;
      if (hour > 20 || (hour == 20 && min >= 35)) isNight = true;
      else if (hour < 5 || (hour == 5 && min < 30)) isNight = true;
      else isNight = false;
    } else {
      isNight = true;
    }

    if (DEBUG) {
      HEARTBEAT_INTERVAL_MS = 3000;
    } else {
      HEARTBEAT_INTERVAL_MS = isNight ? HB_NIGHT_MS : HB_DAY_MS;
    }
  }

  // If using deep sleep mode, implement wake-window cycle:
  if (!DEBUG && USE_DEEP_SLEEP && !forcedMode) {
    // On wake (normal loop after boot), send HB and listen for WAKE_WINDOW_MS, then deepSleep
    sendHeartbeat();
    unsigned long start = millis();
    // listen for commands during wake window
    while (millis() - start < WAKE_WINDOW_MS) {
      // espnow callbacks will run asynchronously
      delay(10);
    }
    // persist state before sleeping
    saveState();
    if (DEBUG) Serial.println("Entering deep sleep...");
    ESP.deepSleep(WAKE_INTERVAL_S * 1000000ULL);
    // does not return
  } else {
    // Normal (no deep sleep): periodic heartbeat based on HEARTBEAT_INTERVAL_MS
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
    unsigned long epochNow = nowEpoch();
    bool haveTime = (syncedEpoch != 0) && ((millis() - lastReceivedTimeSync) <= TIME_SYNC_TIMEOUT_MS);
    bool isNight = true;
    if (haveTime) {
      time_t t = (time_t) epochNow;
      struct tm *tm = gmtime(&t);
      int hour = tm->tm_hour; int min = tm->tm_min;
      if (hour > 20 || (hour == 20 && min >= 35)) isNight = true;
      else if (hour < 5 || (hour == 5 && min < 30)) isNight = true;
      else isNight = false;
    } else {
      isNight = true;
    }

    if (isNight) {
      setMode(MODE_IDLE, false);
    } else {
      setMode(MODE_OFF, false);
    }
  }

  if (currentMode == MODE_GLITCH && !forcedMode) {
    if (!glitchLock) {
      if (seenCount >= GROUP_MIN_SEEN) {
        setMode(MODE_IDLE, false);
      }
    }
  }

  if (currentMode == MODE_IDLE) {
    effectIdleBreath(120);
  } else if (currentMode == MODE_GLITCH) {
    effectGlitch();
  } else {
    FastLED.clear(); FastLED.show();
    delay(200);
  }

  delay(10);
}
