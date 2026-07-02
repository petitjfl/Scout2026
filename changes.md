# Journal des modifications

Date : 2026-06-30

Ce document résume tous les changements apportés au projet (master ESP32, balises,
lanterne, interface web) ainsi que les conventions à respecter.

---

## 1. Master ESP32 (`master_esp32/src/main.cpp`)

### 1.1 Stabilité du WiFi (le téléphone décrochait)

- **Power-save WiFi désactivé** (`esp_wifi_set_ps(WIFI_PS_NONE)` + `WiFi.setSleep(false)`
  au démarrage). Par défaut l'ESP32 met la radio en modem-sleep : le point d'accès
  ratait des beacons et le téléphone se déconnectait (alors qu'ESP-NOW continuait).
- **Watchdog AP** : ré-affirme `WIFI_PS_NONE` toutes les 10 s, et le ré-applique
  après tout redémarrage du softAP (qui peut réactiver le power-save).

### 1.2 Plantage lors d'envois en rafale (« force all »)

- **Suppression des `delay(UNICAST_GAP_MS)` dans le callback WebSocket.** Ils
  bloquaient la pile réseau jusqu'à ~2,4 s par commande groupée → le téléphone
  décrochait. De plus ils n'espaçaient rien (l'envoi radio est différé dans la file).
- **Espacement déplacé dans `processPendingQueue()`** : un seul `esp_now_send` par
  fenêtre de `UNICAST_GAP_MS`, sans jamais bloquer le réseau. La file est désormais
  traitée à chaque tour de `loop()` (auto-cadencée en interne).

### 1.3 Crash sous charge (bug de concurrence — cause profonde)

- **File de réception ESP-NOW (SPSC ring).** Le callback `onDataRecv` s'exécute dans
  la tâche WiFi et appelait directement `wsBroadcast()` (WebSocket non thread-safe)
  et `saveConfig()` (écriture flash) → corruption mémoire / crash quand les balises
  répondaient en rafale (ACK).
- Désormais le callback **copie seulement** le paquet brut dans une file ; tout le
  traitement (parsing HB/ACK, état, flash, WebSocket) se fait dans `loop()` via
  `drainRxQueue()` → `processRxMessage()`. Débordement de file compté et loggé.

### 1.4 Correctifs de robustesse / nettoyage

- `handleRoot()` renvoie **404** si `index.html` est absent (au lieu de servir un
  fichier invalide).
- `RETRIES` **pilote réellement** le nombre max de tentatives ; `ACK_TIMEOUT_MS`
  (inutilisé dans ce design) supprimé.
- Code mort supprimé : `HB_BROADCAST_MIN_MS`, `Accessory::lastBroadcastMs`,
  fonction `markPendingSuccess()`.

### 1.5 Fiabilité des commandes en production (fenêtre de réveil)

- **Push de commande à la réception du HB.** Une balise en deep-sleep n'écoute que
  ~2 s après avoir émis son heartbeat. Le master pousse maintenant la commande en
  attente **dès qu'il reçoit le HB** de la balise, au lieu d'attendre son cycle de
  retry « à l'aveugle ». Passe d'un ~3 % de chance de livraison à quasi 100 %.

---

## 2. Balises (`balise_esp8266/balise_esp8266/src/main.cpp`)

- **`saveState()` (écriture EEPROM/flash) déférée hors du callback ESP-NOW.** Sur
  ESP8266 le callback tourne en contexte SYS ; écrire la flash là provoque des
  crashs (« Soft WDT reset »). Les commandes `FORCE_IDLE`, `GLITCH_LOCK`, `TIME`
  posent désormais un flag `stateDirty` ; l'écriture se fait dans `loop()`.
