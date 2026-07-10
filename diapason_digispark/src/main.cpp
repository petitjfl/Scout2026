/* Diapason des Sentiers — Digispark Pro (ATtiny167)
   ------------------------------------------------------------------
   Prop AUTONOME. Lit une carte RFID (RC522) contenant le texte "true"/"false"
   et répond par le moteur de vibration :
     - "true"  -> "vibre juste"        : pulsation propre
     - "false" -> "bourdonnement laid" : vibration hachée
   OVERRIDE télécommande IR (NEC) prioritaire = BACKUP si le RC522 ne marche pas.

   Digispark Pro (ATtiny167) : on utilise les broches SPI matérielles du core
   Digistump (MOSI=10, MISO=8, SCK=11, SS=12). Le récepteur IR n'est plus
   partagé avec MISO ; il a sa propre entrée.

   Brochage utilisé :
     D10 = MOSI  -> RC522
     D11 = SCK   -> RC522
     D8  = MISO  <- RC522
     D12 = SS/NSS-> RC522
     D2  = entrée récepteur IR (TSOP/VS1838B)
     D1  = moteur (transistor + diode)
     RST du RC522 -> VCC (reset logiciel géré par commande)

   ⚠️ Pilote RC522 en SPI logiciel NON testé sur matériel : à valider au banc.
   Cartes NTAG/Ultralight (texte NDEF) supportées ; MIFARE Classic nécessiterait
   une authentification (non gérée ici).
*/

#include <Arduino.h>
#include <avr/eeprom.h>

#define USE_RC522 1
#define USE_IR    1

// ---- Broches (Digispark Pro / ATtiny167) ----
#define PIN_MOSI MOSI
#define PIN_SCK  SCK
#define PIN_MISO MISO
#define PIN_SS   SS
#define MOTOR_PIN 1
#define LED_PIN   1         // LED/PWM sur D1
#define IR_PIN    2         // entrée dédiée IR

#define EE_MAGIC 0x5A
uint8_t g_trueCode = 0, g_falseCode = 0;
bool    g_learned  = false;

enum Verdict { V_NONE = 0, V_TRUE = 1, V_FALSE = 2 };

static uint16_t rndState = 0xACE1;
static uint8_t rnd8() { rndState = rndState * 25173u + 13849u; return rndState >> 8; }

// ================= Retour vibration =================
static inline void motor(uint8_t level) { analogWrite(MOTOR_PIN, level); }
void feedbackTrue() {
  for (uint8_t i = 0; i < 3; i++) {
    for (int v = 0;   v <= 255; v += 15) { motor((uint8_t)v); delay(6); }
    delay(120);
    for (int v = 255; v >= 0;   v -= 15) { motor((uint8_t)v); delay(6); }
    delay(90);
  }
  motor(0);
}
void feedbackFalse() {
  unsigned long t0 = millis();
  while (millis() - t0 < 900) {
    motor(60 + (rnd8() % 195)); delay(6 + (rnd8() % 22));
    motor(0);                    delay(3 + (rnd8() % 15));
  }
  motor(0);
}
void playFeedback(Verdict v) { if (v == V_TRUE) feedbackTrue(); else if (v == V_FALSE) feedbackFalse(); }
void blink(uint8_t n, uint16_t ms) {
  for (uint8_t i = 0; i < n; i++) { digitalWrite(LED_PIN, HIGH); delay(ms); digitalWrite(LED_PIN, LOW); delay(ms); }
}

