#include "devices.h"

#include "alarm.h"
#include "rtc_clock.h"
#include "storage.h"

#include <cerrno>
#include <cstring>

#include <zephyr/drivers/retained_mem.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

namespace {

constexpr uint16_t kNvsDevicesId = 3;
// Offset 0..11 används av rtc_clock.
constexpr size_t kSensorRetainedOffset = 16;
constexpr size_t kCodeSize = kPairingCodeDigits + 1;
constexpr uint32_t kSensorSnapshotMagic = 0x534E5331U;
constexpr uint32_t kSensorStaleSeconds = 15U * 60U;

char g_codes[kDeviceCount][kCodeSize];

struct SensorSnapshot {
	uint32_t magic;
	int16_t temperature_centi;
	uint16_t humidity_centi;
	uint32_t temperature_timestamp;
	uint32_t humidity_timestamp;
	uint8_t valid;
	uint8_t reserved[3];
};

// Senaste värdet ligger bara i retained RAM (överlever reset, aldrig flash).
struct RetainedSensor {
	SensorSnapshot snapshot;
	uint32_t crc;
};

const struct device *const kRetained = DEVICE_DT_GET(DT_NODELABEL(retainedmem0));

uint32_t SensorCrc(const SensorSnapshot &snapshot)
{
	return crc32_ieee(reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot));
}

SensorSnapshot g_sensor = {kSensorSnapshotMagic, 0, 0, 0, 0, 0, {0, 0, 0}};
K_MUTEX_DEFINE(g_sensor_mutex);

const uint8_t kD[10][10] = {
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 2, 3, 4, 0, 6, 7, 8, 9, 5},
	{2, 3, 4, 0, 1, 7, 8, 9, 5, 6}, {3, 4, 0, 1, 2, 8, 9, 5, 6, 7},
	{4, 0, 1, 2, 3, 9, 5, 6, 7, 8}, {5, 9, 8, 7, 6, 0, 4, 3, 2, 1},
	{6, 5, 9, 8, 7, 1, 0, 4, 3, 2}, {7, 6, 5, 9, 8, 2, 1, 0, 4, 3},
	{8, 7, 6, 5, 9, 3, 2, 1, 0, 4}, {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}};

const uint8_t kP[8][10] = {
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 5, 7, 6, 2, 8, 3, 0, 9, 4},
	{5, 8, 0, 3, 7, 9, 6, 1, 4, 2}, {8, 9, 1, 6, 0, 4, 3, 5, 2, 7},
	{9, 4, 5, 3, 1, 2, 6, 8, 7, 0}, {4, 2, 8, 6, 5, 7, 3, 9, 0, 1},
	{2, 7, 9, 3, 8, 0, 6, 4, 1, 5}, {7, 0, 4, 6, 9, 1, 3, 2, 5, 8}};

}

bool PairingCodeValid(const char *code)
{
	if (strlen(code) != kPairingCodeDigits) {
		return false;
	}

	uint8_t check = 0;
	bool all_zero = true;

	for (int i = 0; i < kPairingCodeDigits; i++) {
		const char c = code[kPairingCodeDigits - 1 - i];

		if (c < '0' || c > '9') {
			return false;
		}
		all_zero = all_zero && c == '0';
		check = kD[check][kP[i % 8][c - '0']];
	}

	// Koder utan VID/PID har första siffran 0-3.
	return check == 0 && !all_zero && code[0] <= '3';
}

void DevicesInit()
{
	memset(g_codes, 0, sizeof(g_codes));

	char stored[kDeviceCount][kCodeSize];

	if (NvsReadBlob(kNvsDevicesId, stored, sizeof(stored)) == static_cast<int>(sizeof(stored))) {
		for (int i = 0; i < kDeviceCount; i++) {
			stored[i][kCodeSize - 1] = '\0';
			if (PairingCodeValid(stored[i])) {
				memcpy(g_codes[i], stored[i], kCodeSize);
			}
		}
	}

	RetainedSensor retained;
	if (device_is_ready(kRetained) &&
	    retained_mem_read(kRetained, kSensorRetainedOffset,
			      reinterpret_cast<uint8_t *>(&retained), sizeof(retained)) == 0 &&
	    retained.snapshot.magic == kSensorSnapshotMagic &&
	    retained.crc == SensorCrc(retained.snapshot) &&
	    (retained.snapshot.valid & ~0x03U) == 0) {
		const SensorSnapshot &snapshot = retained.snapshot;
		k_mutex_lock(&g_sensor_mutex, K_FOREVER);
		g_sensor = snapshot;
		k_mutex_unlock(&g_sensor_mutex);
		printk("SENSOR cache temp_valid=%u temp_centi=%d temp_timestamp=%u "
		       "humidity_valid=%u humidity_centi=%u humidity_timestamp=%u\r\n",
		       (snapshot.valid & 0x01U) ? 1U : 0U, snapshot.temperature_centi,
		       static_cast<unsigned>(snapshot.temperature_timestamp),
		       (snapshot.valid & 0x02U) ? 1U : 0U,
		       static_cast<unsigned>(snapshot.humidity_centi),
		       static_cast<unsigned>(snapshot.humidity_timestamp));
	}
}

