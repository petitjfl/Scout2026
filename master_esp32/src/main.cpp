// Master ESP32 - ESP-NOW manager + WebServer + WebSocket + LittleFS
// Channel forced to 9, UNICAST_GAP_MS = 300
// Detailed ESP-NOW RX logging and WS send/broadcast  wrappers

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------------- CONFIG ----------------
#define AP_SSID "MASTER_AP"
#define AP_PASS "masterpass"
const uint16_t WS_PORT = 81;
const uint16_t HTTP_PORT = 80;
const char* CONFIG_PATH = "/config.json";

int RETRIES = 3;                    // max send attempts per pending command
unsigned long UNICAST_GAP_MS = 300; // ms

// Align this with balise WAKE_INTERVAL_S (seconds)
const unsigned long WAKE_INTERVAL_MS = 60UL * 1000UL; // 60s

// ---------------- State ----------------
struct Accessory {
  uint8_t id;
  String name;
  uint8_t mac[6];
  bool hasMac;
  unsigned long lastHbMs;
  unsigned long lastHbSeq;
  int mode;
  int batteryMv;
  unsigned long lastAckMs;
  bool present;
};
Accessory accessories[8];
int accessoryCount = 0;

// Pending command per-target (one slot per balise, keep only last command)
struct PendingCmd {
  bool active;
  uint8_t targetMac[6];
  uint8_t targetId;
  String cmd;        // command name e.g. "FORCE_GLITCH"
  String msg;        // full payload "CMD|FORCE_GLITCH|arg"
  unsigned long createdAt;
  unsigned long nextTryAt; // millis()
  int attempts;
  unsigned long sentAt;
  int lastBroadcastAttempts; // avoid duplicate pending_update broadcasts
};
const int MAX_PENDING = 8;
PendingCmd pending[MAX_PENDING];

const unsigned long PENDING_EXPIRY_MS = 24UL * 3600UL * 1000UL; // 24h max (safety)
// Max attempts is driven by the configurable RETRIES value (see processPendingQueue).

// HB batch aggregation
bool hbDirty = false;
unsigned long lastHbBroadcast = 0;
const unsigned long HB_BROADCAST_INTERVAL_MS = 1000; // send batch every 1s

// Web & WS
WebServer server(HTTP_PORT);
WebSocketsServer webSocket(WS_PORT);

// ---------------- Helpers ----------------
String macToStr(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}
bool macEqual(const uint8_t *a, const uint8_t *b) {
  for (int i=0;i<6;i++) if (a[i]!=b[i]) return false;
  return true;
}
bool macIsValid(const uint8_t *mac) {
  for (int i=0;i<6;i++) if (mac[i] != 0) return true;
  return false;
}

// ---------------- Persistence ----------------
void saveConfig() {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("accessories");
  for (int i=0;i<accessoryCount;i++) {
    JsonObject it = arr.createNestedObject();
    it["id"] = accessories[i].id;
    it["name"] = accessories[i].name;
    it["mac"] = accessories[i].hasMac ? macToStr(accessories[i].mac) : "";
    it["role"] = "balise";
  }
  doc["retries"] = RETRIES;
  doc["unicast_gap_ms"] = UNICAST_GAP_MS;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("saveConfig: open failed");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved");
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  RETRIES = doc["retries"] | RETRIES;
  UNICAST_GAP_MS = doc["unicast_gap_ms"] | UNICAST_GAP_MS;
  if (doc.containsKey("accessories")) {
    JsonArray arr = doc["accessories"].as<JsonArray>();
    accessoryCount = 0;
    for (JsonVariant v : arr) {
      if (accessoryCount >= 8) break;
      accessories[accessoryCount].id = v["id"].as<int>();
      accessories[accessoryCount].name = String((const char*)v["name"]);
      String macs = String((const char*)v["mac"]);
      if (macs.length() == 17 && macs != "00:00:00:00:00:00") {
        uint8_t m[6];
        sscanf(macs.c_str(), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
               &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]);
        memcpy(accessories[accessoryCount].mac, m, 6);
        accessories[accessoryCount].hasMac = true;
      } else {
        for (int j=0;j<6;j++) accessories[accessoryCount].mac[j] = 0;
        accessories[accessoryCount].hasMac = false;
      }
      accessories[accessoryCount].present = false;
      accessoryCount++;
    }
  }
}

