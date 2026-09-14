#include "rtc_clock.h"

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/crc.h>
#include <zephyr/spinlock.h>

#include <cstddef>

namespace {

const struct device *const kRtc = DEVICE_DT_GET(DT_NODELABEL(rtc2));

struct k_spinlock g_lock;
uint32_t g_unix_time;

// BehÃ¥ller en post med unix-tid i retained RAM (Ã¶verlever reset, inte spÃ¤nningsbortfall).
// Skrivningar till RAM slits inte som flash, sÃ¥ posten kan uppdateras varje sekund.
struct RetainedTime {
	uint32_t magic;
	uint32_t unix_time;
	uint32_t crc;
};

constexpr uint32_t kRetainedMagic = 0x52544331U; // "RTC1"

const struct device *const kRetained = DEVICE_DT_GET(DT_NODELABEL(retainedmem0));

bool g_restored;

uint32_t RetainedCrc(const RetainedTime &entry)
{
	return crc32_ieee(reinterpret_cast<const uint8_t *>(&entry), offsetof(RetainedTime, crc));
}

bool LoadRetained(uint32_t *unix_time)
{
	RetainedTime entry;

	if (!device_is_ready(kRetained) ||
	    retained_mem_read(kRetained, 0, reinterpret_cast<uint8_t *>(&entry), sizeof(entry)) != 0) {
		return false;
	}

	if (entry.magic != kRetainedMagic || entry.crc != RetainedCrc(entry)) {
		return false;
	}

	*unix_time = entry.unix_time;
	return true;
}

void OnSecondTick(const struct device *, void *)
{
	K_SPINLOCK(&g_lock) {
		g_unix_time++;
	}
}

} // namespace

int RtcClockInit(uint32_t initial_unix_time)
{
	if (!device_is_ready(kRtc)) {
		return -ENODEV;
	}

	uint32_t restored = 0;

	g_restored = LoadRetained(&restored);
	g_unix_time = g_restored ? restored : initial_unix_time;

	// RTC:n rÃ¤knar 32768 Hz frÃ¥n LFCLK; top-vÃ¤rdet gÃ¶r att den lÃ¶per Ã¶ver en gÃ¥ng per sekund.
	struct counter_top_cfg top = {};
	top.ticks = counter_get_frequency(kRtc);
	top.callback = OnSecondTick;

	int err = counter_set_top_value(kRtc, &top);
	if (err) {
		return err;
	}

	return counter_start(kRtc);
}

uint32_t RtcClockGet()
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);
	const uint32_t value = g_unix_time;

	k_spin_unlock(&g_lock, key);
	return value;
}

void RtcClockSet(uint32_t unix_time)
{
	K_SPINLOCK(&g_lock) {
		g_unix_time = unix_time;
	}
}

void RtcClockAdjust(int32_t delta_seconds)
{
	K_SPINLOCK(&g_lock) {
		g_unix_time += static_cast<uint32_t>(delta_seconds);
	}
}

bool RtcClockWasRestored()
{
	return g_restored;
}

void RtcClockPersist()
{
	RetainedTime entry = {};

	entry.magic = kRetainedMagic;
	entry.unix_time = RtcClockGet();
	entry.crc = RetainedCrc(entry);

	if (device_is_ready(kRetained)) {
		retained_mem_write(kRetained, 0, reinterpret_cast<const uint8_t *>(&entry),
				   sizeof(entry));
	}
}
