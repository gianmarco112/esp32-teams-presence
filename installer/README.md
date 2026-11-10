# Teams Presence — Web Installer

Pagina web per installare Teams Presence su ESP32-WROOM-32D tramite USB, con Web Serial ed esptool-js.

## Pubblicazione GitHub Pages

1. Aprire **Settings → Pages** nel repository.
2. In **Build and deployment → Source**, selezionare **GitHub Actions** e salvare.
3. Questo passaggio iniziale va eseguito una sola volta: il token standard di GitHub Actions non può abilitare GitHub Pages per un repository in cui il servizio non è ancora attivo.
4. Il workflow `.github/workflows/pages.yml` pubblica il contenuto di `installer/` e scarica i binari delle GitHub Releases nella cartella statica `firmware/<tag>/`.
5. URL previsto: https://gianmarco112.github.io/esp32-teams-presence/

## Pubblicazione delle release firmware

L'installer recupera l'elenco delle GitHub Releases pubbliche tramite API. La release stabile più recente viene proposta come installazione rapida; le altre release stabili vengono mostrate nella sezione versioni precedenti. Le release draft e prerelease sono escluse.

Ogni release stabile deve contenere un asset chiamato esattamente `manifest.json` e tutti i file binari indicati nei suoi `path`. Quando viene pubblicata una release, GitHub Actions scarica gli asset lato server e li copia nel sito GitHub Pages. L'installer li legge poi dallo stesso dominio del sito, evitando richieste browser dirette al CDN degli asset GitHub.

Esempio per la tabella partizioni ESP32 standard:

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

Gli offset dell'esempio sono quelli comunemente usati dalle build ESP32 Arduino/PlatformIO con tabella partizioni standard. Prima di pubblicare il firmware vanno comunque verificati contro la configurazione effettiva della build: non riutilizzarli se cambiano framework, bootloader o tabella partizioni.

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