- **Lecture de trame ESP-NOW bornée** par `len` (plus de dépendance à un
  terminateur null fourni par l'émetteur).

### Points non corrigés (intentionnel ou à vérifier)

- `forcedMode` qui empêche le deep-sleep tant qu'on n'envoie pas `SETMODE` :
  **laissé tel quel, c'est voulu** (utile pour la finale, mais consomme la batterie).
- Jour/nuit calculé en `gmtime` (UTC) : à vérifier que le master envoie bien un epoch
  en **heure locale** (sinon décalage de quelques heures sur l'allumage).
- Rappel matériel : le deep-sleep exige D0 (GPIO16) relié à RST sur le D1 mini.

---

## 3. Lanterne (`lanterne_esp8266/` — NOUVEAU)

Création du firmware et de la config (`platformio.ini` + `src/lanterne.cpp`).

- Même matériel qu'une balise (D1 mini + anneau NeoPixel) mais **ne suit pas le cycle
  bleu jour/nuit** et **ne dort pas** : reste allumée et à l'écoute en continu →
  commandes fiables.
- **Modes** : `OFF`, `CANDLE` (bougie chaude, défaut), `ALERT` (pulsation rouge),
  `WHITE` (blanc chaud brillant — jamais bleu).
- **Persistance EEPROM** du mode (reprend le dernier mode après une coupure).
- Reprend les correctifs des balises : copie RX bornée, `saveState()` déféré.
- **Alias finale** : la lanterne interprète `FORCE_GLITCH` et `FINAL` comme `ALERT`,
  pour qu'un seul « glitch all » / FINALE déclenche tous les appareils d'un coup.
- Commandes acceptées : `FORCE_OFF`, `FORCE_CANDLE`, `FORCE_ALERT`, `FORCE_WHITE`,
  `FORCE_GLITCH`/`FINAL` (→ alerte), `SETMODE <0..3>`.

---

## 4. Interface web (`master_esp32/data/`)

### 4.1 Deux vues

- **Vue Prod (par défaut)** : épurée, gros boutons groupés par section (Lanterne /
  Balises / Médaillon), compteur « X/Y en ligne » par section, et un bouton
  **🎆 FINALE** déclenchant le glitch général d'un seul tap.
- **Vue Détaillé (dev)** : l'ancienne interface complète (tableau, scan, force-all,
  barre lanternes, journal) — **conservée**, accessible via la bascule en haut.

### 4.2 Mode Discret 🕶️

- Plein écran sombre **mais lisible** : bouton FINALE + 3 sections (Lanterne /
  Balises / Médaillon) avec boutons étiquetés, et bouton « Fermer ».
- Conçu pour piloter discrètement depuis le téléphone sans que les jeunes le
  remarquent, tout en gardant le contrôle des balises et du médaillon.

### 4.3 Détection par type et commandes

- Détection automatique du type par plage d'ID (voir §5), boutons adaptés :
  - Lanterne : Bougie / Blanc / Alerte / Éteindre
  - Balises : Normal / Glitch / Éteindre
  - Médaillon : Déclencher / Arrêter *(provisoire — firmware à venir)*
- Colonne « Mode » affichée en clair (Bougie, Alerte, Glitch, Effet…) au lieu d'un
  chiffre.
- **FINALE** : envoie `FORCE_GLITCH` aux balises + lanterne (alias → alerte) et
  `TRIGGER` au médaillon, simultanément.

---

## 5. Conventions de plages d'ID

À respecter dans les firmwares (`DEVICE_ID`) et utilisé par `kindForId()` côté UI :

| Type      | Plage d'ID |
|-----------|-----------|
| Balises   | 1 – 9     |
| Lanternes | 10 – 19   |
| Médaillons| 20 – 29   |

---

## 6. Outils / dépôt

- **`.gitignore`** créé à la racine : ignore `.pio/`, `node_modules/`, artefacts de
  build (`*.bin`, `*.elf`, …) et le bruit `.vscode`. (`.pio` n'était pas tracké.)
- **Environnement PlatformIO** : module Python `intelhex` installé (il manquait et
  faisait échouer la génération du bootloader esptool).
- **Serveur de test** (`scratchpad/mockserver/`) : mock du master (HTTP + WebSocket)
  servant l'interface réelle avec de fausses devices (4 balises, 1 lanterne, 1
  médaillon) pour tester l'UI sans matériel.

---

## 7. Déploiement

```bash
# Interface web (après modification de data/)
pio run -d master_esp32 -t uploadfs

# Firmware master
pio run -d master_esp32 -t upload

# Firmware balise — un environnement par balise, identité injectée par build flag
pio run -d balise_esp8266 -e balise_nord -t upload   # idem _sud / _est / _ouest

# Firmware lanterne / médaillon
pio run -d lanterne_esp8266  -e lanterne_1  -t upload
pio run -d medaillon_esp8266 -e medaillon_1 -t upload
```

Sous Windows, si `pio` n'est pas dans le PATH :
`& "C:\Users\valse\.platformio\penv\Scripts\pio.exe" ...`

---

## 8. Finale — modes, effets et scènes (2026-07-01)

D'après le déroulé de la finale (4 phases cuées manuellement).

### 8.1 Nouveaux modes firmware

- **Balise** (`3..7`) : `AMBER` (ambre fixe), `BLUE` (bleu stable),
  `ALERT` (ambre clignotant), `RAINBOW` (arc-en-ciel pulsant),
  `BLUE_SLOW` (bleu profond, respiration très lente).
  Commandes : `FORCE_AMBER`, `FORCE_BLUE`, `FORCE_ALERT`, `FORCE_RAINBOW`,
  `FORCE_BLUE_SLOW` (toutes en mode forcé → balise éveillée).
- **Lanterne** (`4..5`) : `REVELATION` (ombres dansantes),
  `WINDIGO` (fondu lent vers le noir puis clignote rouge).
  Commandes : `FORCE_REVELATION`, `FORCE_WINDIGO`.
- **Médaillon** (`medaillon_esp8266/` — NOUVEAU firmware) : D1 mini + anneau
  NeoPixel. Effet `TRIGGER` = comète/dragon chassant sa queue qui change de
  couleur, montée vers un sommet doré/blanc brillant ; `STOP` = repos.

### 8.2 Interface — panneau « Scènes de finale »

Un bouton par cue (dans la vue Prod, repliable, et dans le mode Discret) :

| Scène | Envois |
|-------|--------|
| Prépa | balises `FORCE_AMBER` (réveil), lanterne `FORCE_CANDLE`, médaillon `STOP` |
| Installation | balises `FORCE_BLUE`, lanterne `FORCE_REVELATION` |
| Attaque | lanterne `FORCE_WINDIGO` + alertes balises (séquence / individuel) |
| Cloche | balises `FORCE_BLUE` |
| Accord final | médaillon `TRIGGER`, balises `FORCE_RAINBOW`, lanterne `FORCE_WHITE` |
| Fin | balises `FORCE_BLUE_SLOW`, lanterne `FORCE_CANDLE`, médaillon `STOP` |

- **Alerte balises « une par une »** : bouton « Séquence auto » (intervalle fixe)
  **et** un bouton par balise (tap individuel).
- Les modes sont affichés en clair dans la vue Détaillé (Ambre, Arc-en-ciel,
  Windigo, Illumination…).

### 8.3 Modèle d'alimentation des balises (révisé)

- **Nuit** (~20h35 → 5h30) : la balise reste **éveillée** et allumée **faiblement**
  (bleu pulsant, `NIGHT_BRIGHTNESS = 45`) pour « veiller » sur les tentes, et
  répondre en temps réel (la finale se joue le soir). Plus de fenêtre de 2 s.
- **Jour** : **deep-sleep** (économie batterie, les balises ne servent pas).
- **Mode forcé (finale)** : jamais de sleep, quelle que soit l'heure.
- Batterie : ~24 h sur une charge (recharge quotidienne). Sans heure synchronisée,
  la balise suppose la **nuit** (reste allumée plutôt que de dormir au mauvais moment).

### 8.4 Effet « Recharge » (soir 5)

Nouveau mode balise `RECHARGE (8)` / commande `FORCE_RECHARGE` : flash blanc bref
(« rechargée par la Cloche ») puis **retour automatique au bleu stable**. Exposé
dans la vue Prod (section Balises → bouton « Recharge »).

### 8.5 Décisions logistiques (finale)

- **Audio** = manuel, géré par les aides de camp (pas piloté par le master).
- **UV** : le Miroir de Brume **est** UV (pas de lampe séparée à coder).
- **Diapason** = capteur RFID autonome (cartes true/false) + micro-vibreur — **hors
  réseau** (rien côté master), mais **firmware dédié** créé : voir §8.6.
- **Corde des Liens** = fil lumineux **manuel**, hors réseau.
- **Pas de bouton de sécurité « Lumière »** dans l'UI : les commandos se déroulent
  **hors du camp**, sans accès au master.

### 8.6 Diapason des Sentiers (`diapason_digispark/` — NOUVEAU)

Prop **autonome** (pas d'ESP-NOW) sur **Digispark (ATtiny85)**. Lit des cartes RFID
« true/false » cachées dans les indices et répond par le **moteur de vibration** :
- « true » → **vibration propre** (trois pulsations douces, « vibre juste »),
- « false » → **bourdonnement laid** (vibration hachée, dissonante).

Deux sources de verdict, **l'IR prioritaire** :
1. **RFID RC522 en SPI** (SPI **logiciel** maison, pilote RC522 minimal) : détecte
   la carte, la sélectionne, lit une page NTAG/Ultralight et cherche « true »/« false ».
2. **Override télécommande IR (NEC)** : BACKUP si le RFID ne marche pas, prioritaire.

État : **compile** (Flash 47 %, RAM 5 %). La partie **RC522 est non testée sur
matériel** (SPI logiciel + partage de broche = délicat, à valider au banc) ; l'IR
est le filet fiable. Cartes **NTAG/Ultralight** (texte NDEF) supportées ; **MIFARE
Classic** nécessiterait une authentification (non gérée). Flags `USE_RC522` / `USE_IR`.

#### Câblage (Digispark ATtiny85 — P5 = reset, à éviter)

Le RC522 (SPI) prend 4 broches. Pour garder l'IR **sur l'ATtiny85**, la sortie du
récepteur IR est **partagée avec MISO** (deux entrées : le RC522 met MISO en haute
impédance quand SS est haut) via une **résistance série de 1 kΩ**. Le moteur garde
sa broche.

| Broche | Rôle | Détail |
|--------|------|--------|
| **P0** | **MOSI** → RC522 | |
| **P4** | **SCK** → RC522 | |
| **P2** | **MISO** ← RC522 **+ récepteur IR via 1 kΩ** | TSOP38238 / VS1838B (38 kHz) |
| **P3** | **SS/NSS** → RC522 | |
| **P1** | **moteur** (transistor + diode) + LED interne | la LED montre la vibration |
| RST RC522 | → **VCC** | reset logiciel par commande |
| P5 | (reset) | ne pas utiliser |

- **RC522** : alim **3V3** (⚠️ pas 5V sur le VCC du module), niveaux logiques 5V
  tolérés sur les entrées. RST relié à VCC.
- **Récepteur IR** : OUT → **1 kΩ** → P2 ; alim 3V3/5V, GND commun.
- **Moteur** : jamais direct sur la broche → transistor commandé par P1 (+ diode).
- **Pourquoi pas partager IR avec le moteur** : sortie push-pull de l'IR = contention
  avec la sortie MCU, et ligne IR haute au repos = moteur activé en permanence. Le
  partage propre est IR+MISO (deux entrées).

#### Télécommande IR — apprentissage (pas de code à connaître)

Les codes NEC sont **appris et mémorisés en EEPROM** (les mini-télécommandes
identiques ont parfois des codes différents, donc pas de valeurs en dur) :
- **1er démarrage** (EEPROM vierge) → mode apprentissage automatique.
- **Ré-apprendre plus tard** : presser **n'importe quelle touche dans les 2,5 s**
  après la mise sous tension.
- Déroulé : la LED clignote **lentement** → presse ta touche « **vrai** » (vibration
  propre de confirmation), puis clignote **vite** → presse ta touche « **faux** »
  (bourdonnement de confirmation). C'est mémorisé.

#### Diagnostic au démarrage (LED interne)

À la mise sous tension, après le « bip » du moteur, la LED (sur P1 = moteur) indique
l'état du RC522 :
- **2 clignotements lents** = RC522 détecté (lecture du registre Version OK).
- **6 clignotements rapides** = RC522 absent/non répondant → on compte sur l'IR.

⚠️ **Réserve LED** : la LED interne est sur **P1** sur la plupart des Digispark (ce
qu'on utilise, avec le moteur). Sur certains clones elle est sur **P0** = MOSI ici :
le diagnostic clignoterait de façon incohérente, sans conséquence (RFID/IR marchent
quand même).

---

## 9. Reste à faire

- **Diapason** : valider le pilote **RC522** (SPI logiciel + partage MISO/IR) sur
  matériel — c'est la partie non testée. L'IR est déjà autonome (apprentissage EEPROM).
  Confirmer le type de carte : NTAG/Ultralight OK ; MIFARE Classic = auth à ajouter.
- **Médaillon** : `NUM_LEDS` réglé à 12 par défaut — ajuster au vrai anneau.
- Affiner les couleurs/effets sur le vrai matériel (bougie, arc-en-ciel, comète…).
- Flasher les 4 balises, la lanterne, le médaillon, et pousser l'UI (`uploadfs`).
- Avant le camp : `DEBUG_MODE=0` dans les `platformio.ini` (balise/lanterne/médaillon).

---

## 10. Passe de professionnalisation (2026-07-01)

### 10.1 Identité des appareils par build flags

Fini l'édition du code avant chaque flash : `DEVICE_ID`, `DEVICE_NAME` et
`DEBUG_MODE` sont injectés par `platformio.ini`, avec **un environnement par
appareil physique** (`balise_nord|sud|est|ouest`, `lanterne_1`, `medaillon_1`).
Les sources gardent des fallbacks `#ifndef`. `DEBUG_MODE` se bascule à un seul
endroit (section `[env]`) pour le passage en production.

### 10.2 Documentation

- **`README.md`** (racine) : vue d'ensemble, inventaire des appareils,
  architecture, commandes de build/flash, opération au camp, checklist.
- **`PROTOCOL.md`** (racine, nouvelle référence unique) : trames ESP-NOW,
  commandes et modes par type d'appareil, plages d'ID, modèle de fiabilité,
  sémantique `TIME`, messages WebSocket UI ↔ master.
- **`storyline.md`** (racine) : guide de l'opérateur — cue par cue, soir par
  soir, quelle action dans l'interface et à quel moment de la légende.
- README par sous-projet (câblage, environnements de build, comportement).

### 10.3 Correctifs

- **`TIME` en heure locale** (bug §2 clos) : l'UI envoie désormais un epoch
  décalé du fuseau du téléphone ; le `gmtime()` des balises rend donc l'heure
  murale locale. Plus de décalage du cycle jour/nuit.
- **Lanterne** : `SETMODE` acceptait seulement 0–3 ; borne corrigée à
  `MODE_MAX` (REVELATION et WINDIGO accessibles aussi par SETMODE).
- **Master** : `saveConfig()` écrivait `role="balise"` pour tous les
  accessoires ; le rôle est maintenant déduit de la plage d'ID (`roleForId()`).
- **Master** : migration **ArduinoJson v7** (`JsonDocument`, `to<JsonArray>()`,
  `add<JsonObject>()`) — plus d'API v6 dépréciée ; lecture de `config.json`
  robuste aux champs absents (`| ""`), erreur JSON loggée.
- `platformio.ini` du master : `Upload_port` → `upload_port` (la clé mal
  orthographiée était ignorée par PlatformIO).

### 10.4 Nettoyage

- Suppression du dossier résiduel `balise_esp8266/balise_esp8266/` (reliquat
  du déplacement d'un niveau) et des README boilerplate PlatformIO.
- `medaillon.cpp` → `main.cpp` (convention commune aux projets).
- Balise : suppression du code mort (`HB_DAY_MS`, `HB_NIGHT_MS`, `dbg()`,
  double affectation de `HEARTBEAT_INTERVAL_MS` — désormais constante).
- Constantes nommées côté master (`WIFI_CHANNEL`, `MAX_ACCESSORIES`) au lieu
  des littéraux 9 et 8 répétés.
- `config.json` : suppression de la clé morte `ack_timeout_ms`.
