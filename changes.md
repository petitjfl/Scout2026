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

# Firmware balise (changer DEVICE_ID/DEVICE_NAME avant chaque flash)
pio run -d balise_esp8266/balise_esp8266 -t upload

# Firmware lanterne (DEVICE_ID dans la plage 10–19)
pio run -d lanterne_esp8266 -t upload
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

État : la **logique verdict→vibration est complète et compile** (Flash 19 %, RAM 2 %).
Démarre en **MODE TEST** (bouton sur P0 : appui court = true, appui long = false)
pour valider la vibration au banc. La **lecture RFID reste à intégrer** derrière
`pollVerdict()` (passer `DIAPASON_TEST` à 0).

Contraintes ATtiny85 (~6 Ko flash, pas de vrai SPI) : selon le module RFID final,
soit **PN532 en I2C** (2 broches, léger), soit **RC522** via un pilote USI maison
(RST relié à VCC). Si ça déborde, passer sur **Digispark Pro** (ATtiny167, 14 Ko).
Câblage : moteur sur P4 (via transistor + diode), RFID sur P0..P3.

---

## 9. Reste à faire

- **Diapason** : intégrer la lecture RFID réelle dans `pollVerdict()` (module + lib
  à confirmer : PN532 I2C recommandé), puis `DIAPASON_TEST = 0`.
- **Médaillon** : `NUM_LEDS` réglé à 12 par défaut — ajuster au vrai anneau.
- Vérifier l'heure locale envoyée par la commande `TIME` (cf. §2).
- Affiner les couleurs/effets sur le vrai matériel (bougie, arc-en-ciel, comète…).
- Flasher les 4 balises, la lanterne, le médaillon, et pousser l'UI (`uploadfs`).