// ================= Override IR (NEC) =================
#if USE_IR
int irReadNEC() {
  if (digitalRead(IR_PIN) != LOW) return -1;
  unsigned long t0 = micros();
  while (digitalRead(IR_PIN) == LOW) if (micros() - t0 > 12000UL) return -1;
  unsigned long mark = micros() - t0;
  if (mark < 7000 || mark > 11000) return -1;
  t0 = micros();
  while (digitalRead(IR_PIN) == HIGH) if (micros() - t0 > 7000UL) return -1;
  unsigned long space = micros() - t0;
  if (space < 3000 || space > 6000) return -1;
  uint32_t data = 0;
  for (uint8_t i = 0; i < 32; i++) {
    t0 = micros(); while (digitalRead(IR_PIN) == LOW)  if (micros() - t0 > 2000UL) return -1;
    t0 = micros(); while (digitalRead(IR_PIN) == HIGH) if (micros() - t0 > 3000UL) return -1;
    unsigned long s = micros() - t0;
    data >>= 1; if (s > 1000) data |= 0x80000000UL;
  }
  uint8_t cmd = (data >> 16) & 0xFF, ncmd = (data >> 24) & 0xFF;
  if ((uint8_t)(cmd ^ ncmd) != 0xFF) return -1;
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
void learnCodes() {
  int c; do { digitalWrite(LED_PIN, (millis() / 200) & 1); c = irReadNEC(); } while (c < 0);
  g_trueCode = (uint8_t)c; feedbackTrue(); delay(400);
  int d; do { digitalWrite(LED_PIN, (millis() / 80) & 1); d = irReadNEC(); } while (d < 0 || d == c);
  g_falseCode = (uint8_t)d; feedbackFalse();
  g_learned = true; storeCodes(); digitalWrite(LED_PIN, LOW);
}
#endif

// ================= RC522 (SPI logiciel) =================
#if USE_RC522
// Registres MFRC522
#define R_Command 0x01
#define R_ComIrq  0x04
#define R_DivIrq  0x05
#define R_Error   0x06
#define R_FIFOData 0x09
#define R_FIFOLevel 0x0A
#define R_BitFraming 0x0D
#define R_Mode    0x11
#define R_TxControl 0x14
#define R_TxASK   0x15
#define R_CRCResH 0x21
#define R_CRCResL 0x22
#define R_TMode   0x2A
#define R_TPrescaler 0x2B
#define R_TReloadH 0x2C
#define R_TReloadL 0x2D
#define R_Version 0x37
// Commandes PCD
#define C_Idle 0x00
#define C_CalcCRC 0x03
#define C_Transceive 0x0C
#define C_SoftReset 0x0F

uint8_t spiXfer(uint8_t b) {
  uint8_t r = 0;
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW); b <<= 1;
    digitalWrite(PIN_SCK, HIGH);
    r <<= 1; if (digitalRead(PIN_MISO)) r |= 1;
    digitalWrite(PIN_SCK, LOW);
  }
  return r;
}
void rcWrite(uint8_t reg, uint8_t val) {
  digitalWrite(PIN_SS, LOW); spiXfer((reg << 1) & 0x7E); spiXfer(val); digitalWrite(PIN_SS, HIGH);
}
uint8_t rcRead(uint8_t reg) {
  digitalWrite(PIN_SS, LOW); spiXfer(0x80 | ((reg << 1) & 0x7E)); uint8_t v = spiXfer(0); digitalWrite(PIN_SS, HIGH);
  return v;
}
void rcSetBit(uint8_t reg, uint8_t m)   { rcWrite(reg, rcRead(reg) | m); }
void rcClearBit(uint8_t reg, uint8_t m) { rcWrite(reg, rcRead(reg) & ~m); }

