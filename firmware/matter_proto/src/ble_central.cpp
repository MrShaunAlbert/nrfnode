#include "ble_central.h"
#include "ble_scan.h"

#include <ble/Ble.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemPacketBuffer.h>

#include <bluetooth/gatt_dm.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::Ble;

namespace
{
#define UUID_C1 BT_UUID_128_ENCODE(0x18EE2EF5, 0x263D, 0x4559, 0x959F, 0x4F9C429F9D11)
#define UUID_C2 BT_UUID_128_ENCODE(0x18EE2EF5, 0x263D, 0x4559, 0x959F, 0x4F9C429F9D12)

struct bt_uuid_16 sSvcUuid = BT_UUID_INIT_16(0xFFF6);
struct bt_uuid_128 sC1Uuid = BT_UUID_INIT_128(UUID_C1);
struct bt_uuid_128 sC2Uuid = BT_UUID_INIT_128(UUID_C2);

class CentralDelegate : public BleConnectionDelegate
{
public:
	void NewConnection(BleLayer *layer, void *appState, const SetupDiscriminator &disc) override;
	void NewConnection(BleLayer *layer, void *appState, BLE_CONNECTION_OBJECT connObj) override {}
	CHIP_ERROR CancelConnection() override;
};

CentralDelegate sDelegate;
BleLayer *sLayer;
void *sAppState;
SetupDiscriminator sWanted;
bool sScanning;
bt_conn *sConn;
uint16_t sC1Handle, sC2Handle, sCccHandle;
bt_gatt_subscribe_params sSubParams;
bt_gatt_write_params sWriteParams;
System::PacketBufferHandle sWriteBuf;

// BLE callbacks run on small Zephyr work queues; Matter processing (PASE crypto) must run on the Matter thread.
void Post(void (*fn)(intptr_t), intptr_t arg)
{
	DeviceLayer::PlatformMgr().ScheduleWork(fn, arg);
}

void ReportErrorOnChipThread(intptr_t code)
{
	CHIP_ERROR err(static_cast<uint32_t>(code));
	LOG_ERR("BLE central error: %" CHIP_ERROR_FORMAT, err.Format());
	if (sDelegate.OnConnectionError) {
		sDelegate.OnConnectionError(sAppState, err);
	}
}

void ReportError(CHIP_ERROR err)
{
	Post(ReportErrorOnChipThread, static_cast<intptr_t>(err.AsInteger()));
}

void ConnectionCompleteOnChipThread(intptr_t)
{
	if (sDelegate.OnConnectionComplete && sConn) {
		sDelegate.OnConnectionComplete(sAppState, sConn);
	}
}

void DmCompleted(struct bt_gatt_dm *dm, void *)
{
	const bt_gatt_dm_attr *c1 = bt_gatt_dm_char_by_uuid(dm, &sC1Uuid.uuid);
	const bt_gatt_dm_attr *c2 = bt_gatt_dm_char_by_uuid(dm, &sC2Uuid.uuid);
	const bt_gatt_dm_attr *ccc = c2 ? bt_gatt_dm_desc_by_uuid(dm, c2, BT_UUID_GATT_CCC) : nullptr;
	if (!c1 || !c2 || !ccc) {
		bt_gatt_dm_data_release(dm);
		ReportError(CHIP_ERROR_INCORRECT_STATE);
		return;
	}
	sC1Handle = bt_gatt_dm_attr_chrc_val(c1)->value_handle;
	sC2Handle = bt_gatt_dm_attr_chrc_val(c2)->value_handle;
	sCccHandle = ccc->handle;
	bt_gatt_dm_data_release(dm);
	LOG_INF("GATT found: C1 %u C2 %u CCC %u", sC1Handle, sC2Handle, sCccHandle);
	Post(ConnectionCompleteOnChipThread, 0);
}

void DmNotFound(struct bt_conn *, void *)
{
	ReportError(CHIP_ERROR_NOT_FOUND);
}

void DmError(struct bt_conn *, int err, void *)
{
	ReportError(CHIP_ERROR_INTERNAL);
}

const bt_gatt_dm_cb sDmCb = {DmCompleted, DmNotFound, DmError};

void OnConnected(struct bt_conn *conn, uint8_t err)
{
	if (conn != sConn) {
		return;
	}
	if (err) {
		LOG_ERR("BLE central connect failed 0x%02x", err);
		bt_conn_unref(sConn);
		sConn = nullptr;
		ReportError(CHIP_ERROR_INTERNAL);
		return;
	}
	LOG_INF("BLE central connected");
	int rc = bt_gatt_dm_start(conn, &sSvcUuid.uuid, &sDmCb, nullptr);
	if (rc) {
		LOG_ERR("bt_gatt_dm_start %d", rc);
		ReportError(CHIP_ERROR_INTERNAL);
	}
}

void DisconnectedOnChipThread(intptr_t conn)
{
	if (sLayer) {
		sLayer->HandleConnectionError(reinterpret_cast<bt_conn *>(conn), BLE_ERROR_REMOTE_DEVICE_DISCONNECTED);
	}
}

void OnDisconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != sConn) {
		return;
	}
	LOG_INF("BLE central disconnected 0x%02x", reason);
	bt_conn_unref(sConn);
	sConn = nullptr;
	Post(DisconnectedOnChipThread, reinterpret_cast<intptr_t>(conn));
}

