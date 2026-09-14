#pragma once

#include <cstdint>

// Startar RTC2 med en tick per sekund och räknar upp en UINT32 unix-timestamp (UTC).
// Returnerar 0 vid lyckad start, annars en negativ errno-kod.
int RtcClockInit(uint32_t initial_unix_time);

// Aktuell unix-timestamp i sekunder.
uint32_t RtcClockGet();

// Sätter unix-timestamp direkt.
void RtcClockSet(uint32_t unix_time);

// Lägger till ett signerat antal sekunder atomärt mot sekundtickens avbrott.
void RtcClockAdjust(int32_t delta_seconds);

// True om starttiden återställdes från retained RAM i stället för initialvärdet.
bool RtcClockWasRestored();

// Sparar nuvarande tid i retained RAM (RAM, inte flash, så den kan anropas varje sekund).
void RtcClockPersist();
