# Firmware - PAN1780

nRF Connect SDK (NCS)-firmware för Panasonic PAN1780 (nRF52840). `app/` är den
fristående OLED/klock-appen och `matter_proto/` är bygget som kombinerar
Matter/Thread/BLE med samma OLED-UI. `sniffer/` är en 802.15.4-sniffer för
felsökning.

## Förutsättningar

- nRF Util med SDK Manager och en NCS v3.4.0-toolchain.
- `west` körs i den installerade toolchain-miljön.

## Initiera workspace

Körs en gång, från repo-roten. Detta skapar `.west/`, samt hämtar `nrf`,
`zephyr` och moduler som **syskon-mappar utanför `firmware/`**
(de checkas inte in i git, se `.gitignore`).

**Reproducerbara versioner:** [app/west.yml](app/west.yml) är manifestet och pinnar
`sdk-nrf` till taggen `v3.4.0`. Via `import: true` följer Zephyr (`ncs-v3.4.0`),
MCUboot, nrfxlib och alla moduler med exakta revisioner från den taggen, så
`west update` ger alltid samma versioner. Därför behövs inga git-submoduler.
Kontrollera med `west list` (visar revision per projekt). Byt version genom att ändra
`revision` i `west.yml` och köra `west update`. Toolchainen pinnas separat med
`nrfutil sdk-manager ... --ncs-version v3.4.0`.

```sh
cd firmware
west init -l app
west update
```

## Board-target

PAN1780 EVK stöds i Zephyr/NCS som `pan1780_evb/nrf52840`. SDK-versionen
hämtas från `app/west.yml`. Installera dess toolchain och hämta manifestets
moduler:

```sh
nrfutil sdk-manager toolchain install --ncs-version v3.4.0
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <firmware> -- west update
```

Byt `<firmware>` mot sökvägen till den här katalogen. Om `nrfutil` just
installerats, öppna en ny terminal så att den uppdaterade PATH:en används.

## Bygga

Alla kommandon körs från `firmware/` via toolchain-miljön. Byggena som används och
verifierats är utan sysbuild (ingen bootloader), med egen byggmapp per mål
(`build*/` är ignorerat av git):

```sh
# Fristående OLED/klock-app
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <firmware> -- west build --no-sysbuild -d build_app -b pan1780_evb/nrf52840 app

# Matter-commissioner + OLED-UI
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <firmware> -- west build --no-sysbuild -d build_matter -b pan1780_evb/nrf52840 matter_proto

# 802.15.4-sniffer
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <firmware> -- west build --no-sysbuild -d build_sniffer -b pan1780_evb/nrf52840 sniffer
```

Använd en ny byggmapp i stället för att återanvända en äldre med annan konfiguration.
`app/sysbuild.conf` aktiverar MCUboot för `west build --sysbuild`, men det bygget
är inte det som flashas och används i dag.

Matter-bygget kräver att `matter_proto/patches/matter-controller.patch` är
applicerad på `modules/lib/matter` (se [../docs/architecture.md](../docs/architecture.md)).
Storlek (Matter-bygget): flash ca 953 KB (90,9 % av 1 MB), RAM ca 217 KB
(83,9 % av 252 KB). Fristående app: flash ca 91 KB, RAM ca 18 KB.