// ---------------- ESP-NOW peer helpers ----------------
bool ensurePeerExists(const uint8_t *mac) {
  if (!macIsValid(mac)) return false;
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 9;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t r = esp_now_add_peer(&peer);
  Serial.print("esp_now_add_peer -> ");
  Serial.println((int)r);
  return (r == ESP_OK);
}

// ---------------- Pending queue helpers ----------------
int findPendingSlotForId(uint8_t id) {
  for (int i=0;i<MAX_PENDING;i++) if (pending[i].active && pending[i].targetId == id) return i;
  return -1;
}
int findFreePendingSlot() {
  for (int i=0;i<MAX_PENDING;i++) if (!pending[i].active) return i;
  return -1;
}

bool queueOrReplaceCommand(const uint8_t *mac, uint8_t targetId, const String &cmdName, const String &msg) {
  int slot = findPendingSlotForId(targetId);
  if (slot == -1) slot = findFreePendingSlot();
  if (slot == -1) return false; // queue full

  memcpy(pending[slot].targetMac, mac, 6);
  pending[slot].targetId = targetId;
  pending[slot].cmd = cmdName;
  pending[slot].msg = msg;
  pending[slot].createdAt = millis();
  pending[slot].nextTryAt = millis(); // try immediately
  pending[slot].attempts = 0;
  pending[slot].active = true;
  pending[slot].sentAt = 0;
  pending[slot].lastBroadcastAttempts = -1;

  Serial.printf("Queued cmd %s for id=%d slot=%d\n", cmdName.c_str(), targetId, slot);
  return true;
}

bool trySendPending(int slot) {
  if (!pending[slot].active) return false;
  if (!macIsValid(pending[slot].targetMac)) {
    pending[slot].active = false;
    return false;
  }
  // ensure peer exists
  ensurePeerExists(pending[slot].targetMac);
  Serial.printf("Attempt send pending id=%d cmd=%s attempt=%d\n", pending[slot].targetId, pending[slot].cmd.c_str(), pending[slot].attempts+1);
  esp_err_t res = esp_now_send(pending[slot].targetMac, (uint8_t*)pending[slot].msg.c_str(), pending[slot].msg.length()+1);
  pending[slot].sentAt = millis();
  pending[slot].attempts++;
  // schedule next try after WAKE_INTERVAL_MS by default; actual removal happens on ACK or expiry
  pending[slot].nextTryAt = millis() + WAKE_INTERVAL_MS;

  // notify UI update about attempt only if attempts changed since last broadcast
  if (pending[slot].attempts != pending[slot].lastBroadcastAttempts) {
    pending[slot].lastBroadcastAttempts = pending[slot].attempts;
    StaticJsonDocument<256> d;
    d["type"]="pending_update";
    d["id"]=pending[slot].targetId;
    d["cmd"]=pending[slot].cmd;
    d["nextTryAt"]=pending[slot].nextTryAt;
    d["attempts"]=pending[slot].attempts;
    String out; serializeJson(d,out);
    webSocket.broadcastTXT(out);
  }
  if (res != ESP_OK) {
    Serial.printf("esp_now_send returned %d\n", (int)res);
  }
  return (res == ESP_OK);
}

void markPendingFailed(int slot, const char* reason) {
  if (!pending[slot].active) return;
  StaticJsonDocument<256> d;
  d["type"]="send_result";
  d["id"]=pending[slot].targetId;
  d["cmd"]=pending[slot].cmd;
  d["ok"]=false;
  d["reason"]=reason;
  String out; serializeJson(d,out);
  webSocket.broadcastTXT(out);
  Serial.printf("Pending id=%d cmd=%s failed: %s\n", pending[slot].targetId, pending[slot].cmd.c_str(), reason);
  pending[slot].active = false;
}

