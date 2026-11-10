# Teams Presence — Web Installer

Pagina web per installare Teams Presence su ESP32-WROOM-32D tramite USB, con Web Serial ed esptool-js.

## Pubblicazione GitHub Pages

1. Aprire **Settings → Pages** nel repository.
2. In **Build and deployment**, selezionare **Deploy from a branch**.
3. Selezionare branch `main` e cartella `/ (root)` non è adatta se si vuole pubblicare solo installer; in alternativa usare GitHub Actions per pubblicare la cartella `installer` oppure spostare i file installer nella root.
4. URL con Pages e cartella: se pubblicata dalla root, usare `https://gianmarco112.github.io/esp32-teams-presence/installer/`.

## Formato release richiesto

Ogni GitHub Release deve avere un asset chiamato `manifest.json`. Il manifest segue il formato ESP Web Tools:

```json
{
  "name": "Teams Presence",
  "version": "1.0.0",
  "new_install_prompt_erase": true,
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

Allegare alla stessa release anche i file indicati nei `path` del manifest. Gli offset devono corrispondere esattamente alla configurazione di partizioni/flash del firmware PlatformIO. Non assumere questi offset senza verificare i file prodotti dalla build.

L'installer legge le release pubbliche da GitHub API, propone l'ultima release stabile e mostra le precedenti. Le release draft e prerelease sono escluse.

## Requisiti browser

- Chrome o Microsoft Edge desktop aggiornati.
- Sito servito su HTTPS (o localhost durante sviluppo).
- Cavo USB dati.
- Driver USB-seriale della scheda, se necessario.

## Nota sul flash

Questa pagina è una prima implementazione e deve essere collaudata con i binari reali prodotti dal progetto. In particolare verificare la compatibilità della versione di esptool-js, il manifest e gli offset con una scheda di test prima di distribuirla agli utenti. Il browser non può flashare da iPhone/Safari né dai browser che non implementano Web Serial.

## Pubblicare il sito dalla cartella installer

Consigliato: configurare GitHub Pages con GitHub Actions e workflow che carica il contenuto di `installer/` come artifact Pages. Oppure usare una branch dedicata con i file di installer nella root.
