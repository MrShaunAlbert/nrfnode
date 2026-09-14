#include "controller.h"
#include <platform/CHIPDeviceLayer.h>
#include <platform/ThreadStackManager.h>
#include <inet/EndPointStateOpenThread.h>
#include <app/server/Server.h>
#include <app/MessageDef/StatusIB.h>
#include <clusters/RelativeHumidityMeasurement/Attributes.h>
#include <clusters/TemperatureMeasurement/Attributes.h>
#include <data-model-providers/codegen/Instance.h>
#include <controller/CHIPCluster.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/GroupDataProviderImpl.h>
#include <crypto/DefaultSessionKeystore.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/ThreadOperationalDataset.h>
#include <lib/support/TestGroupData.h>
#include <lib/address_resolve/AddressResolve.h>
#include <lib/address_resolve/AddressResolve_DefaultImpl.h>
#include <lib/dnssd/Resolver.h>
#include <lib/core/CHIPCallback.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/ip6.h>
#include <openthread/srp_server.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <platform/OpenThread/GenericThreadStackManagerImpl_OpenThread.h>
#include <zephyr/logging/log.h>

#include "ble_scan.h"
#include "devices.h"
#include "rtc_clock.h"

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::Controller;

namespace
{
DeviceCommissioner sCommissioner;
AutoCommissioner sAutoCommissioner;
ExampleOperationalCredentialsIssuer sOpCredsIssuer;
Crypto::DefaultSessionKeystore sSessionKeystore;
Crypto::P256Keypair sEphemeralKey;
uint8_t sNoc[kMaxCHIPDERCertLength];
uint8_t sIcac[kMaxCHIPDERCertLength];
uint8_t sRcac[kMaxCHIPDERCertLength];
} // namespace

namespace
{
void LockOt()
{
	DeviceLayer::ThreadStackMgr().LockThreadStack();
}
void UnlockOt()
{
	DeviceLayer::ThreadStackMgr().UnlockThreadStack();
}
Inet::EndPointStateOpenThread::OpenThreadEndpointInitParam sOtParams;

k_work_delayable sStartWork;
uint8_t sDatasetBuf[Thread::kSizeOperationalDataset];
size_t sDatasetLen;

// Prototyp: godkänner alla attestationsresultat tills IKEA:s PAA finns som förtroendeanker.
class IgnoreAttestation : public Credentials::DeviceAttestationDelegate {
public:
	Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override { return NullOptional; }
	bool ShouldWaitAfterDeviceAttestation() override { return true; }
	void OnDeviceAttestationCompleted(DeviceCommissioner *c, DeviceProxy *d,
					  const Credentials::DeviceAttestationVerifier::AttestationDeviceInfo &,
					  Credentials::AttestationVerificationResult r) override
	{
		LOG_WRN("Attestation result %u ignored", static_cast<unsigned>(r));
		c->ContinueCommissioningAfterDeviceAttestation(d, Credentials::AttestationVerificationResult::kSuccess);
	}
} sIgnoreAttestation;

class SensorReader : public DevicePairingDelegate
{
public:
	SensorReader() :
		mOnConnected(&OnConnected, this),
		mOnConnectionFailure(&OnConnectionFailure, this)
	{
	}

	void ConnectOrPair(NodeId deviceId)
	{
		CHIP_ERROR err = sCommissioner.GetConnectedDevice(
			deviceId, &mOnConnected, &mOnConnectionFailure);
		if (err != CHIP_NO_ERROR) {
			LOG_WRN("Existing sensor connection unavailable: %" CHIP_ERROR_FORMAT, err.Format());
			StartPairing(deviceId);
		}
	}

