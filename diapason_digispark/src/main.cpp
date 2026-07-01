/* Diapason des Sentiers — Digispark (ATtiny85)
   ------------------------------------------------------------------
   Prop AUTONOME (pas de réseau ESP-NOW). Lit une carte RFID cachée dans un
   indice ; le contenu texte "true"/"false" décide de la réponse :
     - "true"  -> "vibre juste"        : pulsation propre et nette
     - "false" -> "bourdonnement laid" : vibration hachée, dissonante

   DEUX sources de verdict :
     1) RFID PN532 (I2C)      -> lecture réelle des cartes
     2) OVERRIDE télécommande IR (NEC) -> BACKUP si le RFID ne marche pas
        (l'IR est prioritaire : une touche mappée déclenche toujours la réponse)

   Brochage (ATtiny85 / Digispark), P5 = reset :
     P0 (PB0) = SDA  (PN532)     -- pull-ups sur la carte PN532 requis
     P2 (PB2) = SCL  (PN532)
     P3 (PB3) = sortie récepteur IR (TSOP/VS1838B, 38 kHz)
     P4 (PB4) = moteur de vibration (via transistor + diode de roue libre)
     P1 (PB1) = LED interne (état, optionnel)

   ⚠️ La partie PN532 est écrite au mieux mais NON testée sur matériel : à
   valider/ajuster au banc. L'override IR est le filet de sécurité fiable.
*/

#include <Arduino.h>
#include <avr/eeprom.h>

#define USE_PN532 1            // 0 = désactive le RFID (IR seul)
#define USE_IR    1            // 0 = désactive l'override IR

// ---- Broches ----
#define SDA_PIN   0
#define SCL_PIN   2
#define IR_PIN    3
#define MOTOR_PIN 4
#define LED_PIN   1

// ---- Codes télécommande : APPRIS automatiquement, stockés en EEPROM ----
// Pas besoin de connaître les codes NEC : au 1er démarrage (ou si tu presses une
// touche dans les 2.5 s après la mise sous tension), le Diapason apprend TA
// touche "vrai" puis TA touche "faux".
#define EE_MAGIC     0x5A
uint8_t g_trueCode = 0, g_falseCode = 0;
bool    g_learned  = false;

enum Verdict { V_NONE = 0, V_TRUE = 1, V_FALSE = 2 };

// ---- PRNG compact (pour le "laid") ----
static uint16_t rndState = 0xACE1;
static uint8_t rnd8() { rndState = rndState * 25173u + 13849u; return rndState >> 8; }

// ================= Retour vibration =================
static inline void motor(uint8_t level) { analogWrite(MOTOR_PIN, level); }

void feedbackTrue() {                 // "vibre juste" : 3 pulsations propres
  for (uint8_t i = 0; i < 3; i++) {
    for (int v = 0;   v <= 255; v += 15) { motor((uint8_t)v); delay(6); }
    delay(120);
    for (int v = 255; v >= 0;   v -= 15) { motor((uint8_t)v); delay(6); }
    delay(90);
  }
  motor(0);
}
void feedbackFalse() {                // "bourdonnement laid" : ~900 ms haché
  unsigned long t0 = millis();
  while (millis() - t0 < 900) {
    motor(60 + (rnd8() % 195));
    delay(6 + (rnd8() % 22));
    motor(0);
    delay(3 + (rnd8() % 15));
  }
  motor(0);
}
void playFeedback(Verdict v) {
  digitalWrite(LED_PIN, HIGH);
  if (v == V_TRUE) feedbackTrue(); else if (v == V_FALSE) feedbackFalse();
  digitalWrite(LED_PIN, LOW);
}

// Clignotements de diagnostic sur la LED interne (P1).
void blink(uint8_t n, uint16_t ms) {
  for (uint8_t i = 0; i < n; i++) {
    digitalWrite(LED_PIN, HIGH); delay(ms);
    digitalWrite(LED_PIN, LOW);  delay(ms);
  }
}

// ================= Override IR (NEC) =================
#if USE_IR
// Décodeur NEC minimal, bloquant le temps d'une trame (~68 ms). Renvoie le
// code commande (0..255) ou -1 si rien/erreur. IR idle = HIGH, actif = LOW.
int irReadNEC() {
  if (digitalRead(IR_PIN) != LOW) return -1;
  unsigned long t0 = micros();
  while (digitalRead(IR_PIN) == LOW) if (micros() - t0 > 12000UL) return -1;
  unsigned long mark = micros() - t0;
  if (mark < 7000 || mark > 11000) return -1;               // entête 9 ms
  t0 = micros();
  while (digitalRead(IR_PIN) == HIGH) if (micros() - t0 > 7000UL) return -1;
  unsigned long space = micros() - t0;
  if (space < 3000 || space > 6000) return -1;              // 4.5 ms (repeat ignoré)
  uint32_t data = 0;
  for (uint8_t i = 0; i < 32; i++) {
    t0 = micros();
    while (digitalRead(IR_PIN) == LOW)  if (micros() - t0 > 2000UL) return -1; // 560 us
    t0 = micros();
    while (digitalRead(IR_PIN) == HIGH) if (micros() - t0 > 3000UL) return -1;
    unsigned long s = micros() - t0;
    data >>= 1;
    if (s > 1000) data |= 0x80000000UL;                     // space long = '1'
  }
  uint8_t cmd  = (data >> 16) & 0xFF;
  uint8_t ncmd = (data >> 24) & 0xFF;
  if ((uint8_t)(cmd ^ ncmd) != 0xFF) return -1;             // contrôle NEC
  return cmd;
}

