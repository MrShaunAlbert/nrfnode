#pragma once

#include <cstddef>
#include <cstdint>

// Monterar Zephyrs settings (samma storage_partition som Matter använder) och flyttar vid första
// starten över äldre råa NVS-poster (id 1 = alarm, 2 = standardläge, 3 = enhetskoder).
// Returnerar 0 eller negativ errno.
int StorageInit();

// Sant om data migrerades från rå NVS vid den här starten.
bool StorageMigrated();

// Läser blob med givet id. Returnerar antal bytes, eller negativ errno (-ENOENT om saknas).
int StorageRead(uint16_t id, void *data, size_t len);

// Skriver blob med givet id. Returnerar 0 eller negativ errno.
int StorageWrite(uint16_t id, const void *data, size_t len);