void processPendingQueue() {
  static unsigned long lastSendMs = 0;
  unsigned long now = millis();
  // Handle expiry / max-attempts for every slot first.
  for (int i=0;i<MAX_PENDING;i++) {
    if (!pending[i].active) continue;
    // expiry by age
    if (now - pending[i].createdAt > PENDING_EXPIRY_MS) {
      markPendingFailed(i, "expired");
      continue;
    }
    // max attempts (configurable via RETRIES)
    if (pending[i].attempts >= RETRIES) {
      markPendingFailed(i, "max_attempts");
      continue;
    }
  }
  // Send at most ONE due command per UNICAST_GAP_MS window. This spaces the
  // actual radio transmissions (the real point of UNICAST_GAP_MS) without ever
  // blocking the WiFi/WebSocket stack with delay().
  if (now - lastSendMs < UNICAST_GAP_MS) return;
  for (int i=0;i<MAX_PENDING;i++) {
    if (!pending[i].active) continue;
    if (now >= pending[i].nextTryAt) {
      trySendPending(i);   // non-blocking
      lastSendMs = now;
      break;               // one send per window; rest go next cycle
    }
  }
}

// ---------------- WS helper wrappers ----------------
void wsSend(uint8_t clientNum, String payload) {
  Serial.printf("WS send -> client %u : %u bytes\n", clientNum, (unsigned)payload.length());
  webSocket.sendTXT(clientNum, payload);
}
void wsBroadcast(String payload) {
  Serial.printf("WS broadcast -> %u bytes\n", (unsigned)payload.length());
  webSocket.broadcastTXT(payload);
}

// ---------------- ESP-NOW RX queue ----------------
// The esp_now recv callback runs in the WiFi task. It MUST NOT touch the
// WebSocket library, LittleFS, or shared state directly (the WebSocket lib is
// not thread-safe, and racing the main loop corrupts memory -> crash / WiFi
// drop under load). Instead the callback only copies the raw packet into this
// SPSC ring; all processing happens in loop() via drainRxQueue().
struct RxPacket {
  uint8_t mac[6];
  uint8_t data[251]; // ESP_NOW_MAX_DATA_LEN (250) + NUL
  int len;
};
const int RX_QUEUE_SIZE = 24;
static RxPacket rxQueue[RX_QUEUE_SIZE];
static volatile int rxHead = 0; // producer (WiFi task)
static volatile int rxTail = 0; // consumer (loop task)
static volatile int rxCount = 0;
static volatile uint32_t rxDropped = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------- ESP-NOW callbacks ----------------
// Runs in WiFi task context: keep it minimal, no WS / flash / String parsing.
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len <= 0) return;
  if (len > (int)sizeof(rxQueue[0].data) - 1) len = sizeof(rxQueue[0].data) - 1;
  portENTER_CRITICAL(&rxMux);
  if (rxCount < RX_QUEUE_SIZE) {
    int slot = rxHead;
    memcpy(rxQueue[slot].mac, mac, 6);
    memcpy(rxQueue[slot].data, incomingData, len);
    rxQueue[slot].data[len] = 0;
    rxQueue[slot].len = len;
    rxHead = (rxHead + 1) % RX_QUEUE_SIZE;
    rxCount++;
  } else {
    rxDropped++; // queue full: drop (handled/logged by loop)
  }
  portEXIT_CRITICAL(&rxMux);
}