void loadCodes() {
  if (eeprom_read_byte((const uint8_t*)0) == EE_MAGIC) {
    g_trueCode  = eeprom_read_byte((const uint8_t*)1);
    g_falseCode = eeprom_read_byte((const uint8_t*)2);
    g_learned = true;
  }
}
void storeCodes() {
  eeprom_update_byte((uint8_t*)1, g_trueCode);
  eeprom_update_byte((uint8_t*)2, g_falseCode);
  eeprom_update_byte((uint8_t*)0, EE_MAGIC);
}
// Apprend la touche "vrai" puis la touche "faux" (différente). La LED clignote
// lentement (attente "vrai") puis vite (attente "faux") ; chaque capture est
// confirmée par la vibration correspondante.
void learnCodes() {
  int c;
  do { digitalWrite(LED_PIN, (millis() / 200) & 1); c = irReadNEC(); } while (c < 0);
  g_trueCode = (uint8_t)c; feedbackTrue(); delay(400);
  int d;
  do { digitalWrite(LED_PIN, (millis() / 80) & 1); d = irReadNEC(); } while (d < 0 || d == c);
  g_falseCode = (uint8_t)d; feedbackFalse();
  g_learned = true; storeCodes();
  digitalWrite(LED_PIN, LOW);
}
#endif

// ================= RFID PN532 (I2C logiciel) =================
#if USE_PN532
// --- I2C bit-bang (open-drain via bascule INPUT/OUTPUT ; pull-ups externes) ---
static inline void sdaHigh() { pinMode(SDA_PIN, INPUT); }
static inline void sdaLow()  { pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, LOW); }
static inline void sclHigh() { pinMode(SCL_PIN, INPUT); }
static inline void sclLow()  { pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, LOW); }
static inline void i2cDelay(){ delayMicroseconds(5); }

void i2cStart() { sdaHigh(); sclHigh(); i2cDelay(); sdaLow(); i2cDelay(); sclLow(); i2cDelay(); }
void i2cStop()  { sdaLow(); i2cDelay(); sclHigh(); i2cDelay(); sdaHigh(); i2cDelay(); }

bool i2cWrite(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    if (b & 0x80) sdaHigh(); else sdaLow();
    i2cDelay(); sclHigh(); i2cDelay(); sclLow(); i2cDelay();
    b <<= 1;
  }
  sdaHigh(); i2cDelay(); sclHigh(); i2cDelay();     // lit l'ACK
  bool ack = (digitalRead(SDA_PIN) == LOW);
  sclLow(); i2cDelay();
  return ack;
}
uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sdaHigh();
  for (uint8_t i = 0; i < 8; i++) {
    i2cDelay(); sclHigh(); i2cDelay();
    b = (b << 1) | (digitalRead(SDA_PIN) ? 1 : 0);
    sclLow();
  }
  if (ack) sdaLow(); else sdaHigh();
  i2cDelay(); sclHigh(); i2cDelay(); sclLow(); sdaHigh(); i2cDelay();
  return b;
}

#define PN532_W 0x48           // adresse écriture (0x24<<1)
#define PN532_R 0x49           // adresse lecture

bool pn532Write(uint8_t cmd, const uint8_t* d, uint8_t n) {
  uint8_t len = n + 2;                          // TFI + cmd + params
  i2cStart();
  if (!i2cWrite(PN532_W)) { i2cStop(); return false; }
  i2cWrite(0x00); i2cWrite(0x00); i2cWrite(0xFF);
  i2cWrite(len); i2cWrite((uint8_t)(~len + 1));
  uint8_t sum = 0xD4; i2cWrite(0xD4);
  i2cWrite(cmd); sum += cmd;
  for (uint8_t i = 0; i < n; i++) { i2cWrite(d[i]); sum += d[i]; }
  i2cWrite((uint8_t)(~sum + 1)); i2cWrite(0x00);
  i2cStop();
  return true;
}
bool pn532WaitReady(uint16_t ms) {
  uint32_t t = millis();
  while (millis() - t < ms) {
    i2cStart(); i2cWrite(PN532_R);
    uint8_t s = i2cRead(false);
    i2cStop();
    if (s & 1) return true;
    delay(2);
  }
  return false;
}
// Lit un cadre brut (jusqu'à 'max' octets) après l'octet d'état. Renvoie le nb lu.
uint8_t pn532ReadRaw(uint8_t* buf, uint8_t max) {
  i2cStart(); i2cWrite(PN532_R);
  i2cRead(true);                                // octet d'état (ready), ignoré
  for (uint8_t i = 0; i < max; i++) buf[i] = i2cRead(i < (max - 1));
  i2cStop();
  return max;
}
void pn532DiscardAck() { uint8_t tmp[8]; if (pn532WaitReady(50)) pn532ReadRaw(tmp, 7); }

