# Diapason des Sentiers (Digispark ATtiny85)

Prop **autonome, hors réseau** (pas d'ESP-NOW). Il « vibre juste » près d'un
vrai indice et « bourdonne laid » sur un leurre : il lit une carte RFID cachée
dans l'indice (texte `true` / `false`) et répond par le moteur de vibration.
Backup : télécommande IR (NEC), prioritaire, codes appris en EEPROM.

- `true`  → vibration propre : trois pulsations douces.
- `false` → bourdonnement haché, dissonant (~0,9 s).

## Câblage (P5 = reset, à éviter)

| Broche | Rôle | Détail |
|---|---|---|
| P0 | MOSI → RC522 | |
| P4 | SCK → RC522 | |
| P2 | MISO ← RC522 **+ sortie récepteur IR via 1 kΩ** | TSOP38238 / VS1838B (38 kHz) ; deux entrées, le RC522 met MISO en haute impédance quand SS est haut |
| P3 | SS/NSS → RC522 | |
| P1 | Moteur (transistor + diode) + LED interne | la LED montre la vibration |
| — | RST du RC522 → VCC | reset logiciel par commande |

RC522 alimenté en **3V3** (pas 5 V sur VCC). Moteur jamais en direct sur la
broche.

## Build & flash

```bash
pio run -t upload    # micronucleus : brancher le Digispark quand l'outil le demande
```

## Utilisation

- **Démarrage** : bref « bip » moteur, puis diagnostic LED — 2 clignotements
  lents = RC522 détecté ; 6 rapides = absent (on compte sur l'IR).
- **Apprentissage IR** (1er démarrage EEPROM vierge, ou n'importe quelle touche
  pressée dans les 2,5 s après la mise sous tension) : LED lente → presser la
  touche « vrai » (confirmation : vibration propre) ; LED rapide → presser la
  touche « faux » (confirmation : bourdonnement). Mémorisé en EEPROM.

## État / limites

- ⚠️ Le pilote **RC522 (SPI logiciel) n'est pas testé sur matériel** — à
  valider au banc avant le camp. L'IR est le filet de secours fiable.
- Cartes **NTAG / Ultralight** (texte en page 4) supportées ; MIFARE Classic
  nécessiterait une authentification (non gérée).
- Flags `USE_RC522` / `USE_IR` en tête de `src/main.cpp`.
