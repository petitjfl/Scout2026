# Diapason des Sentiers (Digispark Pro ATtiny167)

Prop **autonome, hors réseau** (pas d'ESP-NOW).

Mode actuel : **IR seul**. Le RC522 est désactivé dans le firmware pour isoler
le système télécommande/récepteur/moteur.

- `true`  → vibration propre : trois pulsations douces.
- `false` → bourdonnement haché, dissonant (~0,9 s).

## Cablage Digispark Pro (ATtiny167)

| Broche Digispark Pro | Rôle | Connexion |
|---|---|---|
| D2 | Entree IR | Sortie du recepteur IR (TSOP38238 / VS1838B, 38 kHz) |
| D1 | LED diagnostic | LED integree Digispark Pro |
| D0 (PWM) | Pilotage moteur | Base/gate du transistor moteur (avec diode de roue libre) |
| GND | Masse commune | recepteur IR GND, moteur GND |
| VCC | Alimentation recepteur IR | selon module IR (souvent 5V) |

Moteur jamais en direct sur une broche du microcontroleur.

### Schema rapide (texte)

```text
Digispark Pro (ATtiny167)                      Recepteur IR (TSOP/VS1838B)
-------------------------                      ----------------------------
D2          <--------------------------------- OUT
VCC         ---------------------------------> VCC
GND         ---------------------------------> GND

Digispark Pro (ATtiny167)                      Moteur vibreur
-------------------------                      --------------
D0 (PWM)    ---> R base/gate ---> Transistor ---> Moteur ---> +V
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

4. Apprendre deux boutons IR, puis tester la LED et le moteur.

## Utilisation

- **Démarrage** : 2 flashs LED = le firmware tourne et la LED D1 fonctionne.
- **Apprentissage** :
  - Si aucune touche n'est apprise, la LED clignote lentement : presser le
    bouton `true`.
  - Ensuite la LED clignote rapidement : presser un bouton different pour
    `false`.
  - Pour refaire l'apprentissage plus tard, allumer la carte et presser le
    nouveau bouton `true` pendant les 4 premieres secondes.
- **Boutons appris** :
  - `true` = 2 flashs LED lents, puis vibration agréable.
  - `false` = 6 flashs LED rapides, puis vibration tres désagréable.
- **Diagnostic IR** :
  - 1 flash très court = activité brute vue sur D2, mais pas une trame NEC
    valide.
  - 3 flashs rapides, puis 1 flash long + 8 bits = bouton NEC valide mais non
    appris. Les 8 bits affichent le code : court = 0, long = 1.
  - Aucun flash = le signal du recepteur IR n'arrive pas sur D2. Verifier
    VCC/GND/OUT, le sens du recepteur, la masse commune, et que OUT est bien sur
    D2.

## État / limites

- Flags `USE_RC522`, `USE_IR` et `STARTUP_RELEARN_MS` en tête de `src/main.cpp`.
