#pragma once

#include <cstdint>

// Antal fysiska knappar på PAN1780 EVK.
constexpr uint8_t kButtonCount = 4;

// Callback som anropas varje gång knapp-bitmasken ändras.
// Bit N (0-3) = knapp N+1, 1 = intryckt.
using ButtonStateChangedCallback = void (*)(uint8_t bitmask);

// Initierar GPIO-callbacks för de 4 knapparna på EVK:t.
// Returnerar 0 vid lyckad initiering, annars en negativ errno-kod.
int ButtonHandlerInit(ButtonStateChangedCallback callback);

// Registers the callback after the Matter board has initialized DK buttons and LEDs.
int ButtonHandlerAttach(ButtonStateChangedCallback callback);

// Läser nuvarande bitmask (t.ex. för att skicka initial status).
uint8_t ButtonHandlerGetState();