	void OnCommissioningComplete(NodeId deviceId, CHIP_ERROR error) override
	{
		LOG_INF("Commissioning complete callback node=0x%llx err=%" CHIP_ERROR_FORMAT,
			static_cast<unsigned long long>(deviceId), error.Format());
		if (error != CHIP_NO_ERROR || mSubscriptionsStarted) {
			return;
		}
		ConnectOrPair(deviceId);
	}

private:
	void StartPairing(NodeId deviceId)
	{
		if (mPairingAttempted) {
			LOG_ERR("Sensor connection failed after commissioning fallback");
			return;
		}
		const char *code = DeviceCode(0);
		if (code[0] == '\0') {
			LOG_ERR("No pairing code saved in device slot 1");
			return;
		}
		mPairingAttempted = true;
		CommissioningParameters cp;
		cp.SetThreadOperationalDataset(ByteSpan(sDatasetBuf, sDatasetLen));
		cp.SetDeviceAttestationDelegate(&sIgnoreAttestation);
		CHIP_ERROR err = sCommissioner.PairDevice(deviceId, code, cp);
		LOG_INF("PairDevice fallback: %" CHIP_ERROR_FORMAT, err.Format());
	}

	static void OnConnected(void *context, Messaging::ExchangeManager &exchangeMgr,
				const SessionHandle &sessionHandle)
	{
		auto *self = static_cast<SensorReader *>(context);
		if (self->mSubscriptionsStarted) {
			return;
		}
		self->mSubscriptionsStarted = true;
		ClusterBase cluster(exchangeMgr, sessionHandle, 1);
		CHIP_ERROR err = cluster.SubscribeAttribute<
			app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::TypeInfo>(
				self, OnTemperature, OnSubscriptionFailure, 0, 900,
				OnSubscriptionEstablished);
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("Temperature subscription request: %" CHIP_ERROR_FORMAT, err.Format());
		}
		ClusterBase humidityCluster(exchangeMgr, sessionHandle, 2);
		err = humidityCluster.SubscribeAttribute<
			app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::TypeInfo>(
				self, OnHumidity, OnSubscriptionFailure, 0, 900,
				OnSubscriptionEstablished);
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("Humidity subscription request: %" CHIP_ERROR_FORMAT, err.Format());
		}
	}

	static void OnConnectionFailure(void *context, const ScopedNodeId &peerId, CHIP_ERROR error)
	{
		LOG_ERR("Sensor CASE connection failed: %" CHIP_ERROR_FORMAT, error.Format());
		static_cast<SensorReader *>(context)->StartPairing(peerId.GetNodeId());
	}

	static void OnTemperature(
		void *, const app::DataModel::Nullable<int16_t> &value)
	{
		if (value.IsNull()) {
			LOG_WRN("Temperature attribute is null");
			return;
		}
		const uint32_t timestamp = RtcClockGet();
		SensorUpdateTemperature(value.Value(), timestamp);
		const int temperature = value.Value();
		const int magnitude = temperature < 0 ? -temperature : temperature;
		LOG_INF("SENSOR temperature=%s%d.%02dC timestamp=%u",
			temperature < 0 ? "-" : "", magnitude / 100, magnitude % 100,
			static_cast<unsigned>(timestamp));
	}

	static void OnHumidity(
		void *, const app::DataModel::Nullable<uint16_t> &value)
	{
		if (value.IsNull()) {
			LOG_WRN("Humidity attribute is null");
			return;
		}
		const uint32_t timestamp = RtcClockGet();
		SensorUpdateHumidity(value.Value(), timestamp);
		LOG_INF("SENSOR humidity=%u.%02u%% timestamp=%u",
			static_cast<unsigned>(value.Value() / 100),
			static_cast<unsigned>(value.Value() % 100),
			static_cast<unsigned>(timestamp));
	}

	static void OnSubscriptionEstablished(void *, SubscriptionId id)
	{
		LOG_INF("SENSOR subscription established id=%u",
			static_cast<unsigned>(id));
	}

	static void OnSubscriptionFailure(void *, CHIP_ERROR error)
	{
		if (error.IsIMStatus()) {
			const app::StatusIB status(error);
			LOG_ERR("Sensor subscription failed: IM status 0x%02x",
				static_cast<unsigned>(to_underlying(status.mStatus)));
		} else {
			LOG_ERR("Sensor subscription failed: %" CHIP_ERROR_FORMAT, error.Format());
		}
	}

	Callback::Callback<OnDeviceConnected> mOnConnected;
	Callback::Callback<OnDeviceConnectionFailure> mOnConnectionFailure;
	bool mSubscriptionsStarted = false;
	bool mPairingAttempted = false;
} sSensorReader;

