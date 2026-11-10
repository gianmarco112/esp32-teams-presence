# Collegamenti – ESP32-WROOM-32D + WS2812B

Configurazione prototipo: 8 LED WS2812B, GPIO18 come DATA.

| ESP32 / alimentatore | WS2812B | Note |
|---|---|---|
| GPIO18 | DIN | Segnale dati; per il prototipo collegamento diretto |
| GND ESP32 | GND LED | Massa comune obbligatoria |
| +5 V alimentatore | +5V LED | Mai usare il pin 3V3 per alimentare i LED |
| GND alimentatore | GND LED | Massa comune con ESP32 |

Aggiungere preferibilmente una resistenza da 330–470 Ω in serie al DATA e un condensatore da 1000 µF tra 5 V e GND vicino alla strip.

## Collegamento diretto del DATA

Il GPIO dell'ESP32 fornisce un segnale logico da 3,3 V mentre il WS2812B è alimentato a 5 V. Il collegamento diretto spesso funziona, ma non è garantito per tutte le revisioni/strip: se compaiono colori casuali, sfarfallii o mancata accensione, utilizzare un level shifter 74AHCT125 o 74HCT14 alimentato a 5 V.

## Alimentazione

- Un WS2812B può assorbire fino a circa 60 mA a bianco pieno.
- Per 8 LED prevedere un alimentatore 5 V da almeno 1 A.
- Non alimentare l'anello dal pin 3V3 dell'ESP32.
- Evitare di collegare contemporaneamente USB e un'alimentazione esterna al pin 5V/VIN senza conoscere il circuito di power-path della propria DevKit.
- Verificare il verso della strip: il segnale va su DIN del primo LED, seguendo la freccia.

## Impostazioni firmware

Nel file include/config.h:

    #define LED_PIN 18
    #define LED_COUNT 8
    #define LED_BRIGHTNESS 80
