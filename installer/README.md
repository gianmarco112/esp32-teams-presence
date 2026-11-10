# Teams Presence — Web Installer

Pagina web per installare Teams Presence su ESP32-WROOM-32D tramite USB, con Web Serial ed esptool-js.

## Pubblicazione GitHub Pages

1. Aprire **Settings → Pages** nel repository.
2. In **Build and deployment → Source**, selezionare **GitHub Actions** e salvare.
3. Questo passaggio iniziale è manuale: il workflow non può attivare GitHub Pages da solo con il token standard di Actions.
4. Il workflow `.github/workflows/pages.yml` pubblica automaticamente il contenuto di `installer/` alla root del sito.
5. URL previsto: https://gianmarco112.github.io/esp32-teams-presence/

## Pubblicazione delle release firmware

L'installer recupera le GitHub Releases pubbliche del repository. La release più recente stabile viene proposta come installazione rapida; le altre vengono elencate nella sezione versioni precedenti. Le release draft e prerelease sono escluse.

Ogni release deve contenere un asset chiamato esattamente `manifest.json` e i file binari indicati nei suoi `path`. Esempio per la tabella partizioni standard ESP32:

```json
{
  "name": "Teams Presence",
  "version": "1.0.0",
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bootloader.bin", "offset": 4096 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
```

Gli offset dell'esempio corrispondono alla configurazione ESP32 Arduino/PlatformIO più comune, ma vanno verificati sui file generati dalla build effettiva prima di pubblicare una release. Non riutilizzare questi indirizzi alla cieca se viene modificata la tabella partizioni o la configurazione del framework.

## Requisiti browser

- Chrome o Microsoft Edge desktop aggiornati.
- Sito servito su HTTPS (GitHub Pages lo fornisce).
- Cavo USB dati.
- Driver USB-seriale della scheda, se necessario.

## Note

- L'installer usa esptool-js 0.7.1 e il formato manifest `builds[].parts[]`.
- Il browser deve supportare Web Serial. Safari e iOS non sono supportati per questo flusso.
- Questa prima versione non è ancora collaudata con i binari reali del firmware. Prima della distribuzione agli utenti occorre compilare il progetto, preparare una GitHub Release con i quattro file richiesti e provare il flash su una scheda di test.
- Il flash non può essere verificato senza un dispositivo collegato fisicamente.

