# Balise Est sur ESP32

Port de test de la Balise Est pour un ESP32 DevKit v1. Il réutilise la logique
d'effets et le protocole de la balise ESP8266.

- Anneau NeoPixel : GPIO 2 par défaut (`LED_PIN` dans `platformio.ini`).
- Mesure batterie : GPIO 34 par défaut (`BATTERY_PIN`).
- Identité : ID 3, `Balise_Est`.
- `DEBUG_MODE=1` : pas de deep-sleep pendant les essais.

```bash
pio run -d balise_esp32 -e balise_est_esp32
```

