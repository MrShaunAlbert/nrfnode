#pragma once

#include <lib/core/CHIPError.h>

// Initierar Matter-controllern (egen fabric) i Matter-tråden. Anropas som PostServerInit-callback.
CHIP_ERROR ControllerInit();

// Commissionerar en enhet med manuell parningskod (11 siffror) som nod `node_id`.
CHIP_ERROR ControllerCommission(const char *manual_code, uint64_t node_id);