## Flasha

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir <firmware> -- west flash -d <byggmapp> --dev-id <J-Link-serienummer>
```

Byt `<firmware>` mot sökvägen till den här katalogen. Kortet måste vara anslutet
via en programmerare som stöds av `pan1780_evb`; installera J-Link vid behov.
Efter flashning kan Flotte behöva ca 5 minuter för att ansluta igen (Thread-backoff)
om kortet startats om flera gånger i rad.


## Hårdvara

OLED: SSD1306 (128x32, I2C-adress 0x3c) på I2C0, SCL P0.27 och SDA P0.26 (EVK:ns
Arduino-header), definierat som noden `oled` i [app/app.overlay](app/app.overlay)
och motsvarande overlay i `matter_proto/boards/`. Kontrollera att modulen klarar
3,3 V och att dess pull-ups inte ligger på 5 V innan den kopplas in.
Knappar och LED:ar är EVK:ns `sw0`–`sw3` respektive `led0`–`led3`.

## UART

Zephyr-konsolen och loggningen använder UART0 på 115200 8N1 utan flödeskontroll
(COM3 på utvecklingsdatorn). Protokollet är radbaserat, ASCII, CRLF och
enkelriktat från kortet. Exempel (fullständig lista under Alarm nedan):
`DISPLAY ok ...`, `CLOCK restored=1`, `MODE default=0`, `TIME unix=... hms=...`
varje sekund, och i Matter-bygget `SENSOR temperature=24.43C timestamp=...`.

## Klocka (RTC) och OLED

RTC2 (32768 Hz från LFCLK) körs med en tick per sekund och räknar upp en UINT32
unix-timestamp (UTC) i [rtc_clock.cpp](app/src/rtc_clock.cpp). Tiden visas som
`HH:mm:ss` på SSD1306-displayen (128x32, adress 0x3c, nod `oled` i
[app.overlay](app/app.overlay)).

Tiden sparas varje sekund i retained RAM allra sist i RAM (64 byte på
0x2003ffc0, magic + CRC32; sensorvärden ligger på offset 16), alltså inte i
flash, så ingen flashslitage. Den
överlever reset och omprogrammering via J-Link men inte spänningsbortfall; då
startar klockan på 00:00:00 (`CLOCK restored=0`). Tiden under själva
resetögonblicket räknas inte med, så klockan kan tappa någon sekund per omstart.

Knappar:

| Knapp | Mode 0 (HH:mm:ss) | Mode 1 (HH:mm + veckodag) | Mode 2 (datum YY-MM-DD) | Mode 3 (tid + datum, två rader) | Mode 99 (ställ in) |
|-------|-------------------|---------------------------|-------------------------|---------------------------------|--------------------|
| SW4   | gå till mode 1    | gå till mode 2            | gå till mode 3          | gå till mode 4 (sensorer)       | gå till mode 0 |
| SW2   | -                 | -                         | -                       | -                               | nästa fält: timmar → minuter → sekunder → år → månad → dag → timmar |
| SW1   | -                 | -                         | -                       | -                               | upp (+1, rullar runt) |
| SW3   | -                 | -                         | -                       | -                               | ned (-1, rullar runt) |

Mode 3 visar `HH:mm:ss` på rad 1 och `YY-MM-DD WD` på rad 2 (WD = Må, Ti, On, To, Fr, Lö, Sö).

**Standardläge:** mode 0 är standard tills något annat ställts. Lämnas ett
visningsläge (0–3) orört i 10 s blir det nya standardläget (`MODE default=N`). Det
sparas i flash (NVS, id 2, bara när det byts) och används vid start. Ställ-lägena (50 och 99) går efter 10 s utan
knapptryck tillbaka till standardläget. SW4 från mode 99 går alltid till mode 0, så
att man kan nå alla lägen och välja ett annat standardläge.

Mode 0-2 visar tecken i full displayhöjd (32 px): sjusegmentssiffror ritade med
CFB-linjer och, i mode 1, två bokstäver för veckodagen (Må, Ti, On, To, Fr, Lö,
Sö) från egna 5x8-bitmappar i skala 4 (den inbyggda fonten saknar å/ö).

Datum och veckodag räknas ur unix-tiden (UTC, startvärde 1970-01-01 = torsdag,
skottår hanteras). Veckodagen ställs alltså genom att ändra datumet. Året kan
ställas mellan 1970 och 2099; mode 2 visar bara de två sista siffrorna. Ändras
månad eller år kapas dagen till månadens längd (31 → 28/29 i februari).

Mode 99 visar tid (HH:mm:ss) för tidsfälten och datum (YYYY-MM-DD) för datumfälten
med det egna 8x16-typsnittet (se Alarm nedan), samt en rad `SET <fält>`. Valt fält blinkar, och klockan
fortsätter gå.

Hålls SW1 eller SW3 inne mer än en sekund hoppar värdet därefter var sekund till
nästa jämna tiotal i den riktningen (t.ex. 07 → 10 → 20, ned: 07 → 00 → 50).

Utan knapptryck på 10 s återgår appen från mode 99 och mode 50 till standardläget.
SW4 växlar 0 → 1 → 2 → 3 → 4 → 50 → 60 → 61 → 99 → 0.

### Sensorvisning (mode 4)

Byter var 2:a sekund mellan tid (HH:mm:ss), temperatur och luftfuktighet.
I Matter-bygget kommer värdena från prenumerationer på TIMMERFLOTTE. Innan
första rapporten visas `VANTAR DATA`; efter 15 minuter utan ny rapport visas
senaste värdet märkt `GAMMAL`. Bara senaste temperatur- och fuktvärdet med
varsin tidsstämpel sparas, i retained RAM (överlever reset men inte spänningsbortfall,
aldrig i flash); äldre mätvärden sparas inte. Den fristående
`app/`-bilden saknar Matter-prenumeration. Mode 4 kan bli standardläge precis som 0–3.

### Matter-bygget

`matter_proto/` är verifierat mot en fabriksåterställd IKEA TIMMERFLOTTE:

- BLE, PASE och Matter-commissioning inklusive Thread-dataset fungerar.
- PAN1780 bildar Thread-nätet och kör SRP/DNS-SD utan en separat border router.
- Enheten går att återansluta med CASE efter omstart.
- Temperatur prenumereras från endpoint 1, kluster `0x0402`; luftfuktighet
  prenumereras från endpoint 2, kluster `0x0405`.
- Värden och UTC-tidsstämplar loggas på UART och visas i OLED mode 4.

Återanslutning använder den sparade manuella parningskoden på enhetsplats 1 om
CASE misslyckas. Attestation godkänns fortfarande utan verifiering eftersom
IKEA:s PAA saknas i trust store. Prenumerationen förnyas ca var 5:e–9:e minut
eftersom Flotte inte skickar keepalive (se [../docs/architecture.md](../docs/architecture.md)).
OpenThread-debugloggning, ping, netdiag och Matter-debugnivå är avstängda för att spara
flash; Matter- och sensorloggar på UART är kvar.

### Parningskod (mode 60)

Mata in Matter manuell parningskod (11 siffror) för upp till 4 enheter. Inmatningen
börjar alltid med en tom kod (`0000-000-0000`); tidigare sparade koder visas inte.
Koden visas i klartext som `XXXX-XXX-XXXX`. SW2 byter fält: siffra 1–11 → plats →
spara → siffra 1. SW1/SW3 ändrar vald siffra respektive plats (valt fält blinkar).
Platsfältet visar `E2 LEDIG` eller `E2 SKRIV ÖVER` (första lediga plats föreslås).
SW2 på `SPARA` validerar koden (Verhoeff-kontrollsiffra, första siffran 0–3,
inte enbart nollor) och sparar den i NVS (id 3); därefter börjar inmatningen om.
Ogiltig kod visar `UGILTIG KOD` och sparas inte. Osparade ändringar förloras vid timeout (10 s).

### Enhetslista (mode 61)

Visar plats 1–4: `E1 TOM`, `E1 KOD SPARAD` eller `E1 ANSLUTEN`, samt koden maskerad
som `xxxx-xxx-1234` (de fyra sista siffrorna) för att kunna identifiera enheten.
SW1/SW2 går framåt, SW3 bakåt. `ANSLUTEN` visas först när
Matter-commissioning finns (`DeviceCommissioned` returnerar tills dess alltid false).

### Lagring (Zephyr settings)

Alarm, standardläge och enhetskoder lagras som settings-poster `nrfnode/<id>`
(id 1 = alarm, 2 = standardläge, 3 = enhetskoder) i `storage_partition`,
via Zephyrs settings (NVS-backend), samma lagring som Matter-stacken använder.
Sensorernas senaste värden skrivs inte hit, utan till retained RAM (se klockan ovan),
eftersom de uppdateras ungefär 24 gånger i timmen. En eventuell gammal post med id 4
från tidigare versioner används inte längre. Vid första
starten efter övergången läses gamla råa NVS-poster (id 1–3) och skrivs över till
settings (`ALARMS loaded=1 migrated=1`).
Implementation: [storage.cpp](app/src/storage.cpp).

### Alarm (mode 50)

Upp till 5 alarm, vart och ett med tid (HH:mm), veckodagar (mån–sön) och på/av.
De lagras i flash (NVS i `storage_partition`, 0xf8000) och överlever alltså
strömbortfall. De skrivs bara när något ändrats, när man lämnar mode 50 (SW4 eller
10 s timeout). Fabriksvärde: 07:00 mån–fre, avstängt.

Mode 50 visar t.ex. `A1 07:30 PÅ` (eller `AV`) och på rad 2 veckodagarna `MTOTFLS` (inaktiv dag
visas som `-`). Texten i mode 50 och 99 använder ett eget 8x16-typsnitt med
Latin-1 (inkl. åäöÅÄÖ), genererat ur Unscii 8 (public domain) av
[scripts/gen_font.ps1](scripts/gen_font.ps1) till `app/src/font_unscii_8x16.inc`
(`powershell -ExecutionPolicy Bypass -File scripts\gen_font.ps1`). Strängar skrivs som
Latin-1-bytes i koden, t.ex. `"P\305"` för PÅ. SW2 byter fält: alarmnummer → timme → minut → mån … sön → på/av →
alarmnummer. SW1/SW3 ändrar värdet (för dagar och på/av växlar de). Valt fält
blinkar. Håll SW1/SW3 inne för tiotalshopp på timme/minut.

När ett aktivt alarm matchar tid och veckodag (kontrolleras när minuten byter, inte
i mode 50/99) blinkar LED1 med 5 Hz och displayen växlar 2 Hz mellan inverterad
"ALARM" och klockan. Valfri knapptryckning stoppar alarmet (tryckningen används
inte till något annat), annars stoppar det själv efter 1 minut.

Protokollrader:

```text
DISPLAY ok driver=ssd1306 size=128x32 addr=0x3c font=8x16
CLOCK restored=1
ALARMS loaded=1
ALARM id=1 time=07:30 days=MTOTF-- enabled=1
MODE mode=99 field=month
MODE mode=50 alarm=1 field=hours
MODE mode=0 reason=timeout
MODE default=3
ALARM_SAVED
ALARM_RING id=1
ALARM_STOP reason=button
TIME unix=1793311362 date=2026-10-29 hms=22:02:42 wday=thu
```
`TIME` skrivs varje sekund. Fel rapporteras som `ERROR display=...` eller
`ERROR init=rtc code=...`.

## Struktur

```
app/
  west.yml          Västmanifest (pekar på NCS-revision)
  app.overlay        I2C0 och OLED-nod, retained RAM, RTC2
  CMakeLists.txt
  prj.conf           Konfiguration för OLED, I2C, RTC, settings och UART
  sysbuild.conf       Sysbuild-inställningar (aktiverar MCUboot, används inte i dagens byggen)
  Kconfig             App-specifika Kconfig-alternativ
  src/
    main.cpp          OLED-UI: lägen, knappar, alarm, sensorvy
    button_handler.*  Knappar och LED
    rtc_clock.*       Klocka (RTC2) och retained RAM
    alarm.*           Alarm
    devices.*         Parningskoder och senaste sensorvärde (stale-logik)
    storage.*         Zephyr settings (NVS)
matter_proto/
  prj.conf, Kconfig, CMakeLists.txt, boards/   Matter + Thread + OLED-UI-konfiguration
  patches/matter-controller.patch              Ändringar i modules/lib/matter
  src/
    app_task.*      Startar storage, Matter-servern och UI-tråden
    controller.*    Commissioner, CASE-återanslutning, prenumerationer
    ble_scan.*, ble_central.*   BLE-scan och central för commissioning
sniffer/            802.15.4-sniffer
scripts/gen_font.ps1  Genererar font_unscii_8x16.inc
```