void rcCalcCRC(uint8_t* d, uint8_t n, uint8_t* out) {
  rcWrite(R_Command, C_Idle);
  rcWrite(R_DivIrq, 0x04);
  rcWrite(R_FIFOLevel, 0x80);
  for (uint8_t i = 0; i < n; i++) rcWrite(R_FIFOData, d[i]);
  rcWrite(R_Command, C_CalcCRC);
  for (uint16_t j = 0; j < 5000; j++) if (rcRead(R_DivIrq) & 0x04) break;
  rcWrite(R_Command, C_Idle);
  out[0] = rcRead(R_CRCResL); out[1] = rcRead(R_CRCResH);
}
// Transmet/reçoit une trame ; renvoie true si OK, remplit back/backLen.
bool rcTransceive(uint8_t* send, uint8_t sendLen, uint8_t* back, uint8_t* backLen, uint8_t lastBits) {
  rcWrite(R_Command, C_Idle);
  rcWrite(R_ComIrq, 0x7F);
  rcWrite(R_FIFOLevel, 0x80);
  for (uint8_t i = 0; i < sendLen; i++) rcWrite(R_FIFOData, send[i]);
  rcWrite(R_BitFraming, lastBits);
  rcWrite(R_Command, C_Transceive);
  rcSetBit(R_BitFraming, 0x80);
  uint16_t i;
  for (i = 3000; i > 0; i--) { uint8_t i2 = rcRead(R_ComIrq); if (i2 & 0x30) break; if (i2 & 0x01) { i = 0; break; } }
  rcClearBit(R_BitFraming, 0x80);
  if (i == 0) return false;
  if (rcRead(R_Error) & 0x13) return false;
  uint8_t n = rcRead(R_FIFOLevel);
  if (n > *backLen) n = *backLen;
  for (uint8_t k = 0; k < n; k++) back[k] = rcRead(R_FIFOData);
  *backLen = n;
  return true;
}
bool rcRequestA(uint8_t* atqa) { uint8_t c = 0x26, bl = 2; return rcTransceive(&c, 1, atqa, &bl, 0x07); }
bool rcAnticoll(uint8_t* uid)  { uint8_t s[2] = {0x93, 0x20}, bl = 5; return rcTransceive(s, 2, uid, &bl, 0x00); }
bool rcSelect(uint8_t* uid) {
  uint8_t b[9] = {0x93, 0x70, uid[0], uid[1], uid[2], uid[3], uid[4], 0, 0};
  rcCalcCRC(b, 7, &b[7]);
  uint8_t back[3], bl = 3; return rcTransceive(b, 9, back, &bl, 0x00);
}
bool rcRead16(uint8_t page, uint8_t* out) {
  uint8_t b[4] = {0x30, page, 0, 0}; rcCalcCRC(b, 2, &b[2]);
  uint8_t bl = 18; return rcTransceive(b, 4, out, &bl, 0x00) && bl >= 16;
}
static bool contains(const uint8_t* b, uint8_t n, const char* s, uint8_t sl) {
  if (n < sl) return false;
  for (uint8_t i = 0; i + sl <= n; i++) { uint8_t k = 0; while (k < sl && b[i + k] == (uint8_t)s[k]) k++; if (k == sl) return true; }
  return false;
}
void rc522Begin() {
  pinMode(PIN_SS, OUTPUT);  digitalWrite(PIN_SS, HIGH);
  pinMode(PIN_SCK, OUTPUT); digitalWrite(PIN_SCK, LOW);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_MISO, INPUT);
  rcWrite(R_Command, C_SoftReset); delay(50);
  rcWrite(R_TMode, 0x8D); rcWrite(R_TPrescaler, 0x3E);
  rcWrite(R_TReloadL, 30); rcWrite(R_TReloadH, 0);
  rcWrite(R_TxASK, 0x40); rcWrite(R_Mode, 0x3D);
  uint8_t v = rcRead(R_TxControl); if (!(v & 0x03)) rcWrite(R_TxControl, v | 0x03); // antenne on
}
bool rc522Present() { uint8_t v = rcRead(R_Version); return (v != 0x00 && v != 0xFF); }
Verdict rc522Verdict() {
  uint8_t atqa[2];
  if (!rcRequestA(atqa)) return V_NONE;
  uint8_t uid[5];
  if (!rcAnticoll(uid)) return V_NONE;
  if (!rcSelect(uid))   return V_NONE;
  uint8_t data[18];
  if (rcRead16(4, data)) {
    if (contains(data, 16, "true", 4))  return V_TRUE;
    if (contains(data, 16, "false", 5)) return V_FALSE;
  }
  return V_NONE;
}
#endif

void setup() {
  pinMode(MOTOR_PIN, OUTPUT); motor(0);
  motor(180); delay(60); motor(0);               // "bip" de démarrage
#if USE_RC522
  rc522Begin();
  bool ok = rc522Present();
  blink(ok ? 2 : 6, ok ? 250 : 90);              // 2 lents = RC522 OK ; 6 rapides = absent
#endif
#if USE_IR
  pinMode(IR_PIN, INPUT);
  loadCodes();
  bool relearn = false; uint32_t t = millis();
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
    delay(150); return;
  }
#endif
#if USE_RC522
  Verdict v = rc522Verdict();
  if (v != V_NONE) { playFeedback(v); delay(400); }
#endif
}
