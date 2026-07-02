# Scout2026 — La Symphonie des Échos Noirs

Artefacts électroniques du camp d'été 2026 de la **Meute 6A St-Paul d'Aylmer**.

Dans la légende du camp, les louveteaux — les *Raviveurs d'Accords* — retrouvent
les sept artefacts du Premier Orchestre pour rejouer la Symphonie avant que le
Windigo des Échos Noirs ne transforme tout en dissonance. Ce dépôt contient le
firmware et l'interface de contrôle des artefacts électroniques qui donnent vie
à cette histoire : le Cercle de Veille qui protège les tentes, la lanterne qui
révèle, le médaillon qui conclut, le diapason qui distingue le vrai du faux.

## Les appareils

| Dossier | Artefact | Matériel | Rôle |
|---|---|---|---|
| [`master_esp32/`](master_esp32/) | La Baguette (console de régie) | ESP32 DevKit v1 | Point d'accès WiFi + interface web + coordination ESP-NOW de tous les accessoires |
| [`balise_esp8266/`](balise_esp8266/) | Balises de Veille (×4) | D1 mini + anneau NeoPixel 6 LEDs | Cercle de Veille : bleu pulsant la nuit, deep-sleep le jour, effets pilotés (glitch, finale) |
| [`lanterne_esp8266/`](lanterne_esp8266/) | Lanterne des Portées | D1 mini + anneau NeoPixel 6 LEDs | Source de lumière : bougie, blanc chaud, révélation, Windigo |
| [`medaillon_esp8266/`](medaillon_esp8266/) | Médaillon du Dernier Accord | D1 mini + anneau NeoPixel 12 LEDs | Effet signature de la finale : comète montant vers un sommet doré |
| [`diapason_digispark/`](diapason_digispark/) | Diapason des Sentiers | Digispark ATtiny85 + RC522 + vibreur | **Autonome, hors réseau** : lit des cartes RFID « true/false », vibre juste ou bourdonne |

Les autres artefacts (Miroir de Brume, Corde des Liens, Cloche du Rappel) sont
des props manuels, sans électronique pilotée.

## Architecture

```
téléphone de l'animateur
     │  WiFi « MASTER_AP » (canal 9)
     ▼
┌──────────────────────────────┐
│ master ESP32                 │  http://192.168.4.1 (interface web, LittleFS)
│ HTTP :80 · WebSocket :81     │  vues Prod / Détaillé / Discret + scènes de finale
└──────────────┬───────────────┘
               │  ESP-NOW (canal 9)  HB / CMD / ACK
   ┌───────────┼───────────────┬─────────────┐
   ▼           ▼               ▼             ▼
 balises 1–4  lanterne 10   médaillon 20   (diapason : autonome, hors réseau)
```

Le protocole complet (formats de trame, commandes, modes, modèle de
fiabilité, WebSocket) est documenté dans **[PROTOCOL.md](PROTOCOL.md)** —
c'est la référence unique à maintenir.

## Prise en main

Prérequis : [PlatformIO CLI](https://platformio.org/) (`pio`). Sous Windows,
si `pio` n'est pas dans le PATH :
`& "C:\Users\valse\.platformio\penv\Scripts\pio.exe" ...`

```bash
# Master : firmware puis interface web (contenu de data/)
pio run -d master_esp32 -t upload
pio run -d master_esp32 -t uploadfs

# Balises : un environnement par balise physique — AUCUNE édition de code
pio run -d balise_esp8266 -e balise_nord  -t upload
pio run -d balise_esp8266 -e balise_sud   -t upload
pio run -d balise_esp8266 -e balise_est   -t upload
pio run -d balise_esp8266 -e balise_ouest -t upload

# Lanterne et médaillon
pio run -d lanterne_esp8266  -e lanterne_1  -t upload
pio run -d medaillon_esp8266 -e medaillon_1 -t upload

# Diapason (upload micronucleus : brancher le Digispark quand l'outil le demande)
pio run -d diapason_digispark -t upload
```

L'identité de chaque appareil (`DEVICE_ID`, `DEVICE_NAME`) est injectée par
l'environnement PlatformIO choisi (voir le `platformio.ini` de chaque projet).
Plages d'ID : balises 1–9, lanternes 10–19, médaillons 20–29.

## Opération au camp

1. Alimenter le master ; se connecter au WiFi `MASTER_AP` (mot de passe
   `masterpass`) ; ouvrir `http://192.168.4.1`.
2. **Vue Prod** (défaut) : gros boutons par groupe d'appareils, bouton FINALE,
   panneau « Scènes de finale » (une scène = un cue de la Nuit du Grand
   Rappel). **Vue Détaillé** : tableau complet, journal, commandes unitaires.
   **Mode Discret** : plein écran sombre pour piloter sans être remarqué.
3. Presser **Time (sync)** (vue Détaillé) après chaque mise en route du master :
   les balises en ont besoin pour leur cycle jour/nuit (sans heure, elles
   supposent la nuit et restent allumées).
4. Les balises endormies (le jour) ne reçoivent une commande qu'à leur réveil
   (≤ 60 s) : le badge « en attente » de l'interface l'indique.

## Checklist avant le camp

- [ ] Passer `DEBUG_MODE` à `0` dans le `platformio.ini` des balises, lanterne
      et médaillon, puis reflasher (série coupée, deep-sleep de jour actif).
- [ ] Vérifier le pont **D0 (GPIO16) → RST** sur chaque balise (requis pour le
      réveil du deep-sleep).
- [ ] Reflasher l'interface web (`uploadfs`) si `data/` a changé.
- [ ] Valider le lecteur **RC522 du diapason au banc** (SPI logiciel non testé
      sur matériel — l'IR appris en EEPROM est le filet de secours).
- [ ] Ajuster `NUM_LEDS` du médaillon au vrai anneau (12 par défaut).
- [ ] Charger les batteries ; tester une soirée complète de scènes de finale.

## Documents

- [storyline.md](storyline.md) — guide de l'opérateur : quel bouton presser,
  à quel moment de la légende, soir par soir.
- [PROTOCOL.md](PROTOCOL.md) — protocole ESP-NOW + WebSocket (référence).
- [changes.md](changes.md) — journal des modifications et décisions.
- README de chaque sous-projet — câblage et spécificités matérielles.
