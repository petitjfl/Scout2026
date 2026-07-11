# Protocole réseau — Symphonie des Échos Noirs

Source de vérité du protocole entre le **master ESP32** et les accessoires
(balises, lanternes, médaillons). Toute modification ici doit être répercutée
dans les firmwares et dans `master_esp32/data/app.js`.

## 1. Couche radio

- **ESP-NOW**, canal WiFi **9** (imposé partout : `WIFI_CHANNEL`).
- Le master est aussi un point d'accès WiFi (`MASTER_AP` / `masterpass`,
  même canal 9 — softAP et ESP-NOW partagent la radio).
- Trames : texte ASCII, champs séparés par `|`, terminées par un NUL,
  max 250 octets (limite ESP-NOW). Les récepteurs ne font **pas** confiance au
  NUL de l'émetteur (copie bornée par `len`).
- Heartbeats en **broadcast** (`FF:FF:FF:FF:FF:FF`) ; commandes en **unicast**
  master → accessoire ; ACK en unicast accessoire → master.

## 2. Messages

| Message | Direction | Format |
|---|---|---|
| Heartbeat | accessoire → tous | `HB\|<seq>\|<id>\|<mode>\|<batt_mv>\|<batt_pct>` |
| Commande | master → accessoire | `CMD\|<NOM>[\|<arg>]` |
| Accusé | accessoire → master | `ACK\|<id>\|<NOM>\|<status>` (`OK` / `ERR`) |

- `seq` : compteur monotone par appareil (diagnostic de pertes).
- `batt_mv` : tension batterie en millivolts. `batt_pct` : charge 0–100 %
  interpolée sur la courbe de décharge mesurée. Champ **optionnel** ajouté après
  coup : un firmware plus ancien envoie 4 champs, le master lit alors `pct = -1`.
- Un préfixe d'adressage optionnel `TO\|<id>\|` devant la commande est encore
  accepté par tous les accessoires (compatibilité) ; le master n'en émet plus,
  l'unicast suffit.
- L'ACK reprend **le nom de commande reçu** (les alias inclus), afin que le
  master retrouve la commande en attente correspondante.

## 3. Plages d'ID (`DEVICE_ID`)

| Type | Plage | Environnements PlatformIO |
|---|---|---|
| Balises de Veille | 1 – 9 | `balise_nord` (1), `balise_sud` (2), `balise_est` (3), `balise_ouest` (4) |
| Lanternes des Portées | 10 – 19 | `lanterne_1` (10) |
| Médaillons du Dernier Accord | 20 – 29 | `medaillon_1` (20) |

L'interface web déduit le type d'un appareil de son ID (`kindForId()` dans
`app.js`) ; le master en déduit le champ `role` persisté dans `config.json`.

## 4. Commandes et modes par type d'appareil

### 4.1 Balise (`balise_esp8266`)

| Commande | Effet |
|---|---|
| `SETMODE <0..8>` | change de mode sans le forcer (le cycle jour/nuit reprend la main) |
| `FORCE_IDLE` | veille bleue + **déverrouille** `GLITCH_LOCK` + rend le contrôle au cycle jour/nuit |
| `FORCE_OFF` | éteint (forcé) |
| `FORCE_GLITCH` | glitch (forcé) |
| `GLITCH_LOCK <0/1>` | verrouille le mode glitch (ne se résorbe pas quand le groupe est réuni) |
| `FORCE_AMBER` / `FORCE_BLUE` / `FORCE_ALERT` / `FORCE_RAINBOW` / `FORCE_BLUE_SLOW` | modes de la finale (forcés) |
| `FORCE_RECHARGE` | flash blanc « rechargée par la Cloche » puis retour auto au bleu stable |
| `TIME <epoch_local>` | synchronise l'horloge — **epoch décalé en heure locale** (voir §6) |

Tout mode **forcé** maintient la balise éveillée (pas de deep-sleep) jusqu'à
`FORCE_IDLE`.

| Mode | # | Rendu |
|---|---|---|
| `OFF` | 0 | éteint |
| `IDLE` | 1 | respiration bleue faible (veille nocturne, visible des tentes) |
| `GLITCH` | 2 | néon bleu instable, flashs blanc/teal et coupures aléatoires |
| `AMBER` | 3 | ambre fixe (finale — préparation) |
| `BLUE` | 4 | bleu stable plein (finale — « la balise tient ») |
| `ALERT` | 5 | ambre clignotant ~1,4 Hz (finale — le Windigo teste le périmètre) |
| `RAINBOW` | 6 | arc-en-ciel pulsant (finale — accord final) |
| `BLUE_SLOW` | 7 | bleu profond, respiration très lente (finale — fin) |
| `RECHARGE` | 8 | flash blanc 1,6 s puis retour auto à `BLUE` |