BT_CONN_CB_DEFINE(sConnCb) = {.connected = OnConnected, .disconnected = OnDisconnected};

void OnMatterDevice(const bt_addr_le_t *addr, uint16_t disc)
{
	if (!sScanning || !sWanted.MatchesLongDiscriminator(disc)) {
		return;
	}
	sScanning = false;
	bt_le_scan_stop();
	LOG_INF("BLE central: connecting to discriminator %u", disc);
	int rc = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &sConn);
	if (rc) {
		LOG_ERR("bt_conn_le_create %d", rc);
		ReportError(CHIP_ERROR_INTERNAL);
	}
}

void IndicationOnChipThread(intptr_t raw)
{
	auto buf = System::PacketBufferHandle::Adopt(reinterpret_cast<System::PacketBuffer *>(raw));
	if (sLayer && sConn) {
		sLayer->HandleIndicationReceived(sConn, &CHIP_BLE_SVC_ID, &CHIP_BLE_CHAR_2_UUID, std::move(buf));
	}
}

uint8_t OnIndication(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data, uint16_t length)
{
	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	auto buf = System::PacketBufferHandle::NewWithData(data, length);
	if (!buf.IsNull()) {
		Post(IndicationOnChipThread, reinterpret_cast<intptr_t>(std::move(buf).UnsafeRelease()));
	}
	return BT_GATT_ITER_CONTINUE;
}

void SubscribedOnChipThread(intptr_t arg)
{
	uint8_t err = arg & 0xFF;
	bool subscribed = arg & 0x100;
	if (!sLayer || !sConn) {
		return;
	}
	if (err) {
		LOG_ERR("Subscribe error %u", err);
		sLayer->HandleConnectionError(sConn, BLE_ERROR_GATT_SUBSCRIBE_FAILED);
	} else if (subscribed) {
		sLayer->HandleSubscribeComplete(sConn, &CHIP_BLE_SVC_ID, &CHIP_BLE_CHAR_2_UUID);
	} else {
		sLayer->HandleUnsubscribeComplete(sConn, &CHIP_BLE_SVC_ID, &CHIP_BLE_CHAR_2_UUID);
	}
}

void OnSubscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params)
{
	Post(SubscribedOnChipThread, err | (params->value ? 0x100 : 0));
}

void WrittenOnChipThread(intptr_t err)
{
	sWriteBuf = nullptr;
	if (!sLayer || !sConn) {
		return;
	}
	if (err) {
		LOG_ERR("Write error %d", static_cast<int>(err));
		sLayer->HandleConnectionError(sConn, BLE_ERROR_GATT_WRITE_FAILED);
		return;
	}
	sLayer->HandleWriteConfirmation(sConn, &CHIP_BLE_SVC_ID, &CHIP_BLE_CHAR_1_UUID);
}

void OnWritten(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	Post(WrittenOnChipThread, err);
}

void CentralDelegate::NewConnection(BleLayer *layer, void *appState, const SetupDiscriminator &disc)
{
	sLayer = layer;
	sAppState = appState;
	sWanted = disc;
	sScanning = true;
	LOG_INF("BLE central: looking for discriminator %u%s", disc.IsShortDiscriminator() ? disc.GetShortValue() : disc.GetLongValue(),
		disc.IsShortDiscriminator() ? " (short)" : "");
	BleScanStart();
}

CHIP_ERROR CentralDelegate::CancelConnection()
{
	sScanning = false;
	if (sConn) {
		bt_conn_disconnect(sConn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	return CHIP_NO_ERROR;
}
} // namespace

void BleCentralOnMatterAdv(const bt_addr_le_t *addr, uint16_t discriminator)
{
	OnMatterDevice(addr, discriminator);
}

BleConnectionDelegate *ChipBleCentralConnectionDelegate(BleLayer *layer)
{
	sLayer = layer;
	return &sDelegate;
}

CHIP_ERROR ChipBleCentralSubscribe(bt_conn *conn, bool subscribe)
{
	if (subscribe) {
		memset(&sSubParams, 0, sizeof(sSubParams));
		sSubParams.notify = OnIndication;
		sSubParams.subscribe = OnSubscribed;
		sSubParams.value = BT_GATT_CCC_INDICATE;
		sSubParams.value_handle = sC2Handle;
		sSubParams.ccc_handle = sCccHandle;
		int rc = bt_gatt_subscribe(conn, &sSubParams);
		return rc ? CHIP_ERROR_INTERNAL : CHIP_NO_ERROR;
	}
	int rc = bt_gatt_unsubscribe(conn, &sSubParams);
	return rc ? CHIP_ERROR_INTERNAL : CHIP_NO_ERROR;
}

CHIP_ERROR ChipBleCentralWrite(bt_conn *conn, System::PacketBufferHandle pBuf)
{
	sWriteBuf = std::move(pBuf);
	memset(&sWriteParams, 0, sizeof(sWriteParams));
	sWriteParams.func = OnWritten;
	sWriteParams.handle = sC1Handle;
	sWriteParams.data = sWriteBuf->Start();
	sWriteParams.length = sWriteBuf->DataLength();
	int rc = bt_gatt_write(conn, &sWriteParams);
	return rc ? CHIP_ERROR_INTERNAL : CHIP_NO_ERROR;
}
