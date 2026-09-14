# nrfnode

Firmware för Panasonic **PAN1780** (nRF52840) på PAN1780 EVK med en SSD1306-OLED.
Kortet fungerar som en fristående klocka/alarm och, i Matter-bygget, som en
egen **Matter-commissioner och Thread-ledare** som läser en IKEA TIMMERFLOTTE
(temperatur och luftfuktighet) utan hub, telefon eller border router.

## Repo-struktur

```
firmware/              nRF Connect SDK (NCS) v3.4.0-workspace
  app/                 Fristående OLED/klock-app (klocka, alarm, parningskod, sensorvy)
  matter_proto/        Matter-commissioner + Thread-ledare + samma OLED-UI
  sniffer/             802.15.4-sniffer-firmware för felsökning
docs/architecture.md   Hur Matter-bygget är uppbyggt och dess begränsningar
```

## Status

| Del | Status |
|-----|--------|
| OLED-klocka, alarm, ställ-läge, parningskodsinmatning | Fungerar (`app/`) |
| Matter-commissioning av TIMMERFLOTTE över BLE + Thread | Fungerar, verifierat på kort |
| Thread-nät (PAN1780 är ledare) och SRP/DNS-SD utan border router | Fungerar |
| CASE-återanslutning efter omstart | Fungerar |
| Prenumeration på temperatur och luftfuktighet | Fungerar; förnyas ca var 5:e–9:e minut |
| Visning på OLED (mode 4) med stale-flagga | Fungerar |
| Senaste sensorvärde i retained RAM | Fungerar (överlever reset, inte strömavbrott) |
| Attestation | **Förbikopplad** (IKEA:s PAA saknas i trust store) |
| Android-app, egen BLE-tjänst, FOTA | Inte implementerat |

Se [firmware/README.md](firmware/README.md) för bygg- och flashinstruktioner,
lägen och knappar, och [docs/architecture.md](docs/architecture.md) för
Matter-bygget.