// Runs in loop() context: safe to call WebSocket, LittleFS, mutate state.
void processRxMessage(const uint8_t * mac, const uint8_t *incomingData, int len) {
  String msg = String((char*)incomingData);
  // raw logging
  Serial.printf("ESP-NOW RX raw from %s : %d bytes\n", macToStr(mac).c_str(), len);
  Serial.printf("ESP-NOW payload: %s\n", msg.c_str());

  if (msg.startsWith("HB|")) {
    int p1 = msg.indexOf('|', 3);
    int p2 = msg.indexOf('|', p1+1);
    int p3 = msg.indexOf('|', p2+1);
    int p4 = msg.indexOf('|', p3+1);
    if (p1>0 && p2>0 && p3>0) {
      String seqS = msg.substring(3, p1);
      String idS  = msg.substring(p1+1, p2);
      String modeS= msg.substring(p2+1, p3);
      String battS= (p4>0) ? msg.substring(p3+1, p4) : msg.substring(p3+1);
      uint8_t id = idS.toInt();
      unsigned long seq = seqS.toInt();
      int mode = modeS.toInt();
      int batt = battS.toInt();

      int idx = -1;
      for (int i=0;i<accessoryCount;i++) if (accessories[i].id == id) { idx = i; break; }
      if (idx==-1 && accessoryCount < 8) {
        idx = accessoryCount++;
        accessories[idx].id = id;
        accessories[idx].name = String("ID_") + String(id);
        for (int j=0;j<6;j++) accessories[idx].mac[j] = 0;
        accessories[idx].hasMac = false;
        accessories[idx].present = false;
        Serial.print("New accessory id=");
        Serial.println(id);
      }

      if (idx!=-1) {
        if (macIsValid(mac) && (!accessories[idx].hasMac || !macEqual(accessories[idx].mac, mac))) {
          memcpy(accessories[idx].mac, mac, 6);
          accessories[idx].hasMac = true;
          Serial.print("Stored MAC for id ");
          Serial.print(id);
          Serial.print(" = ");
          Serial.println(macToStr(accessories[idx].mac));
          ensurePeerExists(mac);
          saveConfig();
        }

        // update accessory state
        accessories[idx].lastHbMs = millis();
        accessories[idx].lastHbSeq = seq;
        accessories[idx].mode = mode;
        accessories[idx].batteryMv = batt;
        accessories[idx].present = true;

        // mark dirty for batch broadcast (do not broadcast immediately)
        hbDirty = true;

        // A balise only listens for ~2s right after sending its HB. This HB is
        // our one reliable signal that it is awake, so push any pending command
        // for it now instead of waiting for the blind periodic retry.
        int ps = findPendingSlotForId(id);
        if (ps != -1 && pending[ps].active) {
          Serial.printf("HB from id=%d -> pushing pending cmd %s\n", id, pending[ps].cmd.c_str());
          trySendPending(ps);
        }
      }
    }
    return;
  }

  if (msg.startsWith("ACK|")) {
    int p1 = msg.indexOf('|',4);
    int p2 = msg.indexOf('|', p1+1);
    if (p1>0 && p2>0) {
      String idS = msg.substring(4, p1);
      String cmd = msg.substring(p1+1, p2);
      String status = msg.substring(p2+1);
      uint8_t id = idS.toInt();
      // find pending slot for this id+cmd and mark success
      for (int i=0;i<MAX_PENDING;i++) {
        if (pending[i].active && pending[i].targetId == id && pending[i].cmd == cmd) {
          // success
          pending[i].active = false;
          for (int j=0;j<accessoryCount;j++) if (accessories[j].id == id) {
            accessories[j].lastAckMs = millis();
            break;
          }
          StaticJsonDocument<256> d;
          d["type"]="ack";
          d["id"]=id;
          d["cmd"]=cmd;
          d["status"]=status;
          String out; serializeJson(d,out);
          wsBroadcast(out);

          // notify send_result success (UI expects send_result)
          StaticJsonDocument<256> r;
          r["type"]="send_result";
          r["id"]=id;
          r["cmd"]=cmd;
          r["ok"]=true;
          String rout; serializeJson(r, rout);
          wsBroadcast(rout);

          Serial.printf("ACK received id=%d cmd=%s status=%s\n", id, cmd.c_str(), status.c_str());
          break;
        }
      }
    }
    return;
  }

  // other messages (CMD to master?) - ignore or log
}

// Drain the ESP-NOW RX ring in loop() context and process each packet safely.
void drainRxQueue() {
  static uint32_t lastReportedDropped = 0;
  for (;;) {
    RxPacket pkt;
    bool have = false;
    portENTER_CRITICAL(&rxMux);
    if (rxCount > 0) {
      pkt = rxQueue[rxTail];
      rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
      rxCount--;
      have = true;
    }
    portEXIT_CRITICAL(&rxMux);
    if (!have) break;
    processRxMessage(pkt.mac, pkt.data, pkt.len);
  }
  if (rxDropped != lastReportedDropped) {
    Serial.printf("WARN: ESP-NOW RX queue overflow, dropped=%lu\n", (unsigned long)rxDropped);
    lastReportedDropped = rxDropped;
  }
}

