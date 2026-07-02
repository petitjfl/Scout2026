# Lanterne des Portées (ESP8266)

L'artefact de la révélation et de la transmission : elle éclaire les sentiers
nocturnes, projette les « ombres musicales » de la finale et incarne
l'approche du Windigo (phase 3). Protocole et liste complète des
commandes/modes : [PROTOCOL.md](../PROTOCOL.md).

## Matériel

- Wemos D1 mini lite + anneau NeoPixel 6 LEDs sur **D4** (même câblage qu'une
  balise, batterie sur A0 via diviseur 100 kΩ / 220 kΩ).

## Build & flash

```bash
pio run -e lanterne_1 -t upload    # id 10
```

Lanterne supplémentaire : ajouter un environnement dans `platformio.ini`
(plage d'ID 10–19). `DEBUG_MODE=1` par défaut — passer à `0` avant le camp.

## Comportement

- Ne dort jamais et ne suit pas le cycle jour/nuit des balises : c'est une
  source de lumière, toujours à l'écoute (commandes fiables sans file
  d'attente longue).
- Dernier mode persisté en EEPROM : reprend son état après une coupure.
- Alias finale : `FORCE_GLITCH` et `FINAL` sont interprétés comme `ALERT`,
  pour qu'un « glitch all » déclenche aussi la lanterne.