CHIP_ERROR EnsureThreadDataset()
{
	otInstance *ot = DeviceLayer::ThreadStackMgrImpl().OTInstance();
	DeviceLayer::ThreadStackMgr().LockThreadStack();
	otOperationalDataset ds;
	otOperationalDatasetTlvs tlvs;
	if (otDatasetGetActive(ot, &ds) != OT_ERROR_NONE) {
		otDatasetCreateNewNetwork(ot, &ds);
		otDatasetConvertToTlvs(&ds, &tlvs);
		otDatasetSetActiveTlvs(ot, &tlvs);
	}
	otDatasetGetActiveTlvs(ot, &tlvs);
	if (!otIp6IsEnabled(ot)) {
		otIp6SetEnabled(ot, true);
	}
	if (otThreadGetDeviceRole(ot) == OT_DEVICE_ROLE_DISABLED) {
		otThreadSetEnabled(ot, true);
	}
	// No border router: this node is the SRP/DNS-SD server so that Matter operational discovery works.
	otSrpServerSetEnabled(ot, true);
	DeviceLayer::ThreadStackMgr().UnlockThreadStack();
	sDatasetLen = tlvs.mLength;
	memcpy(sDatasetBuf, tlvs.mTlvs, sDatasetLen);
	LOG_INF("Thread dataset %u bytes", static_cast<unsigned>(sDatasetLen));
	return CHIP_NO_ERROR;
}

k_work_delayable sInjectWork;
int sInjectCount;

// Devices that do not publish _matter._tcp until CommissioningComplete cannot be found through
// DNS-SD, so feed the resolver with the address from the device's SRP host registration.
void InjectResolveOnChipThread(intptr_t)
{
	otInstance *ot = DeviceLayer::ThreadStackMgrImpl().OTInstance();
	Inet::IPAddress addr;
	bool found = false;
	DeviceLayer::ThreadStackMgr().LockThreadStack();
	const otSrpServerHost *host = nullptr;
	while (!found && (host = otSrpServerGetNextHost(ot, host)) != nullptr)
	{
		if (otSrpServerHostIsDeleted(host))
			continue;
		uint8_t n = 0;
		const otIp6Address *a = otSrpServerHostGetAddresses(host, &n);
		for (uint8_t i = 0; i < n && !found; i++)
		{
			bool own = false;
			for (const otNetifAddress *u = otIp6GetUnicastAddresses(ot); u; u = u->mNext)
				own |= otIp6IsAddressEqual(&u->mAddress, &a[i]);
			if (!own)
			{
				addr = Inet::IPAddress::FromOtAddr(a[i]);
				found = true;
			}
		}
	}
	DeviceLayer::ThreadStackMgr().UnlockThreadStack();
	if (found)
	{
		Dnssd::ResolvedNodeData data;
		data.resolutionData.numIPs = 1;
		data.resolutionData.ipAddress[0] = addr;
		data.resolutionData.port = 5540;
		data.resolutionData.interfaceId = Inet::InterfaceId::Null();
		data.operationalData.peerId = PeerId(sCommissioner.GetCompressedFabricId(), 0x2001);
		static_cast<AddressResolve::Impl::Resolver &>(AddressResolve::Resolver::Instance())
			.OnOperationalNodeResolved(data);
	}
}

void InjectResolve(k_work *)
{
	DeviceLayer::PlatformMgr().ScheduleWork(InjectResolveOnChipThread, 0);
	if (++sInjectCount < 150)
		k_work_schedule(&sInjectWork, K_SECONDS(2));
}

void StartCommissioningOnChipThread(intptr_t)
{
	k_work_init_delayable(&sInjectWork, InjectResolve);
	k_work_schedule(&sInjectWork, K_SECONDS(10));
	sSensorReader.ConnectOrPair(0x2001);
}

