# Médaillon du Dernier Accord (ESP8266)

Le sceau final de la Symphonie, porté au cou. Quand la Baguette le touche
(finale, phase 4), le master envoie `TRIGGER` : une comète parcourt l'anneau
en dérivant de couleur et monte en ~7 s vers un sommet doré/blanc brillant,
puis scintille tenu jusqu'à `STOP`. Protocole : [PROTOCOL.md](../PROTOCOL.md).

## Matériel

- Wemos D1 mini lite + anneau NeoPixel **12 LEDs** sur **D4** (ajuster
  `NUM_LEDS` dans `src/main.cpp` au vrai anneau), batterie sur A0 via
  diviseur 100 kΩ / 220 kΩ.

## Build & flash

```bash
pio run -e medaillon_1 -t upload    # id 20
```

Médaillon supplémentaire : ajouter un environnement dans `platformio.ini`
(plage d'ID 20–29). `DEBUG_MODE=1` par défaut — passer à `0` avant le camp.

## Comportement

- Ne dort jamais, toujours à l'écoute ; repos éteint par défaut.
- Pas de persistance EEPROM : après une coupure il revient au repos (voulu —
  l'illumination est un moment déclenché, pas un état durable).