// onDataSent left as-is for logging
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("onDataSent to ");
  Serial.print(macToStr(mac_addr));
  Serial.print(" status=");
  Serial.println((int)status);
}

// ---------------- WebSocket handlers ----------------
void handleWSMessage(uint8_t num, WStype_t type, uint8_t * payload, size_t length);

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("WS client connected: id=%u ip=%s\n", num, ip.toString().c_str());
    StaticJsonDocument<128> d;
    d["type"]="log";
    d["msg"]=String("WS connected: ") + ip.toString();
    String out; serializeJson(d,out);
    wsSend(num, out);

    // send initial scan_result to this client
    StaticJsonDocument<1024> s;
    s["type"]="scan_result";
    JsonArray arr = s.createNestedArray("items");
    for (int i=0;i<accessoryCount;i++) {
      JsonObject it = arr.createNestedObject();
      it["id"] = accessories[i].id;
      it["name"] = accessories[i].name;
      it["mac"] = accessories[i].hasMac ? macToStr(accessories[i].mac) : "";
      it["present"] = accessories[i].present;
      it["lastHbMs"] = accessories[i].lastHbMs;
      it["mode"] = accessories[i].mode;
      it["batteryMv"] = accessories[i].batteryMv;
    }
    String scanOut; serializeJson(s, scanOut);
    wsSend(num, scanOut);
    return;
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("WS client disconnected: id=%u\n", num);
    return;
  } else if (type == WStype_TEXT) {
    // Lightweight raw log for debugging WS frames
    String s = String((char*)payload);
    Serial.printf("WS RX from client %u: %s\n", num, s.c_str());

    // Try parse JSON to handle ping/pong quickly
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, s);
    if (!err) {
      if (doc.containsKey("type")) {
        String t = String((const char*)doc["type"]);
        if (t == "ping") {
          // reply with pong to the same client
          StaticJsonDocument<64> r;
          r["type"] = "pong";
          String out; serializeJson(r, out);
          wsSend(num, out);
          Serial.printf("WS: pong -> client %u\n", num);
          return; // handled
        }
      }
    }
    // Not a ping/pong or not handled above: forward to existing handler
    handleWSMessage(num, type, payload, length);
    return;
  }
}

// ---------------- HTTP handlers ----------------
void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(404, "text/plain", "index.html missing (LittleFS not flashed?)");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}
void handleConfigGet() {
  if (LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    server.streamFile(f, "application/json");
    f.close();
  } else {
    server.send(404, "text/plain", "no config");
  }
}

// ---------------- Config load (already defined above) ----------------
// loadConfig() implemented earlier

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Master boot");
  Serial.print("Free heap at boot: ");
  Serial.println(esp_get_free_heap_size());

  if (!LittleFS.begin()) {
    Serial.println("LittleFS.begin FAILED");
  } else Serial.println("LittleFS OK");

  // AP + STA on channel 9 (user requested)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, 9);
  Serial.println("AP started (channel 9)");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  WiFi.disconnect();

  accessoryCount = 0;
  for (int i=0;i<8;i++) {
    accessories[i].hasMac = false;
    accessories[i].present = false;
    for (int j=0;j<6;j++) accessories[i].mac[j] = 0;
  }
  for (int i=0;i<MAX_PENDING;i++) {
    pending[i].active = false;
    pending[i].lastBroadcastAttempts = -1;
  }

  loadConfig();

  // Force WiFi radio channel for ESP-NOW before esp_now_init
  esp_err_t ch = esp_wifi_set_channel(9, WIFI_SECOND_CHAN_NONE);
  Serial.printf("esp_wifi_set_channel -> %d\n", (int)ch);

  // init ESP-NOW (do NOT change WiFi.mode after this)
  esp_err_t r = esp_now_init();
  Serial.print("esp_now_init -> ");
  Serial.println((int)r);
  if (r != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    ESP.restart();
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  // ensure broadcast peer exists on channel 9
  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_peer_info_t pb = {};
  memcpy(pb.peer_addr, bcast, 6);
  pb.channel = 9;
  pb.ifidx = WIFI_IF_STA;
  pb.encrypt = false;
  esp_err_t rr = esp_now_add_peer(&pb);
  Serial.printf("esp_now_add_peer(bcast) -> %d\n", (int)rr);

  // HTTP server
  server.on("/", handleRoot);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.serveStatic("/app.js", LittleFS, "/app.js");
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.begin();

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  Serial.println("Master ready");
}

