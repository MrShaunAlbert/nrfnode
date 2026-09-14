#include "ble_scan.h"
#include "ble_central.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
constexpr uint16_t kMatterServiceUuid = 0xFFF6;

bool ParseAd(struct bt_data *data, void *user_data)
{
	if (data->type != BT_DATA_SVC_DATA16 || data->data_len < 2 + 8) {
		return true;
	}
	uint16_t uuid = data->data[0] | (data->data[1] << 8);
	if (uuid != kMatterServiceUuid) {
		return true;
	}
	// Matter BLE service data: opcode(1), discriminator(2, 12 bit), VID(2), PID(2), flags(1)
	const uint8_t *d = &data->data[2];
	uint16_t disc = (d[1] | (d[2] << 8)) & 0x0FFF;
	uint16_t vid = d[3] | (d[4] << 8);
	uint16_t pid = d[5] | (d[6] << 8);
	static uint16_t sLastLogged;
	if (disc != sLastLogged) {
		sLastLogged = disc;
		LOG_INF("Matter BLE device: discriminator %u VID 0x%04x PID 0x%04x", disc, vid, pid);
	}
	*static_cast<int32_t *>(user_data) = disc;
	return false;
}

void OnScan(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
	int32_t disc = -1;
	bt_data_parse(ad, ParseAd, &disc);
	if (disc >= 0) {
		BleCentralOnMatterAdv(addr, static_cast<uint16_t>(disc));
	}
}
} // namespace

int BleScanStart()
{
	struct bt_le_scan_param p = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	bt_le_scan_stop();
	int err = bt_le_scan_start(&p, OnScan);
	LOG_INF("BLE scan start: %d", err);
	return err;
}
