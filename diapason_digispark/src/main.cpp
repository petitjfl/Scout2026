/* Diapason des Sentiers — Digispark (ATtiny85)
   ------------------------------------------------------------------
   Prop AUTONOME (pas de réseau ESP-NOW). Un animateur l'approche d'un indice ;
   une carte RFID cachée dans l'indice contient le texte "true" ou "false".
     - carte "true"  -> le diapason "vibre juste" : pulsation propre et nette
     - carte "false" -> "bourdonnement laid" : vibration hachée, dissonante

   Matériel (ATtiny85 / Digispark) — 5 broches utiles (P0..P4, P5=reset) :
     P4 (PB4) -> transistor -> moteur de vibration (+ diode de roue libre)
     P0..P3   -> lecteur RFID (voir la section INTÉGRATION RFID plus bas)

   ⚠️ Contraintes ATtiny85 : ~6 Ko flash, 512 o RAM, pas de vrai périphérique
   SPI (USI). La lib MFRC522 standard risque de ne PAS tenir/compiler ici. Deux
   pistes réalistes selon le module final :
     A) RFID I2C (PN532 en mode I2C) -> 2 broches (SDA/SCL), lib légère TinyWire.
     B) RFID SPI (RC522) via USI -> pilote minimal maison (SCK=P2, DI=P0, DO=P1,
        CS=P3, RST relié à VCC pour économiser une broche).
   Si le flash déborde, passer sur "Digispark Pro" (ATtiny167, 14 Ko).

   Pour valider la MÉCANIQUE DE VIBRATION dès maintenant sans RFID, ce firmware
   démarre en MODE TEST : un bouton sur P0 (vers GND) simule les cartes.
     - appui court (<600 ms) = carte "true"
     - appui long  (>=600 ms) = carte "false"
   Passer DIAPASON_TEST à 0 quand le lecteur RFID est intégré.
*/

#include <Arduino.h>

#define DIAPASON_TEST 1        // 1 = bouton de test ; 0 = lecture RFID réelle

#define MOTOR_PIN 4            // P4 (PB4, PWM OC1B) -> transistor -> moteur
#define TEST_BTN_PIN 0         // P0 vers GND (INPUT_PULLUP) — mode test seulement

enum Verdict { V_NONE = 0, V_TRUE = 1, V_FALSE = 2 };

// Petit générateur pseudo-aléatoire compact (LCG 16 bits) pour le "laid".
static uint16_t rndState = 0xACE1;
static uint8_t rnd8() { rndState = rndState * 25173u + 13849u; return rndState >> 8; }

static inline void motor(uint8_t level) { analogWrite(MOTOR_PIN, level); }

// "Vibre juste" : trois pulsations propres, égales, avec montée/descente douces.
void feedbackTrue() {
  for (uint8_t i = 0; i < 3; i++) {
    for (int v = 0;   v <= 255; v += 15) { motor((uint8_t)v); delay(6); } // montée
    delay(120);
    for (int v = 255; v >= 0;   v -= 15) { motor((uint8_t)v); delay(6); } // descente
    delay(90);
  }
  motor(0);
}

// "Bourdonnement laid" : ~900 ms de vibration hachée, intensité et rythme irréguliers.
void feedbackFalse() {
  unsigned long t0 = millis();
  while (millis() - t0 < 900) {
    motor(60 + (rnd8() % 195));          // niveau aléatoire moyen->fort
    delay(6 + (rnd8() % 22));            // on : durée courte irrégulière
    motor(0);
    delay(3 + (rnd8() % 15));            // off : silence court irrégulier
  }
  motor(0);
}

void playFeedback(Verdict v) {
  if (v == V_TRUE)  feedbackTrue();
  else if (v == V_FALSE) feedbackFalse();
}

// ------------------------------------------------------------------
// SOURCE DU VERDICT
// ------------------------------------------------------------------
#if DIAPASON_TEST
// --- Mode test : bouton sur P0. Appui court = true, appui long = false. ---
Verdict pollVerdict() {
  if (digitalRead(TEST_BTN_PIN) == LOW) {
    unsigned long t = millis();
    while (digitalRead(TEST_BTN_PIN) == LOW && millis() - t < 2000) { /* attend le relâchement */ }
    unsigned long held = millis() - t;
    delay(120); // anti-rebond au relâchement
    return (held < 600) ? V_TRUE : V_FALSE;
  }
  return V_NONE;
}
#else
// --- INTÉGRATION RFID (à compléter selon le module retenu) ---
// Objectif : lire le bloc/texte de la carte présentée et renvoyer :
//   V_TRUE  si le contenu == "true"
//   V_FALSE si le contenu == "false"
//   V_NONE  si aucune carte / illisible
//
// Squelette (remplacer par les appels réels du lecteur, ex. RC522 via USI ou
// PN532 I2C). Garder cette fonction NON bloquante et courte.
Verdict pollVerdict() {
  // TODO: initialiser le lecteur dans setup(), interroger ici la présence d'une
  //       carte, lire les données, comparer à "true"/"false".
  // char buf[8]; if (!rfidRead(buf, sizeof(buf))) return V_NONE;
  // if (!strcmp(buf, "true"))  return V_TRUE;
  // if (!strcmp(buf, "false")) return V_FALSE;
  return V_NONE;
}
#endif

void setup() {
  pinMode(MOTOR_PIN, OUTPUT);
  motor(0);
#if DIAPASON_TEST
  pinMode(TEST_BTN_PIN, INPUT_PULLUP);
#else
  // TODO: rfidBegin();  // init du lecteur RFID
#endif
  // Petit "bip" tactile au démarrage pour confirmer que le prop est vivant.
  motor(180); delay(60); motor(0);
}

void loop() {
  Verdict v = pollVerdict();
  if (v != V_NONE) {
    playFeedback(v);
    delay(150); // évite de rejouer immédiatement sur la même carte/appui
  }
}