// Envoie une commande, renvoie la charge utile (cmd de réponse + params) dans
// resp ; renvoie sa longueur (0 si échec).
uint8_t pn532Command(uint8_t cmd, const uint8_t* d, uint8_t n, uint8_t* resp, uint8_t respMax) {
  if (!pn532Write(cmd, d, n)) return 0;
  pn532DiscardAck();
  if (!pn532WaitReady(300)) return 0;
  uint8_t raw[36];
  pn532ReadRaw(raw, sizeof(raw));
  // cherche 00 00 FF
  uint8_t p = 0;
  while (p < 30 && !(raw[p] == 0x00 && raw[p+1] == 0x00 && raw[p+2] == 0xFF)) p++;
  if (p >= 30) return 0;
  uint8_t len = raw[p + 3];                     // TFI + données
  if (len < 2 || len > respMax + 1) return 0;
  // raw[p+5]=TFI(0xD5), raw[p+6]=cmd réponse, puis params
  uint8_t count = len - 1;                       // cmd + params
  for (uint8_t i = 0; i < count && i < respMax; i++) resp[i] = raw[p + 6 + i];
  return count;
}

bool pn532Begin() {
  const uint8_t sam[] = { 0x01, 0x14, 0x01 };    // SAMConfiguration: mode normal
  uint8_t r[8];
  return pn532Command(0x14, sam, sizeof(sam), r, sizeof(r)) > 0;
}

static bool contains(const uint8_t* b, uint8_t n, const char* s, uint8_t sl) {
  if (n < sl) return false;
  for (uint8_t i = 0; i + sl <= n; i++) {
    uint8_t k = 0; while (k < sl && b[i + k] == (uint8_t)s[k]) k++;
    if (k == sl) return true;
  }
  return false;
}

Verdict pn532Verdict() {
  // 1) détecte une carte ISO14443A (106 kbps)
  const uint8_t lp[] = { 0x01, 0x00 };
  uint8_t r[32];
  uint8_t rl = pn532Command(0x4A, lp, sizeof(lp), r, sizeof(r)); // -> 0x4B
  if (rl < 2 || r[1] == 0x00) return V_NONE;     // r[1] = NbTg
  // 2) lit une page NTAG (READ 0x30, page 4) via InDataExchange (0x40)
  const uint8_t rd[] = { 0x01, 0x30, 0x04 };
  uint8_t d[24];
  uint8_t dl = pn532Command(0x40, rd, sizeof(rd), d, sizeof(d)); // -> 0x41 [status][16o]
  if (dl >= 2) {
    const uint8_t* data = &d[2];                 // saute cmd(0x41) + status
    uint8_t n = dl - 2;
    if (contains(data, n, "true", 4))  return V_TRUE;
    if (contains(data, n, "false", 5)) return V_FALSE;
  }
  // Carte présente mais texte non lisible : on renvoie NONE (l'IR reste le backup).
  return V_NONE;
}
#endif

void setup() {
  pinMode(MOTOR_PIN, OUTPUT); motor(0);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
#if USE_IR
  pinMode(IR_PIN, INPUT);
#endif
  motor(180); delay(60); motor(0);               // "bip" tactile de démarrage
#if USE_PN532
  sdaHigh(); sclHigh();
  bool pnOK = pn532Begin();
  // Diagnostic : 2 clignotements lents = PN532 détecté ; 6 rapides = absent.
  blink(pnOK ? 2 : 6, pnOK ? 250 : 90);
#endif

#if USE_IR
  loadCodes();
  // Fenêtre de (ré)apprentissage : presser une touche dans les 2.5 s au démarrage
  // force l'apprentissage. Sinon, apprend seulement si rien n'est encore mémorisé.
  bool relearn = false;
  uint32_t t = millis();
  while (millis() - t < 2500) { if (irReadNEC() >= 0) { relearn = true; break; } }
  if (!g_learned || relearn) learnCodes();
#endif
}

void loop() {
#if USE_IR
  int c = irReadNEC();                            // override prioritaire
  if (c >= 0) {
    if (g_learned && (uint8_t)c == g_trueCode)       playFeedback(V_TRUE);
    else if (g_learned && (uint8_t)c == g_falseCode) playFeedback(V_FALSE);
    delay(150);
    return;
  }
#endif
#if USE_PN532
  Verdict v = pn532Verdict();
  if (v != V_NONE) { playFeedback(v); delay(400); } // évite la relecture immédiate
#endif
}
