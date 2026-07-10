# Master de test ESP8266

Master léger pour piloter une balise déjà flashée lorsqu'aucun ESP32 n'est
disponible. Le firmware crée le point d'accès **MASTER_TEST** (mot de passe
`masterpass`) et sert une petite console à l'adresse **http://192.168.4.1/**.

Il utilise le même canal 9 et le même protocole ESP-NOW que le master ESP32.
La balise n'a donc besoin d'aucune modification. Le premier heartbeat reçu
enregistre automatiquement son adresse MAC; la page affiche ensuite son ID,
son mode, sa batterie et son dernier ACK.

## Compiler et flasher

Depuis la racine `Scout2026` :

```bash
~/.platformio/penv/bin/pio run -d master_esp8266_test -e master_test
~/.platformio/penv/bin/pio run -d master_esp8266_test -e master_test -t upload --upload-port /dev/ttyS1
```

Depuis le dossier `master_esp8266_test` lui-même :

```bash
~/.platformio/penv/bin/pio run -e master_test
cmd.exe /c C:\\Python312\\python.exe -m esptool --chip esp8266 --port COM2 --baud 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 40m --flash-size 1MB 0x0 .pio\\build\\master_test\\firmware.bin
```

Dans WSL, les ports Windows sont nommés autrement : **COM2 = `/dev/ttyS1`**,
COM3 = `/dev/ttyS2`, etc. Cependant, WSL peut retourner `Input/output error`
en tentant de configurer un port USB série. La commande `cmd.exe` ci-dessus
appelle alors directement l'outil Windows et constitue la méthode recommandée.

Si la commande courte `pio` lance encore l'ancien PlatformIO 4.3.4 de
`/usr/bin`, actualiser le `PATH` du terminal courant :

```bash
export PATH="$HOME/.local/bin:$PATH"
hash -r
pio --version
```

La version affichée doit être PlatformIO Core 6.1.19 ou plus récente.

Une seule balise est suivie à la fois afin de garder une bonne marge de RAM.
