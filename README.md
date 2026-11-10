# ESP32 Teams Presence

Firmware per un dispositivo ESP32 che visualizza lo stato Microsoft Teams tramite LED WS2812B.

## Web installer

La pagina di installazione USB è nella cartella [installer](./installer/). Dopo aver abilitato GitHub Pages con sorgente **GitHub Actions** in **Settings → Pages**, il workflow pubblicherà automaticamente il sito:

https://gianmarco112.github.io/esp32-teams-presence/

L'installer propone l'ultima release stabile e consente di selezionare anche release precedenti. Le release devono includere `manifest.json` e i relativi binari ESP32. Finché non vengono pubblicati firmware compilati, il pulsante di installazione rimane disabilitato.

## Stato del progetto

L'installer web è in fase iniziale di integrazione. Il flash va ancora verificato su ESP32-WROOM-32D con i binari generati dalla build ufficiale del firmware.
