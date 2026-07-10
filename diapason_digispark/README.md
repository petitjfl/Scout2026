# Diapason des Sentiers (Digispark Pro ATtiny167)

Prop **autonome, hors réseau** (pas d'ESP-NOW). Il « vibre juste » près d'un
vrai indice et « bourdonne laid » sur un leurre : il lit une carte RFID cachée
dans l'indice (texte `true` / `false`) et répond par le moteur de vibration.
Backup : télécommande IR (NEC), prioritaire, codes appris en EEPROM.

- `true`  → vibration propre : trois pulsations douces.
- `false` → bourdonnement haché, dissonant (~0,9 s).

## Cablage Digispark Pro (ATtiny167)

| Broche Digispark Pro | Rôle | Connexion |
|---|---|---|
| D10 (MOSI) | SPI MOSI | RC522 SDA/DI/MOSI |
| D8 (MISO) | SPI MISO | RC522 SO/MISO |
| D11 (SCK) | SPI clock | RC522 SCK |
| D12 (SS) | SPI chip select | RC522 NSS/SDA/SS |
| D2 | Entree IR | Sortie du recepteur IR (TSOP38238 / VS1838B, 38 kHz) |
| D1 (PWM) | Pilotage moteur | Base/gate du transistor moteur (avec diode de roue libre) |
| 3V3 | Alimentation RC522 | RC522 VCC |
| GND | Masse commune | RC522 GND, recepteur IR GND, moteur GND |
| VCC | Alimentation recepteur IR | selon module IR (souvent 5V) |
| - | Reset RC522 | RC522 RST -> VCC (reset logiciel) |

RC522 alimente en **3V3** (pas 5 V sur VCC). Moteur jamais en direct sur une
broche du microcontroleur.

### Schema rapide (texte)

```text
Digispark Pro (ATtiny167)                      RC522
-------------------------                      -----
D10 (MOSI)  ---------------------------------> MOSI / SDA / DI
D8  (MISO)  <--------------------------------- MISO / SO
D11 (SCK)   ---------------------------------> SCK
D12 (SS)    ---------------------------------> NSS / SDA / SS
3V3         ---------------------------------> VCC
GND         ---------------------------------> GND
RST RC522   ---------------------------------> VCC (strap)

Digispark Pro (ATtiny167)                      Recepteur IR (TSOP/VS1838B)
-------------------------                      ----------------------------
D2          <--------------------------------- OUT
VCC         ---------------------------------> VCC
GND         ---------------------------------> GND

Digispark Pro (ATtiny167)                      Moteur vibreur
-------------------------                      --------------
D1 (PWM)    ---> R base/gate ---> Transistor ---> Moteur ---> +V
GND         ------------------------------------------^------- GND commun
                                   diode de roue libre en parallele moteur
```

## Build & flash

```bash
pio run -t upload    # micronucleus : brancher le Digispark Pro quand l'outil le demande
```

## Programmer la carte (pas a pas)

### Methode simple (PlatformIO)

1. Ouvrir ce dossier dans VS Code.
2. Connecter le Digispark Pro au PC uniquement quand demande (fenetre courte).
3. Lancer la commande:

```bash
pio run -t upload
```

4. Quand PlatformIO affiche un message du type "Please plug in the device", brancher la carte.
5. Attendre la fin de flash.

### Si l'upload ne part pas (Windows)

1. Utiliser un cable USB data (pas charge seule).
2. Essayer un autre port USB (eviter hub non alimente).
3. Rebrancher juste apres le message de demande de branchement.
4. Installer/reinstaller le driver Digistump micronucleus si besoin.
5. Fermer les applications qui peuvent monopoliser l'USB.

### Cycle de dev recommande

1. Modifier le code.
2. Compiler:

```bash
pio run
```

3. Flasher:

```bash
pio run -t upload
```

4. Tester avec une carte RFID true/false et verifier aussi le backup IR.

## Utilisation

- **Démarrage** : bref « bip » moteur, puis diagnostic LED — 2 clignotements
  lents = RC522 détecté ; 6 rapides = absent (on compte sur l'IR).
- **Apprentissage IR**:
  - Au premier démarrage (EEPROM vierge), apprentissage automatique.
  - En re-apprentissage, il faut maintenant **2 trames NEC valides** dans les
    2,5 s après mise sous tension (evite les declenchements accidentels).
  - LED lente -> presser la touche "vrai" ; LED rapide -> presser une touche
    differente pour "faux".
  - Timeout de 15 s par etape: si timeout, les anciens codes EEPROM sont
    conserves (pas de blocage infini).

## État / limites

- ⚠️ Le pilote **RC522 (SPI logiciel) n'est pas testé sur matériel** — à
  valider au banc avant le camp. L'IR est le filet de secours fiable.
- Cartes **NTAG / Ultralight** (texte en page 4) supportées ; MIFARE Classic
  nécessiterait une authentification (non gérée).
- Flags `USE_RC522` / `USE_IR` en tête de `src/main.cpp`.
