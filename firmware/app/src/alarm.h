#pragma once

#include <cstddef>
#include <cstdint>

constexpr int kAlarmCount = 5;

struct Alarm {
	uint8_t hour;    // 0-23
	uint8_t minute;  // 0-59
	uint8_t days;    // bit 0 = måndag ... bit 6 = söndag
	uint8_t enabled; // 0 eller 1
};

constexpr uint8_t kAlarmAllDays = 0x7F;

// Monterar NVS i storage-partitionen och läser alarmen.
// Returnerar 0 vid lyckad montering; vid tomt/ogiltigt innehåll används standardvärden.
int AlarmsInit();

// Sant om alarmen lästes från flash i stället för att standardvärden användes.
bool AlarmsWereLoaded();

// Pekare till alarm 0..kAlarmCount-1 som kan ändras direkt.
Alarm *AlarmAt(int index);

// Skriver alarmen till flash. NVS skriver inget om innehållet är oförändrat,
// så anropet kan göras utan att slita på flashen. Returnerar 0 eller negativ errno.
int AlarmsSave();

// Första aktiva alarm (index) som matchar tid och veckodag, annars -1.
int AlarmsMatch(uint32_t unix_time);

// Rå läs/skriv mot samma NVS för andra moduler. Returnerar antal bytes (läs), 0 (skriv) eller negativ errno.
int NvsReadBlob(uint16_t id, void *data, size_t len);
int NvsWriteBlob(uint16_t id, const void *data, size_t len);

// Standardvisningsläget lagras i samma NVS. Load returnerar 0 om inget sparats.
uint8_t DefaultModeLoad();
int DefaultModeSave(uint8_t mode);
