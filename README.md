# ESP32 Teams Presence

Firmware per un dispositivo ESP32 che visualizza lo stato Microsoft Teams tramite LED WS2812B.

## Web installer

La pagina di installazione USB è nella cartella [installer](./installer/). Dopo aver abilitato GitHub Pages con sorgente **GitHub Actions** in **Settings → Pages**, il workflow pubblica il sito:

https://gianmarco112.github.io/esp32-teams-presence/

L'installer propone l'ultima release stabile e permette di installare versioni precedenti. Quando viene pubblicata una release, il workflow copia i relativi binari nell'area statica GitHub Pages, così il browser li scarica dallo stesso dominio dell'installer.

Finché non vengono pubblicati binari firmware compilati con il relativo `manifest.json`, il pulsante di installazione rimarrà disabilitato.

## Stato

L'installer web è in fase iniziale di integrazione. Il flash deve ancora essere provato su una ESP32-WROOM-32D con i binari effettivi del progetto.