### 4.2 Lanterne (`lanterne_esp8266`)

| Commande | Effet |
|---|---|
| `SETMODE <0..5>` | change de mode |
| `FORCE_OFF` / `FORCE_CANDLE` / `FORCE_ALERT` / `FORCE_WHITE` | modes courants |
| `FORCE_REVELATION` / `FORCE_WINDIGO` | modes de la finale |
| `FORCE_GLITCH`, `FINAL` | **alias** → `ALERT` (un « glitch all » déclenche aussi la lanterne) |

| Mode | # | Rendu |
|---|---|---|
| `OFF` | 0 | éteint |
| `CANDLE` | 1 | bougie chaude vacillante (défaut) |
| `ALERT` | 2 | pulsation rouge |
| `WHITE` | 3 | blanc chaud brillant (jamais bleuté) |
| `REVELATION` | 4 | lueur chaude/mystique tournante (finale phase 2 — ombres musicales) |
| `WINDIGO` | 5 | fondu au noir ~4 s puis clignotement rouge (finale phase 3) |

Le dernier mode est persisté en EEPROM (reprise après coupure).

### 4.3 Médaillon (`medaillon_esp8266`)

| Commande | Effet |
|---|---|
| `TRIGGER` | comète montant vers un sommet doré/blanc (~7 s), puis scintillement tenu |
| `STOP`, `FORCE_OFF` | retour au repos (éteint) |

| Mode | # |
|---|---|
| `OFF` | 0 |
| `ILLUMINATE` | 1 |

### 4.4 Diapason (`diapason_digispark`)

**Hors réseau** — prop autonome (RFID + vibration + backup IR), aucun message
ESP-NOW. Voir `diapason_digispark/README.md`.

## 5. Modèle de fiabilité (master)

- **Présence** : un accessoire est « en ligne » si un HB a été reçu depuis
  moins de **10 s** ; les accessoires émettent leur HB toutes les **5 s**
  éveillés. Une balise en deep-sleep (le jour) n'émet qu'un HB par réveil
  (**60 s**) et n'écoute que **2 s** après l'émission.
- **File de commandes** : une commande en attente par accessoire (la dernière
  remplace la précédente). Retries espacés du cycle de réveil, plafonnés par
  `retries` (`config.json`, défaut 3), expiration de sécurité 24 h.
- **Push sur heartbeat** : à la réception d'un HB d'un accessoire qui a une
  commande en attente, le master la pousse immédiatement (c'est la fenêtre de
  réveil fiable des balises endormies).
- **Espacement radio** : au plus un `esp_now_send` par fenêtre de
  `unicast_gap_ms` (défaut 300 ms), sans jamais bloquer la pile réseau.

## 6. Sémantique de `TIME`

L'ESP8266 n'a pas de gestion de fuseau : la balise applique `gmtime()` sur
l'epoch reçu pour décider jour/nuit (nuit ≈ 20 h 35 → 5 h 30). L'interface web
envoie donc un **epoch déjà décalé en heure locale** :
`Math.floor((Date.now() - tzOffset) / 1000)`. Ne jamais envoyer d'epoch UTC
brut. Les valeurs ≤ 1 600 000 000 sont rejetées (`ACK … ERR`).

Sans heure synchronisée (ou après 10 min sans resynchronisation au réveil),
la balise **suppose la nuit** : elle reste éveillée et en veille bleue plutôt
que de dormir au mauvais moment.

## 7. WebSocket UI ↔ master (port 81, JSON)

### Requêtes (UI → master)

| `action` | Champs | Effet |
|---|---|---|
| `send` | `targets: [id...]`, `cmd`, `arg` | met en file `CMD\|<cmd>[\|<arg>]` pour chaque cible |
| `send_all` | `cmd`, `arg` | idem pour tous les accessoires connus |
| `scan` | — | renvoie `scan_result` |
| `finale` | `params` | met en file `CMD\|FINAL\|<params>` pour tous |

Plus `{"type":"ping"}` → `{"type":"pong"}` (keepalive).

### Événements (master → UI)

| `type` | Contenu |
|---|---|
| `scan_result` | `items[]` : id, name, mac, present, lastHbMs, mode, batteryMv, batteryPct |
| `hb_batch` | agrégat (≤ 1/s) : id, mode, batt, battPct, present, mac, lastHbMs |
| `status` | changement de présence : id, present |
| `pending_update` | commande en attente : id, cmd, nextTryAt, attempts |
| `send_result` | id, cmd, ok, [queued], [reason: `no_mac` / `queue_full` / `max_attempts` / `expired`] |
| `ack` | ACK reçu de l'accessoire : id, cmd, status |
| `log` | message texte libre |
