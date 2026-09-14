#include "alarm.h"

#include "storage.h"

#include <cerrno>

namespace {

constexpr uint16_t kNvsId = 1;
constexpr uint16_t kNvsDefaultModeId = 2;
constexpr uint32_t kSecondsPerDay = 86400U;

bool g_loaded;
Alarm g_alarms[kAlarmCount];

bool Valid(const Alarm &alarm)
{
	return alarm.hour < 24 && alarm.minute < 60 && (alarm.days & ~kAlarmAllDays) == 0 &&
	       alarm.enabled <= 1;
}

void SetDefaults()
{
	for (Alarm &alarm : g_alarms) {
		alarm = {7, 0, 0x1F, 0}; // 07:00 måndag-fredag, avstängt
	}
}

} // namespace

int AlarmsInit()
{
	SetDefaults();

	const int err = StorageInit();

	if (err) {
		return err;
	}

	Alarm stored[kAlarmCount];
	const int read = StorageRead(kNvsId, stored, sizeof(stored));

	if (read == static_cast<int>(sizeof(stored))) {
		bool all_valid = true;

		for (const Alarm &alarm : stored) {
			all_valid = all_valid && Valid(alarm);
		}
		if (all_valid) {
			for (int i = 0; i < kAlarmCount; i++) {
				g_alarms[i] = stored[i];
			}
			g_loaded = true;
		}
	}

	return 0;
}

bool AlarmsWereLoaded()
{
	return g_loaded;
}

Alarm *AlarmAt(int index)
{
	return &g_alarms[index];
}

int AlarmsSave()
{
	return StorageWrite(kNvsId, g_alarms, sizeof(g_alarms));
}

int NvsReadBlob(uint16_t id, void *data, size_t len)
{
	return StorageRead(id, data, len);
}

int NvsWriteBlob(uint16_t id, const void *data, size_t len)
{
	return StorageWrite(id, data, len);
}

uint8_t DefaultModeLoad()
{
	uint8_t mode = 0;

	if (StorageRead(kNvsDefaultModeId, &mode, sizeof(mode)) != static_cast<int>(sizeof(mode))) {
		return 0;
	}

	return mode;
}

int DefaultModeSave(uint8_t mode)
{
	return StorageWrite(kNvsDefaultModeId, &mode, sizeof(mode));
}

int AlarmsMatch(uint32_t unix_time)
{
	const uint32_t tod = unix_time % kSecondsPerDay;
	const unsigned int hour = tod / 3600U;
	const unsigned int minute = (tod / 60U) % 60U;
	// 1970-01-01 var en torsdag; 0 = måndag.
	const unsigned int weekday = (unix_time / kSecondsPerDay + 3U) % 7U;

	for (int i = 0; i < kAlarmCount; i++) {
		const Alarm &alarm = g_alarms[i];

		if (alarm.enabled && alarm.hour == hour && alarm.minute == minute &&
		    (alarm.days & (1U << weekday))) {
			return i;
		}
	}

	return -1;
}
