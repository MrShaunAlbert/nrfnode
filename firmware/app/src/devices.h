#pragma once

#include <cstdint>

constexpr int kDeviceCount = 4;
constexpr int kPairingCodeDigits = 11;

// Matter manuell parningskod: 11 siffror där sista är Verhoeff-kontrollsiffra.
bool PairingCodeValid(const char *code);

// Läser sparade koder ur NVS (efter AlarmsInit). Tomma platser får tom sträng.
void DevicesInit();

// Pekare till platsens kod (NUL-terminerad, 11 siffror eller tom sträng).
const char *DeviceCode(int slot);

// Sparar koden i NVS. Returnerar 0, -EINVAL för ogiltig kod eller negativ errno.
int DeviceCodeSave(int slot, const char *code);

// Sant när enheten är commissionerad (Matter är inte implementerat än, så alltid falskt).
bool DeviceCommissioned(int slot);

struct SensorReading {
	int16_t temperature_dc; // 0,1 °C
	uint8_t humidity_pct;
	bool temperature_valid;
	bool humidity_valid;
	bool temperature_stale;
	bool humidity_stale;
};

// Senaste värdena sparas i en enda settings-post med varsin UTC-tidsstämpel.
// Historiska mätvärden lagras inte.
void SensorUpdateTemperature(int16_t value_centi, uint32_t timestamp);
void SensorUpdateHumidity(uint16_t value_centi, uint32_t timestamp);

// Returnerar senaste cachelagrade värden; de blir stale efter 15 minuter.
SensorReading SensorGet(int slot, uint32_t unix_time);