// ---------------- Loop ----------------
unsigned long lastPresenceCheck = 0;
unsigned long lastApCheck = 0;
const unsigned long AP_CHECK_INTERVAL_MS = 10000;
unsigned long lastHeapLog = 0;
const unsigned long HEAP_LOG_INTERVAL_MS = 30000;

void loop() {
  server.handleClient();
  webSocket.loop();

  // Process any ESP-NOW packets received in the WiFi-task callback (safe here).
  drainRxQueue();

  // AP watchdog: periodically re-assert WiFi power-save OFF (the single most
  // important setting for keeping the phone connected). If the AP IP ever
  // looks invalid, restart softAP as a last resort.
  if (millis() - lastApCheck > AP_CHECK_INTERVAL_MS) {
    lastApCheck = millis();
    esp_wifi_set_ps(WIFI_PS_NONE); // ensure modem-sleep never creeps back on
    IPAddress apip = WiFi.softAPIP();
    if (apip.toString() == "0.0.0.0") {
      Serial.println("AP appears down, restarting softAP...");
      WiFi.softAP(AP_SSID, AP_PASS, 9);
      esp_wifi_set_ps(WIFI_PS_NONE); // softAP() may re-enable power-save
      Serial.print("AP restarted, IP: ");
      Serial.println(WiFi.softAPIP());
    }
  }

  // Process pending queue every loop: it is self-throttled internally
  // (one radio send per UNICAST_GAP_MS), so calling it often just lets the
  // queue flush at the right cadence without blocking.
  processPendingQueue();

  // presence check and status broadcasts
  if (millis() - lastPresenceCheck > 2000) {
    lastPresenceCheck = millis();
    for (int i=0;i<accessoryCount;i++) {
      if (accessories[i].hasMac) {
        if (millis() - accessories[i].lastHbMs > 10000) {
          if (accessories[i].present) {
            accessories[i].present = false;
            if (macIsValid(accessories[i].mac) && esp_now_is_peer_exist(accessories[i].mac)) {
              esp_err_t r = esp_now_del_peer(accessories[i].mac);
              Serial.print("esp_now_del_peer -> ");
              Serial.println((int)r);
            }
            StaticJsonDocument<256> d;
            d["type"]="status";
            d["id"]=accessories[i].id;
            d["present"]=false;
            String out; serializeJson(d,out);
            wsBroadcast(out);
          }
        } else {
          if (!accessories[i].present) {
            accessories[i].present = true;
            StaticJsonDocument<256> d;
            d["type"]="status";
            d["id"]=accessories[i].id;
            d["present"]=true;
            String out; serializeJson(d,out);
            wsBroadcast(out);
          }
        }
      }
    }
  }

  // HB batch broadcast (aggregate to reduce load)
  if (hbDirty && millis() - lastHbBroadcast >= HB_BROADCAST_INTERVAL_MS) {
    lastHbBroadcast = millis();
    hbDirty = false;
    StaticJsonDocument<2048> d;
    d["type"] = "hb_batch";
    JsonArray arr = d.createNestedArray("items");
    for (int i=0;i<accessoryCount;i++) {
      JsonObject it = arr.createNestedObject();
      it["id"] = accessories[i].id;
      it["mode"] = accessories[i].mode;
      it["batt"] = accessories[i].batteryMv;
      it["present"] = accessories[i].present;
      it["mac"] = accessories[i].hasMac ? macToStr(accessories[i].mac) : "";
      it["lastHbMs"] = accessories[i].lastHbMs;
    }
    String out; serializeJson(d,out);
    Serial.printf("Broadcasting hb_batch (%d items)\n", accessoryCount);
    wsBroadcast(out);
  }

  // periodic heap log
  if (millis() - lastHeapLog > HEAP_LOG_INTERVAL_MS) {
    lastHeapLog = millis();
    Serial.printf("Free heap: %u\n", esp_get_free_heap_size());
  }

  delay(10);
}

