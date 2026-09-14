#include "button_handler.h"

#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(button_handler, LOG_LEVEL_INF);

namespace {
ButtonStateChangedCallback g_callback;
uint8_t g_state;
struct button_handler g_button_handler = {};

void OnButtonChanged(uint32_t button_state, uint32_t has_changed)
{
	// dk_buttons_and_leds mappar knapp N till bit N, i linje med
	// pan1780_evb-aliasen sw0..sw3 och led0..led3.
	g_state = static_cast<uint8_t>(button_state & 0x0F);

	const int err = dk_set_leds(g_state);
	if (err) {
		LOG_ERR("dk_set_leds failed (err %d)", err);
	}

	if (g_callback && (has_changed & 0x0F)) {
		g_callback(g_state);
	}
}
} // namespace

int ButtonHandlerInit(ButtonStateChangedCallback callback)
{
	g_callback = callback;

	int err = dk_leds_init();
	if (err) {
		return err;
	}

	err = dk_set_leds(0);
	if (err) {
		return err;
	}

	return dk_buttons_init(OnButtonChanged);
}

int ButtonHandlerAttach(ButtonStateChangedCallback callback)
{
	g_callback = callback;
	g_button_handler.cb = OnButtonChanged;
	dk_button_handler_add(&g_button_handler);

	int err = dk_leds_init();
	if (err) {
		LOG_ERR("dk_leds_init failed (err %d)", err);
		return err;
	}

	err = dk_set_leds(0);
	if (err) {
		LOG_ERR("dk_set_leds failed (err %d)", err);
	}

	return err;
}

uint8_t ButtonHandlerGetState()
{
	return g_state;
}
