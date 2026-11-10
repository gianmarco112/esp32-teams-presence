# ESP32 Teams Presence

Firmware standalone per ESP32-WROOM-32D che legge la presenza dell'utente Microsoft autenticato in Teams tramite Microsoft Graph e la visualizza con LED WS2812B.

## Hardware e funzioni

- Scheda: ESP32 DevKit con modulo ESP32-WROOM-32D (non ESP32-S3).
- LED: WS2812B RGB, 8 LED nel prototipo, DATA su GPIO18.
- Wi-Fi: captive portal WiFiManager al primo avvio.
- Microsoft: OAuth 2.0 Device Code Flow, una App Registration single-tenant e permesso delegated Microsoft Graph Presence.Read.
- Graph: endpoint /me/presence, polling ogni 60 secondi.
- Token di refresh conservato in NVS sul dispositivo.

Colori LED: Available verde, Busy rosso, DoNotDisturb viola, Away giallo, BeRightBack arancione, Offline spento, errori/stato sconosciuto blu lampeggiante.

## Compilazione locale

1. Installare VS Code e PlatformIO.
2. Copiare include/config.h.example in include/config.h.
3. Inserire il Tenant ID e il Client ID della App Registration Microsoft.
4. Eseguire il build dell'ambiente esp32dev.

Il file include/config.h è escluso da Git: non committare la configurazione locale. Non inserire client secret nel firmware.

## Compilazione automatica GitHub Actions

Il workflow .github/workflows/firmware.yml:

- compila il progetto ad ogni push su main, pull request e avvio manuale;
- usa PlatformIO e l'ambiente esp32dev;
- prepara bootloader.bin, partitions.bin, boot_app0.bin, firmware.bin e manifest.json;
- carica il pacchetto come artifact della build;
- quando viene pubblicato un tag v*, crea una GitHub Release con tutti gli asset necessari all'installer.

### Configurare gli ID Microsoft per le release

Prima di creare la prima release, aggiungere queste due **Actions variables** nelle impostazioni del repository (Settings → Secrets and variables → Actions → Variables):

- MS_TENANT_ID: Directory (tenant) ID della App Registration.
- MS_CLIENT_ID: Application (client) ID della stessa App Registration.

Sono identificativi dell'applicazione, non client secret. Il firmware non deve contenere un client secret. Le build su main possono essere eseguite anche senza variabili, usando valori segnaposto solo per verificare la compilazione; una release taggata richiede invece entrambi i GUID reali.

### Pubblicare una release

Dopo aver configurato le due variabili, creare e inviare un tag, ad esempio:

    git tag v1.0.0
    git push origin v1.0.0

GitHub Actions compilerà il firmware e pubblicherà gli asset nella Release. La pubblicazione della release attiverà anche il workflow GitHub Pages, che sincronizza i binari nella cartella statica servita dal sito installer.

## Web installer

Installer USB: https://gianmarco112.github.io/esp32-teams-presence/

La pagina propone l'ultima release stabile e permette di installare versioni precedenti. Usa Web Serial con Chrome o Edge desktop su HTTPS. Il flash va comunque validato su una scheda fisica prima della distribuzione agli utenti.

## Collegamenti LED

Vedere docs/wiring.md. La configurazione prototipo collega GPIO18 direttamente a DIN, con massa comune; l'alimentazione dei LED deve essere 5 V separata, mai 3V3. Per instabilità del segnale usare un level shifter 74AHCT125/74HCT14.

## Limiti e sicurezza

- L'endpoint Graph /me/presence mostra solo la presenza dell'account che ha effettuato il login sul dispositivo.
- Le policy del tenant possono richiedere il consenso amministratore.
- La NVS standard non è cifrata automaticamente e contiene il refresh token.
- La V1 non implementa OTA, Flash Encryption, Secure Boot o watchdog applicativo.
- Non usare setInsecure(): le richieste HTTPS verificano i certificati tramite ESP32CertBundle.
- Il firmware non è ancora certificato per la produzione; eseguire prove su hardware e controlli di sicurezza prima della distribuzione.