void StartCommissioning(k_work *)
{
	DeviceLayer::PlatformMgr().ScheduleWork(StartCommissioningOnChipThread, 0);
}
} // namespace

CHIP_ERROR ControllerInit()
{
	EnsureThreadDataset();
	auto &server = Server::GetInstance();
	sOtParams.lockCb = LockOt;
	sOtParams.unlockCb = UnlockOt;
	sOtParams.openThreadInstancePtr = DeviceLayer::ThreadStackMgrImpl().OTInstance();
	FactoryInitParams factoryParams;
	SetupParams params;

	factoryParams.fabricIndependentStorage = &server.GetPersistentStorage();
	factoryParams.fabricTable = &server.GetFabricTable();
	factoryParams.sessionKeystore = &sSessionKeystore;
	factoryParams.groupDataProvider = Credentials::GetGroupDataProvider();
	factoryParams.enableServerInteractions = false;
	factoryParams.endpointNativeParams = &sOtParams;
	factoryParams.dataModelProvider = app::CodegenDataModelProviderInstance(&server.GetPersistentStorage());

	params.operationalCredentialsDelegate = &sOpCredsIssuer;
	params.controllerVendorId = VendorId::TestVendor1;
	ReturnErrorOnFailure(sOpCredsIssuer.Initialize(server.GetPersistentStorage()));

	// Testrot tills IKEA:s PAA lagts till som fÃ¶rtroendeanker.
	auto *verifier = Credentials::GetDefaultDACVerifier(Credentials::GetTestAttestationTrustStore(), nullptr);
	VerifyOrReturnError(verifier != nullptr, CHIP_ERROR_NO_MEMORY);
	Credentials::SetDeviceAttestationVerifier(verifier);

	MutableByteSpan noc(sNoc), icac(sIcac), rcac(sRcac);
	ReturnErrorOnFailure(sEphemeralKey.Initialize(Crypto::ECPKeyTarget::ECDSA));
	ReturnErrorOnFailure(sOpCredsIssuer.GenerateNOCChainAfterValidation(
		0x1000, 1, kUndefinedCATs, sEphemeralKey.Pubkey(), rcac, icac, noc));

	params.operationalKeypair = &sEphemeralKey;
	params.controllerRCAC = rcac;
	params.controllerICAC = icac;
	params.controllerNOC = noc;
	params.defaultCommissioner = &sAutoCommissioner;
	params.pairingDelegate = &sSensorReader;
	params.enableServerInteractions = true;

	auto &factory = DeviceControllerFactory::GetInstance();
	ReturnErrorOnFailure(factory.Init(factoryParams));
	ReturnErrorOnFailure(factory.SetupCommissioner(params, sCommissioner));

	LOG_INF("Commissioner ready, fabric index %u", static_cast<unsigned>(sCommissioner.GetFabricIndex()));
	{
		// The controller's own fabric needs the IPK (epoch key) in the group data provider for CASE.
		uint8_t cfid[sizeof(uint64_t)] = {0};
		MutableByteSpan cfidSpan(cfid);
		ReturnErrorOnFailure(sCommissioner.GetCompressedFabricIdBytes(cfidSpan));
		ReturnErrorOnFailure(Credentials::SetSingleIpkEpochKey(
			Credentials::GetGroupDataProvider(), sCommissioner.GetFabricIndex(),
			GroupTesting::DefaultIpkValue::GetDefaultIpk(), cfidSpan));
	}
	BleScanStart();
	EnsureThreadDataset();
	k_work_init_delayable(&sStartWork, StartCommissioning);
	// Thread-ledaren behöver ca 25 s för att bildas innan enheten ska ansluta.
	k_work_schedule(&sStartWork, K_SECONDS(40));
	return CHIP_NO_ERROR;
}

CHIP_ERROR ControllerCommission(const char *manual_code, uint64_t node_id)
{
	return sCommissioner.PairDevice(node_id, manual_code);
}
