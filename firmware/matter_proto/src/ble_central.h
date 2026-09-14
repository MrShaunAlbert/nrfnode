#pragma once

#include <zephyr/bluetooth/addr.h>
#include <cstdint>

void BleCentralOnMatterAdv(const bt_addr_le_t *addr, uint16_t discriminator);