const char *DeviceCode(int slot)
{
	return g_codes[slot];
}

int DeviceCodeSave(int slot, const char *code)
{
	if (slot < 0 || slot >= kDeviceCount || !PairingCodeValid(code)) {
		return -EINVAL;
	}

	char updated[kDeviceCount][kCodeSize];

	memcpy(updated, g_codes, sizeof(updated));
	memcpy(updated[slot], code, kCodeSize);

	const int err = NvsWriteBlob(kNvsDevicesId, updated, sizeof(updated));

	if (err) {
		return err;
	}

	memcpy(g_codes, updated, sizeof(g_codes));
	return 0;
}

bool DeviceCommissioned(int slot)
{
	(void)slot;
	return false;
}

SensorReading SensorGet(int slot, uint32_t unix_time)
{
	(void)slot;
	SensorSnapshot snapshot;
	k_mutex_lock(&g_sensor_mutex, K_FOREVER);
	snapshot = g_sensor;
	k_mutex_unlock(&g_sensor_mutex);

	const bool temperature_valid = (snapshot.valid & 0x01U) != 0;
	const bool humidity_valid = (snapshot.valid & 0x02U) != 0;
	const bool temperature_stale = !temperature_valid ||
		unix_time < snapshot.temperature_timestamp ||
		unix_time - snapshot.temperature_timestamp >= kSensorStaleSeconds;
	const bool humidity_stale = !humidity_valid ||
		unix_time < snapshot.humidity_timestamp ||
		unix_time - snapshot.humidity_timestamp >= kSensorStaleSeconds;
	const int temperature_dc = snapshot.temperature_centi >= 0
		? (snapshot.temperature_centi + 5) / 10
		: (snapshot.temperature_centi - 5) / 10;

	return {static_cast<int16_t>(temperature_dc),
		static_cast<uint8_t>((snapshot.humidity_centi + 50U) / 100U),
		temperature_valid, humidity_valid, temperature_stale, humidity_stale};
}

namespace {

void SaveSensorSnapshot()
{
	SensorSnapshot snapshot;
	k_mutex_lock(&g_sensor_mutex, K_FOREVER);
	snapshot = g_sensor;
	k_mutex_unlock(&g_sensor_mutex);

	RetainedSensor retained = {snapshot, SensorCrc(snapshot)};
	if (!device_is_ready(kRetained) ||
	    retained_mem_write(kRetained, kSensorRetainedOffset,
			       reinterpret_cast<const uint8_t *>(&retained),
			       sizeof(retained)) != 0) {
		printk("ERROR sensor_retain\r\n");
	}
}

} // namespace

void SensorUpdateTemperature(int16_t value_centi, uint32_t timestamp)
{
	k_mutex_lock(&g_sensor_mutex, K_FOREVER);
	if ((g_sensor.valid & 0x01U) != 0 &&
	    g_sensor.temperature_centi == value_centi) {
		g_sensor.temperature_timestamp = timestamp;
	} else {
		g_sensor.temperature_centi = value_centi;
		g_sensor.temperature_timestamp = timestamp;
		g_sensor.valid |= 0x01U;
	}
	k_mutex_unlock(&g_sensor_mutex);
	SaveSensorSnapshot();
}

void SensorUpdateHumidity(uint16_t value_centi, uint32_t timestamp)
{
	k_mutex_lock(&g_sensor_mutex, K_FOREVER);
	if ((g_sensor.valid & 0x02U) != 0 &&
	    g_sensor.humidity_centi == value_centi) {
		g_sensor.humidity_timestamp = timestamp;
	} else {
		g_sensor.humidity_centi = value_centi;
		g_sensor.humidity_timestamp = timestamp;
		g_sensor.valid |= 0x02U;
	}
	k_mutex_unlock(&g_sensor_mutex);
	SaveSensorSnapshot();
}
