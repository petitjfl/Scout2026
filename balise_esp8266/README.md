# Balises de Veille (ESP8266)

Les quatre marqueurs du Cercle de Veille, plantés aux points cardinaux du
camp. Bleu pulsant faible la nuit (visible des tentes), deep-sleep le jour,
modes forcés pour les veillées et la finale. Protocole et liste complète des
commandes/modes : [PROTOCOL.md](../PROTOCOL.md).

## Matériel

- Wemos D1 mini lite + anneau NeoPixel 6 LEDs sur **D4**.
- Pont **D0 (GPIO16) → RST** requis pour le réveil du deep-sleep.
- Batterie sur **A0** via diviseur R1 = 100 kΩ / R2 = 220 kΩ (réf. ADC 3,2 V).

## Build & flash

Un environnement PlatformIO par balise physique (aucune édition de code) :

```bash
pio run -e balise_nord  -t upload    # id 1
pio run -e balise_sud   -t upload    # id 2
pio run -e balise_est   -t upload    # id 3
pio run -e balise_ouest -t upload    # id 4
```

Important : un `pio run` (ou Upload VS Code) **sans** `-e` utilise
`default_envs` dans `platformio.ini` (actuellement `balise_nord`) et ne lance
plus toutes les balises.

Dans VS Code, vous pouvez aussi utiliser les tâches :

- `PIO Build (choisir balise)`
- `PIO Upload (choisir balise)`

Elles affichent une liste (`balise_nord/sud/est/ouest`) avant d'exécuter.

`DEBUG_MODE=1` (défaut, section `[env]` de `platformio.ini`) : série active,
pas de deep-sleep. **Passer à `0` avant le camp.**

## Comportement

- **Nuit** (~20 h 35 → 5 h 30, ou sans heure synchronisée) : éveillée, veille
  bleue faible, heartbeat toutes les 5 s, réactive aux commandes.
- **Jour** : deep-sleep par cycles de 60 s ; à chaque réveil, un heartbeat puis
  2 s d'écoute (le master pousse alors toute commande en attente).
- **Mode forcé** (commandes `FORCE_*`) : plus jamais de sleep jusqu'à
  `FORCE_IDLE` — c'est voulu pour la finale, mais ça consomme la batterie.
- `GLITCH_LOCK` et l'heure synchronisée sont persistés en EEPROM.
