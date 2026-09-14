#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "controller.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"
#include "storage.h"

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::DeviceLayer;

namespace {

constexpr size_t kUiStackSize = 4096;
K_THREAD_STACK_DEFINE(sUiStack, kUiStackSize);
struct k_thread sUiThread;

void UiThread(void *, void *, void *)
{
	const int err = UiTaskMain();
	LOG_ERR("OLED UI exited with error %d", err);
}

} // namespace

CHIP_ERROR AppTask::Init()
{
	const int storageErr = StorageInit();
	if (storageErr) {
		LOG_ERR("Settings initialization failed (err %d)", storageErr);
		return CHIP_ERROR_INCORRECT_STATE;
	}

	Nrf::Matter::InitData initData;

	// Controllern initieras i Matter-tråden direkt efter att servern startat.
	initData.mPostServerInitClbk = ControllerInit;

	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(initData));

	if (!Nrf::GetBoard().Init()) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());
	k_thread_create(&sUiThread, sUiStack, K_THREAD_STACK_SIZEOF(sUiStack), UiThread,
			nullptr, nullptr, nullptr, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