// ---------------- WebSocket message handler ----------------
void handleWSMessage(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) {
    String s = String((char*)payload);
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, s);
    if (err) return;
    String action = doc["action"];
    if (action == "send") {
      JsonArray arr = doc["targets"].as<JsonArray>();
      String cmd = doc["cmd"];
      String arg = doc["arg"];
      String msg = "CMD|";
      msg += cmd;
      if (arg.length()) { msg += "|"; msg += arg; }
      for (JsonVariant v : arr) {
        int id = v.as<int>();
        for (int i=0;i<accessoryCount;i++) {
          if (accessories[i].id == id) {
            if (!accessories[i].hasMac) {
              StaticJsonDocument<256> d;
              d["type"]="send_result";
              d["id"]=id;
              d["cmd"]=cmd;
              d["ok"]=false;
              d["reason"]="no_mac";
              String out; serializeJson(d,out);
              wsSend(num, out);
              break;
            }
            const uint8_t *mac = accessories[i].mac;
            bool queued = queueOrReplaceCommand(mac, id, cmd, msg);
            if (!queued) {
              StaticJsonDocument<256> d;
              d["type"]="send_result";
              d["id"]=id;
              d["cmd"]=cmd;
              d["ok"]=false;
              d["reason"]="queue_full";
              String out; serializeJson(d,out);
              wsSend(num, out);
            } else {
              StaticJsonDocument<256> d;
              d["type"]="send_result";
              d["id"]=id;
              d["cmd"]=cmd;
              d["ok"]=true;
              d["queued"]=true;
              String out; serializeJson(d,out);
              wsSend(num, out);
            }
            break;
          }
        }
      }
    } else if (action == "send_all") {
      String cmd = doc["cmd"];
      String arg = doc["arg"];
      String msg = "CMD|";
      msg += cmd;
      if (arg.length()) { msg += "|"; msg += arg; }
      for (int i=0;i<accessoryCount;i++) {
        if (accessories[i].hasMac) {
          bool queued = queueOrReplaceCommand(accessories[i].mac, accessories[i].id, cmd, msg);
          StaticJsonDocument<256> d;
          d["type"]="send_result";
          d["id"]=accessories[i].id;
          d["cmd"]=cmd;
          d["ok"]=queued;
          if (!queued) d["reason"]="queue_full";
          String out; serializeJson(d,out);
          wsSend(num, out);
        } else {
          StaticJsonDocument<256> d;
          d["type"]="send_result";
          d["id"]=accessories[i].id;
          d["cmd"]=cmd;
          d["ok"]=false;
          d["reason"]="no_mac";
          String out; serializeJson(d,out);
          wsSend(num, out);
        }
      }
    } else if (action == "scan") {
      StaticJsonDocument<1024> d;
      d["type"]="scan_result";
      JsonArray arr = d.createNestedArray("items");
      for (int i=0;i<accessoryCount;i++) {
        JsonObject it = arr.createNestedObject();
        it["id"] = accessories[i].id;
        it["name"] = accessories[i].name;
        it["mac"] = accessories[i].hasMac ? macToStr(accessories[i].mac) : "";
        it["present"] = accessories[i].present;
        it["lastHbMs"] = accessories[i].lastHbMs;
        it["mode"] = accessories[i].mode;
        it["batteryMv"] = accessories[i].batteryMv;
      }
      String out; serializeJson(d,out);
      wsSend(num, out);
    } else if (action == "finale") {
      String params = doc["params"] | "";
      String msg = "CMD|FINAL|" + params;
      for (int i=0;i<accessoryCount;i++) {
        if (accessories[i].hasMac) {
          bool queued = queueOrReplaceCommand(accessories[i].mac, accessories[i].id, "FINAL", msg);
          StaticJsonDocument<256> d;
          d["type"]="send_result";
          d["id"]=accessories[i].id;
          d["cmd"]="FINAL";
          d["ok"]=queued;
          if (!queued) d["reason"]="queue_full";
          String out; serializeJson(d,out);
          wsSend(num, out);
        }
      }
    }
  }
}
