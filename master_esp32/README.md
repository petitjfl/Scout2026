# Master — Baguette du Chef d'orchestre (ESP32)

Console de régie des artefacts : point d'accès WiFi `MASTER_AP` servant
l'interface web depuis LittleFS, et coordinateur ESP-NOW des accessoires
(canal 9). Protocole : voir [PROTOCOL.md](../PROTOCOL.md).

## Matériel

- ESP32 DevKit v1 (`esp32doit-devkit-v1`), alimentation USB / batterie.
- Aucun périphérique câblé : tout passe par la radio.

## Build & flash

```bash
pio run -t upload      # firmware (port : COM4, voir platformio.ini)
pio run -t uploadfs    # interface web = contenu de data/ vers LittleFS
```

À refaire (`uploadfs`) après toute modification de `data/`.

## Contenu

- `src/main.cpp` — AP + HTTP (:80) + WebSocket (:81) + ESP-NOW :
  - file de commandes en attente (1 slot par accessoire, retries, expiration) ;
  - push immédiat d'une commande en attente à la réception du heartbeat
    (fenêtre de réveil des balises endormies) ;
  - réception ESP-NOW découplée par une file SPSC — le callback (tâche WiFi)
    ne touche jamais WebSocket/LittleFS ;
  - watchdog AP : ré-affirme `WIFI_PS_NONE` (le power-save faisait décrocher
    le téléphone) et relance le softAP si son IP devient invalide.
- `data/` — interface web :
  - `index.html` / `style.css` / `app.js` — vues **Prod** (gros boutons,
    FINALE, scènes), **Détaillé** (tableau, journal) et **Discret**
    (plein écran sombre) ;
  - `config.json` — accessoires connus (id, nom, MAC apprise), `retries`,
    `unicast_gap_ms`. Réécrit par le master quand une MAC est apprise.

## Notes

- Le bouton **Time (sync)** envoie un epoch **décalé en heure locale**
  (les balises utilisent `gmtime()` sans fuseau). À presser après chaque
  mise en route du master.
- SSID/mot de passe et canal : `AP_SSID`, `AP_PASS`, `WIFI_CHANNEL` en tête de
  `src/main.cpp`. Le canal doit correspondre à celui des accessoires.
