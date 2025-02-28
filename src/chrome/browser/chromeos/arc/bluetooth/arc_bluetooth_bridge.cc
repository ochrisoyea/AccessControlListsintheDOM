// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/bluetooth/arc_bluetooth_bridge.h"

#include <bluetooth/bluetooth.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>

#include <iomanip>
#include <string>
#include <utility>

#include "ash/public/cpp/ash_pref_names.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/containers/queue.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/singleton.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "components/arc/arc_bridge_service.h"
#include "components/arc/arc_browser_context_keyed_service_factory_base.h"
#include "components/arc/bluetooth/bluetooth_type_converters.h"
#include "components/arc/intent_helper/arc_intent_helper_bridge.h"
#include "components/device_event_log/device_event_log.h"
#include "components/prefs/pref_service.h"
#include "components/user_manager/user_manager.h"
#include "device/bluetooth/bluetooth_common.h"
#include "device/bluetooth/bluetooth_device.h"
#include "device/bluetooth/bluetooth_gatt_connection.h"
#include "device/bluetooth/bluetooth_gatt_notify_session.h"
#include "device/bluetooth/bluetooth_local_gatt_characteristic.h"
#include "device/bluetooth/bluetooth_local_gatt_descriptor.h"
#include "device/bluetooth/bluez/bluetooth_device_bluez.h"
#include "mojo/edk/embedder/embedder.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"

using device::BluetoothAdapter;
using device::BluetoothAdapterFactory;
using device::BluetoothAdvertisement;
using device::BluetoothDevice;
using device::BluetoothDiscoveryFilter;
using device::BluetoothDiscoverySession;
using device::BluetoothGattConnection;
using device::BluetoothGattNotifySession;
using device::BluetoothGattCharacteristic;
using device::BluetoothGattDescriptor;
using device::BluetoothGattService;
using device::BluetoothLocalGattCharacteristic;
using device::BluetoothLocalGattDescriptor;
using device::BluetoothLocalGattService;
using device::BluetoothRemoteGattCharacteristic;
using device::BluetoothRemoteGattDescriptor;
using device::BluetoothRemoteGattService;
using device::BluetoothTransport;
using device::BluetoothUUID;

namespace {
constexpr uint32_t kGattReadPermission =
    BluetoothGattCharacteristic::Permission::PERMISSION_READ |
    BluetoothGattCharacteristic::Permission::PERMISSION_READ_ENCRYPTED |
    BluetoothGattCharacteristic::Permission::
        PERMISSION_READ_ENCRYPTED_AUTHENTICATED;
constexpr uint32_t kGattWritePermission =
    BluetoothGattCharacteristic::Permission::PERMISSION_WRITE |
    BluetoothGattCharacteristic::Permission::PERMISSION_WRITE_ENCRYPTED |
    BluetoothGattCharacteristic::Permission::
        PERMISSION_WRITE_ENCRYPTED_AUTHENTICATED;
constexpr int32_t kInvalidGattAttributeHandle = -1;
constexpr int32_t kInvalidAdvertisementHandle = -1;
// Bluetooth Specification Version 4.2 Vol 3 Part F Section 3.2.2
// An attribute handle of value 0xFFFF is known as the maximum attribute handle.
constexpr int32_t kMaxGattAttributeHandle = 0xFFFF;
// Bluetooth Specification Version 4.2 Vol 3 Part F Section 3.2.9
// The maximum length of an attribute value shall be 512 octets.
constexpr int kMaxGattAttributeLength = 512;
// Copied from Android at system/bt/stack/btm/btm_ble_int.h
// https://goo.gl/k7PM6u
constexpr uint16_t kAndroidMBluetoothVersionNumber = 95;
// Bluetooth SDP Service Class ID List Attribute identifier
constexpr uint16_t kServiceClassIDListAttributeID = 0x0001;
// Timeout for Bluetooth Discovery (scan)
// 120 seconds is used here as the upper bound of the time need to do device
// discovery once, 20 seconds for inquiry scan and 100 seconds for page scan
// for 100 new devices.
constexpr base::TimeDelta kDiscoveryTimeout = base::TimeDelta::FromSeconds(120);
// From https://www.bluetooth.com/specifications/assigned-numbers/baseband
// The Class of Device for generic computer.
constexpr uint32_t kBluetoothComputerClass = 0x100;
// Timeout for Android to complete a disabling op to adapter.
// In the case where an enabling op happens immediately after a disabling op,
// Android takes the following enabling op as a no-op and waits 3~4 seconds for
// the previous disabling op to finish, so the enabling op will never be
// fulfilled by Android, and the disabling op will later routed back to Chrome
// while Chrome's adapter is enabled. This results in the wrong power state
// which should be enabled. Since the signaling from Android to Chrome for
// Bluetooth is via Bluetooth HAL layer which run on the same process as
// Bluetooth Service in Java space, so the signaling to Chrome about the
// to-be-happen sleep cannot be done. This timeout tries to ensure the validity
// and the order of toggles on power state sent to Android.
// If Android takes more than 5 seconds to complete the intent initiated by
// Chrome, Chrome will take EnableAdapter/DisableAdapter calls as a request from
// Android to toggle the power state. The power state will be synced on both
// Chrome and Android, but as a result, Bluetooth will be off.
constexpr base::TimeDelta kPowerIntentTimeout = base::TimeDelta::FromSeconds(5);

using GattReadCallback =
    base::OnceCallback<void(arc::mojom::BluetoothGattValuePtr)>;
using CreateSdpRecordCallback =
    base::OnceCallback<void(arc::mojom::BluetoothCreateSdpRecordResultPtr)>;
using RemoveSdpRecordCallback =
    base::OnceCallback<void(arc::mojom::BluetoothStatus)>;

arc::mojom::BluetoothGattStatus ConvertGattErrorCodeToStatus(
    const device::BluetoothGattService::GattErrorCode& error_code,
    bool is_read_operation) {
  switch (error_code) {
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_INVALID_LENGTH:
      return arc::mojom::BluetoothGattStatus::GATT_INVALID_ATTRIBUTE_LENGTH;
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_NOT_PERMITTED:
      return is_read_operation
                 ? arc::mojom::BluetoothGattStatus::GATT_READ_NOT_PERMITTED
                 : arc::mojom::BluetoothGattStatus::GATT_WRITE_NOT_PERMITTED;
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_NOT_AUTHORIZED:
      return arc::mojom::BluetoothGattStatus::GATT_INSUFFICIENT_AUTHENTICATION;
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_NOT_SUPPORTED:
      return arc::mojom::BluetoothGattStatus::GATT_REQUEST_NOT_SUPPORTED;
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_UNKNOWN:
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_FAILED:
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_IN_PROGRESS:
    case device::BluetoothGattService::GattErrorCode::GATT_ERROR_NOT_PAIRED:
    default:
      return arc::mojom::BluetoothGattStatus::GATT_FAILURE;
  }
}

// Example of identifier: /org/bluez/hci0/dev_E0_CF_65_8C_86_1A/service001a
// Convert the last 4 characters of |identifier| to an
// int, by interpreting them as hexadecimal digits.
int ConvertGattIdentifierToId(const std::string identifier) {
  return std::stoi(identifier.substr(identifier.size() - 4), nullptr, 16);
}

// Create GattDBElement and fill in common data for
// Gatt Service/Characteristic/Descriptor.
template <class RemoteGattAttribute>
arc::mojom::BluetoothGattDBElementPtr CreateGattDBElement(
    const arc::mojom::BluetoothGattDBAttributeType type,
    const RemoteGattAttribute* attribute) {
  arc::mojom::BluetoothGattDBElementPtr element =
      arc::mojom::BluetoothGattDBElement::New();
  element->type = type;
  element->uuid = attribute->GetUUID();
  element->id = element->attribute_handle = element->start_handle =
      element->end_handle =
          ConvertGattIdentifierToId(attribute->GetIdentifier());
  element->properties = 0;
  return element;
}

template <class RemoteGattAttribute>
RemoteGattAttribute* FindGattAttributeByUuid(
    const std::vector<RemoteGattAttribute*>& attributes,
    const BluetoothUUID& uuid) {
  auto it = std::find_if(
      attributes.begin(), attributes.end(),
      [uuid](RemoteGattAttribute* attr) { return attr->GetUUID() == uuid; });
  return it != attributes.end() ? *it : nullptr;
}

// Common success callback for GATT operations that only need to report
// GattStatus back to Android.
void OnGattOperationDone(arc::ArcBluetoothBridge::GattStatusCallback callback) {
  std::move(callback).Run(arc::mojom::BluetoothGattStatus::GATT_SUCCESS);
}

// Common error callback for GATT operations that only need to report
// GattStatus back to Android.
void OnGattOperationError(arc::ArcBluetoothBridge::GattStatusCallback callback,
                          BluetoothGattService::GattErrorCode error_code) {
  std::move(callback).Run(ConvertGattErrorCodeToStatus(
      error_code, /* is_read_operation = */ false));
}

// Common success callback for ReadGattCharacteristic and ReadGattDescriptor
void OnGattReadDone(GattReadCallback callback,
                    const std::vector<uint8_t>& result) {
  arc::mojom::BluetoothGattValuePtr gattValue =
      arc::mojom::BluetoothGattValue::New();
  gattValue->status = arc::mojom::BluetoothGattStatus::GATT_SUCCESS;
  gattValue->value = result;
  std::move(callback).Run(std::move(gattValue));
}

// Common error callback for ReadGattCharacteristic and ReadGattDescriptor
void OnGattReadError(GattReadCallback callback,
                     BluetoothGattService::GattErrorCode error_code) {
  arc::mojom::BluetoothGattValuePtr gattValue =
      arc::mojom::BluetoothGattValue::New();
  gattValue->status =
      ConvertGattErrorCodeToStatus(error_code, /* is_read_operation = */ true);
  std::move(callback).Run(std::move(gattValue));
}

// Callback function for mojom::BluetoothInstance::RequestGattRead
void OnGattServerRead(
    const BluetoothLocalGattService::Delegate::ValueCallback& success_callback,
    const BluetoothLocalGattService::Delegate::ErrorCallback& error_callback,
    arc::mojom::BluetoothGattStatus status,
    const std::vector<uint8_t>& value) {
  if (status == arc::mojom::BluetoothGattStatus::GATT_SUCCESS)
    success_callback.Run(value);
  else
    error_callback.Run();
}

// Callback function for mojom::BluetoothInstance::RequestGattWrite
void OnGattServerWrite(
    const base::Closure& success_callback,
    const BluetoothLocalGattService::Delegate::ErrorCallback& error_callback,
    arc::mojom::BluetoothGattStatus status) {
  if (status == arc::mojom::BluetoothGattStatus::GATT_SUCCESS)
    success_callback.Run();
  else
    error_callback.Run();
}

bool IsGattOffsetValid(int offset) {
  return 0 <= offset && offset < kMaxGattAttributeLength;
}

// This is needed because Android only support UUID 16 bits in service data
// section in advertising data
uint16_t GetUUID16(const BluetoothUUID& uuid) {
  // Convert xxxxyyyy-xxxx-xxxx-xxxx-xxxxxxxxxxxx to int16 yyyy
  return std::stoi(uuid.canonical_value().substr(4, 4), nullptr, 16);
}

arc::mojom::BluetoothPropertyPtr GetDiscoveryTimeoutProperty(uint32_t timeout) {
  arc::mojom::BluetoothPropertyPtr property =
      arc::mojom::BluetoothProperty::New();
  property->set_discovery_timeout(timeout);
  return property;
}

void OnCreateServiceRecordDone(CreateSdpRecordCallback callback,
                               uint32_t service_handle) {
  arc::mojom::BluetoothCreateSdpRecordResultPtr result =
      arc::mojom::BluetoothCreateSdpRecordResult::New();
  result->status = arc::mojom::BluetoothStatus::SUCCESS;
  result->service_handle = service_handle;

  std::move(callback).Run(std::move(result));
}

void OnCreateServiceRecordError(
    CreateSdpRecordCallback callback,
    bluez::BluetoothServiceRecordBlueZ::ErrorCode error_code) {
  arc::mojom::BluetoothCreateSdpRecordResultPtr result =
      arc::mojom::BluetoothCreateSdpRecordResult::New();
  if (error_code ==
      bluez::BluetoothServiceRecordBlueZ::ErrorCode::ERROR_ADAPTER_NOT_READY) {
    result->status = arc::mojom::BluetoothStatus::NOT_READY;
  } else {
    result->status = arc::mojom::BluetoothStatus::FAIL;
  }

  std::move(callback).Run(std::move(result));
}

void OnRemoveServiceRecordDone(RemoveSdpRecordCallback callback) {
  std::move(callback).Run(arc::mojom::BluetoothStatus::SUCCESS);
}

void OnRemoveServiceRecordError(
    RemoveSdpRecordCallback callback,
    bluez::BluetoothServiceRecordBlueZ::ErrorCode error_code) {
  arc::mojom::BluetoothStatus status;
  if (error_code ==
      bluez::BluetoothServiceRecordBlueZ::ErrorCode::ERROR_ADAPTER_NOT_READY)
    status = arc::mojom::BluetoothStatus::NOT_READY;
  else
    status = arc::mojom::BluetoothStatus::FAIL;

  std::move(callback).Run(status);
}

}  // namespace

namespace arc {
namespace {

// Singleton factory for ArcAccessibilityHelperBridge.
class ArcBluetoothBridgeFactory
    : public internal::ArcBrowserContextKeyedServiceFactoryBase<
          ArcBluetoothBridge,
          ArcBluetoothBridgeFactory> {
 public:
  // Factory name used by ArcBrowserContextKeyedServiceFactoryBase.
  static constexpr const char* kName = "ArcBluetoothBridgeFactory";

  static ArcBluetoothBridgeFactory* GetInstance() {
    return base::Singleton<ArcBluetoothBridgeFactory>::get();
  }

 private:
  friend base::DefaultSingletonTraits<ArcBluetoothBridgeFactory>;
  ArcBluetoothBridgeFactory() = default;
  ~ArcBluetoothBridgeFactory() override = default;
};

}  // namespace

// static
ArcBluetoothBridge* ArcBluetoothBridge::GetForBrowserContext(
    content::BrowserContext* context) {
  return ArcBluetoothBridgeFactory::GetForBrowserContext(context);
}

template <typename InstanceType, typename HostType>
class ArcBluetoothBridge::ConnectionObserverImpl
    : public ConnectionObserver<InstanceType> {
 public:
  ConnectionObserverImpl(ArcBluetoothBridge* owner,
                         ArcBridgeService* arc_bridge_service)
      : owner_(owner), arc_bridge_service_(arc_bridge_service) {
    GetHolder()->AddObserver(this);
  }

  ~ConnectionObserverImpl() override { GetHolder()->RemoveObserver(this); }

 protected:
  ConnectionHolder<InstanceType, HostType>* GetHolder();

  ArcBridgeService* arc_bridge_service() { return arc_bridge_service_; }

 private:
  // ConnectionObserver<T>:
  void OnConnectionReady() override { owner_->MaybeSendInitialPowerChange(); }

  // Unowned pointer
  ArcBluetoothBridge* const owner_;
  ArcBridgeService* const arc_bridge_service_;

  DISALLOW_COPY_AND_ASSIGN(ConnectionObserverImpl);
};

template <>
ConnectionHolder<mojom::AppInstance, mojom::AppHost>*
ArcBluetoothBridge::ConnectionObserverImpl<mojom::AppInstance,
                                           mojom::AppHost>::GetHolder() {
  return arc_bridge_service()->app();
}

template <>
ConnectionHolder<mojom::IntentHelperInstance, mojom::IntentHelperHost>*
ArcBluetoothBridge::ConnectionObserverImpl<
    mojom::IntentHelperInstance,
    mojom::IntentHelperHost>::GetHolder() {
  return arc_bridge_service()->intent_helper();
}

ArcBluetoothBridge::ArcBluetoothBridge(content::BrowserContext* context,
                                       ArcBridgeService* bridge_service)
    : arc_bridge_service_(bridge_service), weak_factory_(this) {
  arc_bridge_service_->bluetooth()->SetHost(this);
  arc_bridge_service_->bluetooth()->AddObserver(this);

  app_observer_ = std::make_unique<
      ConnectionObserverImpl<mojom::AppInstance, mojom::AppHost>>(
      this, arc_bridge_service_);
  intent_helper_observer_ =
      std::make_unique<ConnectionObserverImpl<mojom::IntentHelperInstance,
                                              mojom::IntentHelperHost>>(
          this, arc_bridge_service_);

  if (BluetoothAdapterFactory::IsBluetoothSupported()) {
    VLOG(1) << "Registering bluetooth adapter.";
    BluetoothAdapterFactory::GetAdapter(base::Bind(
        &ArcBluetoothBridge::OnAdapterInitialized, weak_factory_.GetWeakPtr()));
  } else {
    VLOG(1) << "Bluetooth not supported.";
  }
}

ArcBluetoothBridge::~ArcBluetoothBridge() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (bluetooth_adapter_)
    bluetooth_adapter_->RemoveObserver(this);

  arc_bridge_service_->bluetooth()->RemoveObserver(this);
  arc_bridge_service_->bluetooth()->SetHost(nullptr);
}

void ArcBluetoothBridge::OnAdapterInitialized(
    scoped_refptr<BluetoothAdapter> adapter) {
  DCHECK(adapter);
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // We can downcast here because we are always running on Chrome OS, and
  // so our adapter uses BlueZ.
  bluetooth_adapter_ =
      static_cast<bluez::BluetoothAdapterBlueZ*>(adapter.get());

  if (!bluetooth_adapter_->HasObserver(this))
    bluetooth_adapter_->AddObserver(this);
}

void ArcBluetoothBridge::OnConnectionReady() {
  // TODO(hidehiko): Replace this by ConnectionHolder::IsConnected().
  is_bluetooth_instance_up_ = true;
}

void ArcBluetoothBridge::OnConnectionClosed() {
  is_bluetooth_instance_up_ = false;
}

void ArcBluetoothBridge::SendDevice(const BluetoothDevice* device) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnDeviceFound);
  if (!bluetooth_instance)
    return;

  std::vector<mojom::BluetoothPropertyPtr> properties =
      GetDeviceProperties(mojom::BluetoothPropertyType::ALL, device);

  bluetooth_instance->OnDeviceFound(std::move(properties));

  if (!(device->GetType() & device::BLUETOOTH_TRANSPORT_LE))
    return;

  base::Optional<int8_t> rssi = device->GetInquiryRSSI();
  mojom::BluetoothAddressPtr addr;

  // We only want to send updated advertise data to Android only when we are
  // scanning which is checked by the validity of rssi. Here are the 2 cases
  // that we don't want to send updated advertise data to Android.
  // 1) Cached found device and 2) rssi became invalid when we stop scanning.
  if (rssi.has_value()) {
    auto* btle_instance = ARC_GET_INSTANCE_FOR_METHOD(
        arc_bridge_service_->bluetooth(), OnLEDeviceFound);
    if (!btle_instance)
      return;
    std::vector<mojom::BluetoothAdvertisingDataPtr> adv_data =
        GetAdvertisingData(device);
    addr = mojom::BluetoothAddress::From(device->GetAddress());
    btle_instance->OnLEDeviceFound(std::move(addr), rssi.value(),
                                   std::move(adv_data));
  }
}

void ArcBluetoothBridge::AdapterPoweredChanged(BluetoothAdapter* adapter,
                                               bool powered) {
  AdapterPowerState power_change =
      powered ? AdapterPowerState::TURN_ON : AdapterPowerState::TURN_OFF;
  if (IsPowerChangeInitiatedByRemote(power_change))
    DequeueRemotePowerChange(power_change);
  else
    EnqueueLocalPowerChange(power_change);
}

void ArcBluetoothBridge::DeviceAdded(BluetoothAdapter* adapter,
                                     BluetoothDevice* device) {
  DeviceChanged(adapter, device);
}

void ArcBluetoothBridge::DeviceChanged(BluetoothAdapter* adapter,
                                       BluetoothDevice* device) {
  if (!IsInstanceUp())
    return;

  SendDevice(device);

  if (!(device->GetType() & device::BLUETOOTH_TRANSPORT_LE))
    return;

  std::string addr = device->GetAddress();
  auto it = gatt_connections_.find(addr);
  bool was_connected =
      (it != gatt_connections_.end() &&
       it->second.state == GattConnection::ConnectionState::CONNECTED);
  bool is_connected = device->IsConnected();
  if (was_connected == is_connected)
    return;

  if (!is_connected) {
    OnGattDisconnected(mojom::BluetoothAddress::From(addr));
    return;
  }

  // Only process connection from remote device. Connection to remote device is
  // processed in the callback of ConnectLEDevice().
  if (it == gatt_connections_.end())
    OnGattConnected(mojom::BluetoothAddress::From(addr), nullptr);
}

void ArcBluetoothBridge::DeviceAddressChanged(BluetoothAdapter* adapter,
                                              BluetoothDevice* device,
                                              const std::string& old_address) {
  if (!IsInstanceUp())
    return;

  std::string new_address = device->GetAddress();
  if (old_address == new_address)
    return;

  if (!(device->GetType() & device::BLUETOOTH_TRANSPORT_LE))
    return;

  auto it = gatt_connections_.find(old_address);
  if (it == gatt_connections_.end())
    return;

  gatt_connections_.emplace(new_address, std::move(it->second));
  gatt_connections_.erase(it);

  auto* btle_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnLEDeviceAddressChange);
  if (!btle_instance)
    return;

  btle_instance->OnLEDeviceAddressChange(
      mojom::BluetoothAddress::From(old_address),
      mojom::BluetoothAddress::From(new_address));
}

void ArcBluetoothBridge::DevicePairedChanged(BluetoothAdapter* adapter,
                                             BluetoothDevice* device,
                                             bool new_paired_status) {
  if (!IsInstanceUp())
    return;

  DCHECK(adapter);
  DCHECK(device);

  mojom::BluetoothAddressPtr addr =
      mojom::BluetoothAddress::From(device->GetAddress());

  if (new_paired_status) {
    // OnBondStateChanged must be called with BluetoothBondState::BONDING to
    // make sure the bond state machine on Android is ready to take the
    // pair-done event. Otherwise the pair-done event will be dropped as an
    // invalid change of paired status.
    OnPairing(addr->Clone());
    OnPairedDone(std::move(addr));
  } else {
    OnForgetDone(std::move(addr));
  }
}

void ArcBluetoothBridge::DeviceMTUChanged(BluetoothAdapter* adapter,
                                          BluetoothDevice* device,
                                          uint16_t mtu) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnMTUReceived);
  if (!device->IsConnected() || bluetooth_instance == nullptr)
    return;
  bluetooth_instance->OnMTUReceived(
      mojom::BluetoothAddress::From(device->GetAddress()), mtu);
}

void ArcBluetoothBridge::DeviceRemoved(BluetoothAdapter* adapter,
                                       BluetoothDevice* device) {
  if (!IsInstanceUp())
    return;

  DCHECK(adapter);
  DCHECK(device);

  std::string address = device->GetAddress();
  OnGattDisconnected(mojom::BluetoothAddress::From(address));
  OnForgetDone(mojom::BluetoothAddress::From(address));
}

void ArcBluetoothBridge::GattServiceAdded(BluetoothAdapter* adapter,
                                          BluetoothDevice* device,
                                          BluetoothRemoteGattService* service) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattServiceRemoved(
    BluetoothAdapter* adapter,
    BluetoothDevice* device,
    BluetoothRemoteGattService* service) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattServicesDiscovered(BluetoothAdapter* adapter,
                                                BluetoothDevice* device) {
  if (!IsInstanceUp())
    return;

  auto* btle_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnSearchComplete);
  if (!btle_instance)
    return;

  mojom::BluetoothAddressPtr addr =
      mojom::BluetoothAddress::From(device->GetAddress());

  btle_instance->OnSearchComplete(std::move(addr),
                                  mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::GattDiscoveryCompleteForService(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattService* service) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattServiceChanged(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattService* service) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattCharacteristicAdded(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattCharacteristic* characteristic) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattCharacteristicRemoved(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattCharacteristic* characteristic) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattDescriptorAdded(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattDescriptor* descriptor) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattDescriptorRemoved(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattDescriptor* descriptor) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

void ArcBluetoothBridge::GattCharacteristicValueChanged(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattCharacteristic* characteristic,
    const std::vector<uint8_t>& value) {
  if (!IsInstanceUp())
    return;

  auto* btle_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnGattNotify);
  if (!btle_instance)
    return;

  BluetoothRemoteGattService* service = characteristic->GetService();
  BluetoothDevice* device = service->GetDevice();
  mojom::BluetoothAddressPtr address =
      mojom::BluetoothAddress::From(device->GetAddress());
  mojom::BluetoothGattServiceIDPtr service_id =
      mojom::BluetoothGattServiceID::New();
  service_id->is_primary = service->IsPrimary();
  service_id->id = mojom::BluetoothGattID::New();
  service_id->id->inst_id = ConvertGattIdentifierToId(service->GetIdentifier());
  service_id->id->uuid = service->GetUUID();

  mojom::BluetoothGattIDPtr char_id = mojom::BluetoothGattID::New();
  char_id->inst_id = ConvertGattIdentifierToId(characteristic->GetIdentifier());
  char_id->uuid = characteristic->GetUUID();

  btle_instance->OnGattNotify(std::move(address), std::move(service_id),
                              std::move(char_id), true /* is_notify */, value);
}

void ArcBluetoothBridge::GattDescriptorValueChanged(
    BluetoothAdapter* adapter,
    BluetoothRemoteGattDescriptor* descriptor,
    const std::vector<uint8_t>& value) {
  if (!IsInstanceUp())
    return;
  // Placeholder for GATT client functionality
}

template <class LocalGattAttribute>
void ArcBluetoothBridge::OnGattAttributeReadRequest(
    const BluetoothDevice* device,
    const LocalGattAttribute* attribute,
    int offset,
    mojom::BluetoothGattDBAttributeType attribute_type,
    const ValueCallback& success_callback,
    const ErrorCallback& error_callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), RequestGattRead);
  if (!bluetooth_instance || !IsGattOffsetValid(offset)) {
    error_callback.Run();
    return;
  }

  DCHECK(gatt_handle_.find(attribute->GetIdentifier()) != gatt_handle_.end());

  bluetooth_instance->RequestGattRead(
      mojom::BluetoothAddress::From(device->GetAddress()),
      gatt_handle_[attribute->GetIdentifier()], offset, false /* is_long */,
      attribute_type,
      base::BindOnce(&OnGattServerRead, success_callback, error_callback));
}

template <class LocalGattAttribute>
void ArcBluetoothBridge::OnGattAttributeWriteRequest(
    const BluetoothDevice* device,
    const LocalGattAttribute* attribute,
    const std::vector<uint8_t>& value,
    int offset,
    mojom::BluetoothGattDBAttributeType attribute_type,
    const base::Closure& success_callback,
    const ErrorCallback& error_callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), RequestGattWrite);
  if (!bluetooth_instance || !IsGattOffsetValid(offset)) {
    error_callback.Run();
    return;
  }

  DCHECK(gatt_handle_.find(attribute->GetIdentifier()) != gatt_handle_.end());

  bluetooth_instance->RequestGattWrite(
      mojom::BluetoothAddress::From(device->GetAddress()),
      gatt_handle_[attribute->GetIdentifier()], offset, value, attribute_type,
      base::BindOnce(&OnGattServerWrite, success_callback, error_callback));
}

void ArcBluetoothBridge::OnCharacteristicReadRequest(
    const BluetoothDevice* device,
    const BluetoothLocalGattCharacteristic* characteristic,
    int offset,
    const ValueCallback& callback,
    const ErrorCallback& error_callback) {
  OnGattAttributeReadRequest(
      device, characteristic, offset,
      mojom::BluetoothGattDBAttributeType::BTGATT_DB_CHARACTERISTIC, callback,
      error_callback);
}

void ArcBluetoothBridge::OnCharacteristicWriteRequest(
    const BluetoothDevice* device,
    const BluetoothLocalGattCharacteristic* characteristic,
    const std::vector<uint8_t>& value,
    int offset,
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  OnGattAttributeWriteRequest(
      device, characteristic, value, offset,
      mojom::BluetoothGattDBAttributeType::BTGATT_DB_CHARACTERISTIC, callback,
      error_callback);
}

void ArcBluetoothBridge::OnDescriptorReadRequest(
    const BluetoothDevice* device,
    const BluetoothLocalGattDescriptor* descriptor,
    int offset,
    const ValueCallback& callback,
    const ErrorCallback& error_callback) {
  OnGattAttributeReadRequest(
      device, descriptor, offset,
      mojom::BluetoothGattDBAttributeType::BTGATT_DB_DESCRIPTOR, callback,
      error_callback);
}

void ArcBluetoothBridge::OnDescriptorWriteRequest(
    const BluetoothDevice* device,
    const BluetoothLocalGattDescriptor* descriptor,
    const std::vector<uint8_t>& value,
    int offset,
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  OnGattAttributeWriteRequest(
      device, descriptor, value, offset,
      mojom::BluetoothGattDBAttributeType::BTGATT_DB_DESCRIPTOR, callback,
      error_callback);
}

void ArcBluetoothBridge::OnNotificationsStart(
    const BluetoothDevice* device,
    const BluetoothLocalGattCharacteristic* characteristic) {}

void ArcBluetoothBridge::OnNotificationsStop(
    const BluetoothDevice* device,
    const BluetoothLocalGattCharacteristic* characteristic) {}

void ArcBluetoothBridge::EnableAdapter(EnableAdapterCallback callback) {
  DCHECK(bluetooth_adapter_);
  if (IsPowerChangeInitiatedByLocal(AdapterPowerState::TURN_ON)) {
    DequeueLocalPowerChange(AdapterPowerState::TURN_ON);
  } else {
    if (!bluetooth_adapter_->IsPowered()) {
      EnqueueRemotePowerChange(AdapterPowerState::TURN_ON, std::move(callback));
      return;
    }
  }

  OnPoweredOn(std::move(callback), false /* save_user_pref */);
}

void ArcBluetoothBridge::DisableAdapter(DisableAdapterCallback callback) {
  DCHECK(bluetooth_adapter_);
  if (IsPowerChangeInitiatedByLocal(AdapterPowerState::TURN_OFF)) {
    DequeueLocalPowerChange(AdapterPowerState::TURN_OFF);
  } else {
    if (bluetooth_adapter_->IsPowered()) {
      EnqueueRemotePowerChange(AdapterPowerState::TURN_OFF,
                               std::move(callback));
      return;
    }
  }

  OnPoweredOff(std::move(callback), false /* save_user_pref */);
}

void ArcBluetoothBridge::GetAdapterProperty(mojom::BluetoothPropertyType type) {
  DCHECK(bluetooth_adapter_);
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnAdapterProperties);
  if (!bluetooth_instance)
    return;

  std::vector<mojom::BluetoothPropertyPtr> properties =
      GetAdapterProperties(type);

  bluetooth_instance->OnAdapterProperties(mojom::BluetoothStatus::SUCCESS,
                                          std::move(properties));
}

void ArcBluetoothBridge::OnSetDiscoverable(bool discoverable,
                                           bool success,
                                           uint32_t timeout) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (success && discoverable && timeout > 0) {
    discoverable_off_timer_.Start(
        FROM_HERE, base::TimeDelta::FromSeconds(timeout),
        base::Bind(&ArcBluetoothBridge::SetDiscoverable,
                   weak_factory_.GetWeakPtr(), false, 0));
  }

  auto status =
      success ? mojom::BluetoothStatus::SUCCESS : mojom::BluetoothStatus::FAIL;
  OnSetAdapterProperty(status, GetDiscoveryTimeoutProperty(timeout));
}

// Set discoverable state to on / off.
// In case of turning on, start timer to turn it back off in |timeout| seconds.
void ArcBluetoothBridge::SetDiscoverable(bool discoverable, uint32_t timeout) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(bluetooth_adapter_);
  DCHECK(!discoverable || timeout == 0);

  bool currently_discoverable = bluetooth_adapter_->IsDiscoverable();

  if (!discoverable && !currently_discoverable)
    return;

  if (discoverable && currently_discoverable) {
    if (base::TimeDelta::FromSeconds(timeout) >
        discoverable_off_timer_.GetCurrentDelay()) {
      // Restart discoverable_off_timer_ if new timeout is greater
      OnSetDiscoverable(true, true, timeout);
    } else {
      // Just send message to Android if new timeout is lower.
      OnSetAdapterProperty(mojom::BluetoothStatus::SUCCESS,
                           GetDiscoveryTimeoutProperty(timeout));
    }
    return;
  }

  bluetooth_adapter_->SetDiscoverable(
      discoverable,
      base::Bind(&ArcBluetoothBridge::OnSetDiscoverable,
                 weak_factory_.GetWeakPtr(), discoverable, true, timeout),
      base::Bind(&ArcBluetoothBridge::OnSetDiscoverable,
                 weak_factory_.GetWeakPtr(), discoverable, false, timeout));
}

void ArcBluetoothBridge::OnSetAdapterProperty(
    mojom::BluetoothStatus status,
    mojom::BluetoothPropertyPtr property) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnAdapterProperties);
  if (!bluetooth_instance)
    return;

  std::vector<arc::mojom::BluetoothPropertyPtr> properties;
  properties.push_back(std::move(property));

  bluetooth_instance->OnAdapterProperties(status, std::move(properties));
}

void ArcBluetoothBridge::SetAdapterProperty(
    mojom::BluetoothPropertyPtr property) {
  DCHECK(bluetooth_adapter_);

  if (property->is_discovery_timeout()) {
    uint32_t discovery_timeout = property->get_discovery_timeout();
    if (discovery_timeout > 0) {
      SetDiscoverable(true, discovery_timeout);
    } else {
      OnSetAdapterProperty(mojom::BluetoothStatus::PARM_INVALID,
                           std::move(property));
    }
  } else if (property->is_bdname()) {
    auto property_clone = property.Clone();
    bluetooth_adapter_->SetName(
        property->get_bdname(),
        base::Bind(&ArcBluetoothBridge::OnSetAdapterProperty,
                   weak_factory_.GetWeakPtr(), mojom::BluetoothStatus::SUCCESS,
                   base::Passed(&property)),
        base::Bind(&ArcBluetoothBridge::OnSetAdapterProperty,
                   weak_factory_.GetWeakPtr(), mojom::BluetoothStatus::FAIL,
                   base::Passed(&property_clone)));
  } else if (property->is_adapter_scan_mode()) {
    // Android will set adapter scan mode in these 3 situations.
    // 1) Set to BT_SCAN_MODE_NONE just before turning BT off.
    // 2) Set to BT_SCAN_MODE_CONNECTABLE just after turning on.
    // 3) Set to BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE just before set the
    //    discoverable timeout.
    // Since turning BT off/on implied scan mode none/connectable and setting
    // discovery timeout implied scan mode discoverable, we don't need to
    // do anything here. We will just call success callback in this case.
    OnSetAdapterProperty(mojom::BluetoothStatus::SUCCESS, std::move(property));
  } else {
    // Android does not set any other property type.
    OnSetAdapterProperty(mojom::BluetoothStatus::UNSUPPORTED,
                         std::move(property));
  }
}

void ArcBluetoothBridge::GetRemoteDeviceProperty(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothPropertyType type) {
  DCHECK(bluetooth_adapter_);
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnRemoteDeviceProperties);
  if (!bluetooth_instance)
    return;

  std::string addr_str = remote_addr->To<std::string>();
  BluetoothDevice* device = bluetooth_adapter_->GetDevice(addr_str);

  std::vector<mojom::BluetoothPropertyPtr> properties =
      GetDeviceProperties(type, device);
  mojom::BluetoothStatus status = mojom::BluetoothStatus::SUCCESS;

  if (!device) {
    VLOG(1) << __func__ << ": device " << addr_str << " not available";
    status = mojom::BluetoothStatus::FAIL;
  }

  bluetooth_instance->OnRemoteDeviceProperties(status, std::move(remote_addr),
                                               std::move(properties));
}

void ArcBluetoothBridge::SetRemoteDeviceProperty(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothPropertyPtr property) {
  DCHECK(bluetooth_adapter_);
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnRemoteDeviceProperties);
  if (!bluetooth_instance)
    return;

  // Unsupported. Only used by Android hidden API, BluetoothDevice.SetAlias().
  // And only Android Settings App / Android TV / NFC used that.
  bluetooth_instance->OnRemoteDeviceProperties(
      mojom::BluetoothStatus::UNSUPPORTED, std::move(remote_addr),
      std::vector<mojom::BluetoothPropertyPtr>());
}

void ArcBluetoothBridge::GetRemoteServiceRecord(
    mojom::BluetoothAddressPtr remote_addr,
    const BluetoothUUID& uuid) {
  // TODO(smbarber): Implement GetRemoteServiceRecord
}

void ArcBluetoothBridge::GetRemoteServices(
    mojom::BluetoothAddressPtr remote_addr) {
  // TODO(smbarber): Implement GetRemoteServices
}

void ArcBluetoothBridge::StartDiscovery() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(bluetooth_adapter_);

  if (discovery_session_) {
    LOG(ERROR) << "Discovery session already running; Reset timeout.";
    discovery_off_timer_.Start(FROM_HERE, kDiscoveryTimeout,
                               base::Bind(&ArcBluetoothBridge::CancelDiscovery,
                                          weak_factory_.GetWeakPtr()));
    SendCachedDevicesFound();
    return;
  }

  bluetooth_adapter_->StartDiscoverySession(
      base::Bind(&ArcBluetoothBridge::OnDiscoveryStarted,
                 weak_factory_.GetWeakPtr()),
      base::Bind(&ArcBluetoothBridge::OnDiscoveryError,
                 weak_factory_.GetWeakPtr()));
}

void ArcBluetoothBridge::CancelDiscovery() {
  if (!discovery_session_) {
    return;
  }

  discovery_session_->Stop(base::Bind(&ArcBluetoothBridge::OnDiscoveryStopped,
                                      weak_factory_.GetWeakPtr()),
                           base::Bind(&ArcBluetoothBridge::OnDiscoveryError,
                                      weak_factory_.GetWeakPtr()));
}

void ArcBluetoothBridge::OnPoweredOn(
    ArcBluetoothBridge::AdapterStateCallback callback,
    bool save_user_pref) const {
  // Saves the power state to user preference only if Android initiated it.
  if (save_user_pref)
    SetPrimaryUserBluetoothPowerSetting(true);

  std::move(callback).Run(mojom::BluetoothAdapterState::ON);
  SendCachedPairedDevices();
}

void ArcBluetoothBridge::OnPoweredOff(
    ArcBluetoothBridge::AdapterStateCallback callback,
    bool save_user_pref) const {
  // Saves the power state to user preference only if Android initiated it.
  if (save_user_pref)
    SetPrimaryUserBluetoothPowerSetting(false);

  std::move(callback).Run(mojom::BluetoothAdapterState::OFF);
}

void ArcBluetoothBridge::OnPoweredError(
    ArcBluetoothBridge::AdapterStateCallback callback) const {
  LOG(WARNING) << "failed to change power state";

  std::move(callback).Run(bluetooth_adapter_->IsPowered()
                              ? mojom::BluetoothAdapterState::ON
                              : mojom::BluetoothAdapterState::OFF);
}

void ArcBluetoothBridge::OnDiscoveryStarted(
    std::unique_ptr<BluetoothDiscoverySession> session) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnDiscoveryStateChanged);
  if (!bluetooth_instance)
    return;

  discovery_session_ = std::move(session);

  // We need to set timer to turn device discovery off because of the difference
  // between Android API (do device discovery once) and Chrome API (do device
  // discovery until user turns it off).
  discovery_off_timer_.Start(FROM_HERE, kDiscoveryTimeout,
                             base::Bind(&ArcBluetoothBridge::CancelDiscovery,
                                        weak_factory_.GetWeakPtr()));

  bluetooth_instance->OnDiscoveryStateChanged(
      mojom::BluetoothDiscoveryState::STARTED);

  SendCachedDevicesFound();
}

void ArcBluetoothBridge::OnDiscoveryStopped() {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnDiscoveryStateChanged);
  if (!bluetooth_instance)
    return;

  discovery_session_.reset();
  discovery_off_timer_.Stop();

  bluetooth_instance->OnDiscoveryStateChanged(
      mojom::BluetoothDiscoveryState::STOPPED);
}

void ArcBluetoothBridge::CreateBond(mojom::BluetoothAddressPtr addr,
                                    int32_t transport) {
  std::string addr_str = addr->To<std::string>();
  BluetoothDevice* device = bluetooth_adapter_->GetDevice(addr_str);
  if (!device || !device->IsPairable()) {
    VLOG(1) << __func__ << ": device " << addr_str
            << " is no longer valid or pairable";
    OnPairedError(std::move(addr), BluetoothDevice::ERROR_FAILED);
    return;
  }

  if (device->IsPaired()) {
    OnPairedDone(std::move(addr));
    return;
  }

  // Use the default pairing delegate which is the delegate registered and owned
  // by ash.
  BluetoothDevice::PairingDelegate* delegate =
      bluetooth_adapter_->DefaultPairingDelegate();

  if (!delegate) {
    OnPairedError(std::move(addr), BluetoothDevice::ERROR_FAILED);
    return;
  }

  // If pairing finished successfully, DevicePairedChanged will notify Android
  // on paired state change event, so DoNothing is passed as a success callback.
  device->Pair(delegate, base::DoNothing(),
               base::Bind(&ArcBluetoothBridge::OnPairedError,
                          weak_factory_.GetWeakPtr(), base::Passed(&addr)));
}

void ArcBluetoothBridge::RemoveBond(mojom::BluetoothAddressPtr addr) {
  // Forget the device if it is no longer valid or not even paired.
  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(addr->To<std::string>());
  if (!device || !device->IsPaired()) {
    OnForgetDone(std::move(addr));
    return;
  }

  // If unpairing finished successfully, DevicePairedChanged will notify Android
  // on paired state change event, so DoNothing is passed as a success callback.
  device->Forget(base::DoNothing(),
                 base::Bind(&ArcBluetoothBridge::OnForgetError,
                            weak_factory_.GetWeakPtr(), base::Passed(&addr)));
}

void ArcBluetoothBridge::CancelBond(mojom::BluetoothAddressPtr addr) {
  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(addr->To<std::string>());
  if (!device) {
    OnForgetDone(std::move(addr));
    return;
  }

  device->CancelPairing();
  OnForgetDone(std::move(addr));
}

void ArcBluetoothBridge::GetConnectionState(
    mojom::BluetoothAddressPtr addr,
    GetConnectionStateCallback callback) {
  if (!bluetooth_adapter_) {
    std::move(callback).Run(false);
    return;
  }

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(addr->To<std::string>());
  if (!device) {
    std::move(callback).Run(false);
    return;
  }

  std::move(callback).Run(device->IsConnected());
}

void ArcBluetoothBridge::StartLEScan() {
  DCHECK(bluetooth_adapter_);
  if (discovery_session_) {
    LOG(WARNING) << "Discovery session already running; leaving alone";
    SendCachedDevicesFound();
    return;
  }
  bluetooth_adapter_->StartDiscoverySessionWithFilter(
      std::make_unique<BluetoothDiscoveryFilter>(
          device::BLUETOOTH_TRANSPORT_LE),
      base::Bind(&ArcBluetoothBridge::OnDiscoveryStarted,
                 weak_factory_.GetWeakPtr()),
      base::Bind(&ArcBluetoothBridge::OnDiscoveryError,
                 weak_factory_.GetWeakPtr()));
}

void ArcBluetoothBridge::StopLEScan() {
  CancelDiscovery();
}

void ArcBluetoothBridge::OnGattConnectStateChanged(
    mojom::BluetoothAddressPtr addr,
    bool connected) const {
  auto* btle_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnLEConnectionStateChange);
  if (!btle_instance)
    return;

  DCHECK(addr);

  btle_instance->OnLEConnectionStateChange(std::move(addr), connected);
}

void ArcBluetoothBridge::OnGattConnected(
    mojom::BluetoothAddressPtr addr,
    std::unique_ptr<BluetoothGattConnection> connection) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  gatt_connections_[addr->To<std::string>()] = GattConnection(
      GattConnection::ConnectionState::CONNECTED, std::move(connection));
  OnGattConnectStateChanged(std::move(addr), true);
}

void ArcBluetoothBridge::OnGattConnectError(
    mojom::BluetoothAddressPtr addr,
    BluetoothDevice::ConnectErrorCode error_code) {
  LOG(WARNING) << "GattConnectError: error_code = " << error_code;
  OnGattDisconnected(std::move(addr));
}

void ArcBluetoothBridge::OnGattDisconnected(mojom::BluetoothAddressPtr addr) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (gatt_connections_.erase(addr->To<std::string>()) == 0) {
    LOG(WARNING) << "OnGattDisconnected called, "
                 << "but no gatt connection was found";
    return;
  }
  OnGattConnectStateChanged(std::move(addr), false);
}

void ArcBluetoothBridge::ConnectLEDevice(
    mojom::BluetoothAddressPtr remote_addr) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnLEConnectionStateChange);
  if (!bluetooth_instance)
    return;

  std::string addr = remote_addr->To<std::string>();
  BluetoothDevice* device = bluetooth_adapter_->GetDevice(addr);

  if (!device) {
    LOG(ERROR) << "Unknown device " << addr;
    OnGattConnectError(std::move(remote_addr),
                       BluetoothDevice::ConnectErrorCode::ERROR_FAILED);
    return;
  }

  auto it = gatt_connections_.find(addr);
  if (it != gatt_connections_.end()) {
    if (it->second.state == GattConnection::ConnectionState::CONNECTED) {
      bluetooth_instance->OnLEConnectionStateChange(std::move(remote_addr),
                                                    true);
    } else {
      OnGattConnectError(std::move(remote_addr),
                         BluetoothDevice::ConnectErrorCode::ERROR_INPROGRESS);
    }
    return;
  }

  // Also pass disconnect callback in error case since it would be disconnected
  // anyway.
  gatt_connections_.emplace(
      addr,
      GattConnection(GattConnection::ConnectionState::CONNECTING, nullptr));
  mojom::BluetoothAddressPtr remote_addr_clone = remote_addr.Clone();
  device->CreateGattConnection(
      base::Bind(&ArcBluetoothBridge::OnGattConnected,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr)),
      base::Bind(&ArcBluetoothBridge::OnGattConnectError,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr_clone)));
}

void ArcBluetoothBridge::DisconnectLEDevice(
    mojom::BluetoothAddressPtr remote_addr) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnLEConnectionStateChange);
  if (!bluetooth_instance)
    return;

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());

  if (!device || !device->IsConnected()) {
    bluetooth_instance->OnLEConnectionStateChange(std::move(remote_addr),
                                                  false);
    return;
  }

  mojom::BluetoothAddressPtr remote_addr_clone = remote_addr.Clone();
  device->Disconnect(
      base::Bind(&ArcBluetoothBridge::OnGattDisconnected,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr)),
      base::Bind(&ArcBluetoothBridge::OnGattDisconnected,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr_clone)));
}

void ArcBluetoothBridge::SearchService(mojom::BluetoothAddressPtr remote_addr) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnSearchComplete);
  if (!bluetooth_instance)
    return;

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());
  if (!device) {
    LOG(ERROR) << "Unknown device " << remote_addr->To<std::string>();
    bluetooth_instance->OnSearchComplete(
        std::move(remote_addr), mojom::BluetoothGattStatus::GATT_FAILURE);
    return;
  }

  // Call the callback if discovery is completed
  if (device->IsGattServicesDiscoveryComplete()) {
    bluetooth_instance->OnSearchComplete(
        std::move(remote_addr), mojom::BluetoothGattStatus::GATT_SUCCESS);
    return;
  }

  // Discard result. Will call the callback when discovery is completed.
  device->GetGattServices();
}

void ArcBluetoothBridge::OnStartLEListenDone(
    ArcBluetoothBridge::GattStatusCallback callback,
    scoped_refptr<BluetoothAdvertisement> advertisement) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisment_ = advertisement;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::OnStartLEListenError(
    ArcBluetoothBridge::GattStatusCallback callback,
    BluetoothAdvertisement::ErrorCode error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisment_ = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
}

void ArcBluetoothBridge::StartLEListen(StartLEListenCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  std::unique_ptr<BluetoothAdvertisement::Data> adv_data =
      std::make_unique<BluetoothAdvertisement::Data>(
          BluetoothAdvertisement::ADVERTISEMENT_TYPE_BROADCAST);
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  bluetooth_adapter_->RegisterAdvertisement(
      std::move(adv_data),
      base::Bind(&ArcBluetoothBridge::OnStartLEListenDone,
                 weak_factory_.GetWeakPtr(), repeating_callback),
      base::Bind(&ArcBluetoothBridge::OnStartLEListenError,
                 weak_factory_.GetWeakPtr(), repeating_callback));
}

void ArcBluetoothBridge::OnStopLEListenDone(
    ArcBluetoothBridge::GattStatusCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisment_ = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::OnStopLEListenError(
    ArcBluetoothBridge::GattStatusCallback callback,
    BluetoothAdvertisement::ErrorCode error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisment_ = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
}

void ArcBluetoothBridge::StopLEListen(StopLEListenCallback callback) {
  if (!advertisment_) {
    OnStopLEListenError(
        std::move(callback),
        BluetoothAdvertisement::ErrorCode::ERROR_ADVERTISEMENT_DOES_NOT_EXIST);
    return;
  }
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  advertisment_->Unregister(
      base::Bind(&ArcBluetoothBridge::OnStopLEListenDone,
                 weak_factory_.GetWeakPtr(), repeating_callback),
      base::Bind(&ArcBluetoothBridge::OnStopLEListenError,
                 weak_factory_.GetWeakPtr(), repeating_callback));
}

void ArcBluetoothBridge::GetGattDB(mojom::BluetoothAddressPtr remote_addr) {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnGetGattDB);
  if (!bluetooth_instance)
    return;

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());
  std::vector<mojom::BluetoothGattDBElementPtr> db;

  if (!device) {
    LOG(ERROR) << "Unknown device " << remote_addr->To<std::string>();
    bluetooth_instance->OnGetGattDB(std::move(remote_addr), std::move(db));
    return;
  }

  for (auto* service : device->GetGattServices()) {
    mojom::BluetoothGattDBElementPtr service_element = CreateGattDBElement(
        service->IsPrimary()
            ? mojom::BluetoothGattDBAttributeType::BTGATT_DB_PRIMARY_SERVICE
            : mojom::BluetoothGattDBAttributeType::BTGATT_DB_SECONDARY_SERVICE,
        service);

    const auto& characteristics = service->GetCharacteristics();
    if (characteristics.size() > 0) {
      const auto& descriptors = characteristics.back()->GetDescriptors();
      service_element->start_handle =
          ConvertGattIdentifierToId(characteristics.front()->GetIdentifier());
      service_element->end_handle = ConvertGattIdentifierToId(
          descriptors.size() > 0 ? descriptors.back()->GetIdentifier()
                                 : characteristics.back()->GetIdentifier());
    }
    db.push_back(std::move(service_element));

    for (auto* characteristic : characteristics) {
      mojom::BluetoothGattDBElementPtr characteristic_element =
          CreateGattDBElement(
              mojom::BluetoothGattDBAttributeType::BTGATT_DB_CHARACTERISTIC,
              characteristic);
      characteristic_element->properties = characteristic->GetProperties();
      db.push_back(std::move(characteristic_element));

      for (auto* descriptor : characteristic->GetDescriptors()) {
        db.push_back(CreateGattDBElement(
            mojom::BluetoothGattDBAttributeType::BTGATT_DB_DESCRIPTOR,
            descriptor));
      }
    }
  }

  bluetooth_instance->OnGetGattDB(std::move(remote_addr), std::move(db));
}

BluetoothRemoteGattCharacteristic* ArcBluetoothBridge::FindGattCharacteristic(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id) const {
  DCHECK(remote_addr);
  DCHECK(service_id);
  DCHECK(char_id);

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());
  if (!device)
    return nullptr;

  BluetoothRemoteGattService* service =
      FindGattAttributeByUuid(device->GetGattServices(), service_id->id->uuid);
  if (!service)
    return nullptr;

  return FindGattAttributeByUuid(service->GetCharacteristics(), char_id->uuid);
}

BluetoothRemoteGattDescriptor* ArcBluetoothBridge::FindGattDescriptor(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    mojom::BluetoothGattIDPtr desc_id) const {
  BluetoothRemoteGattCharacteristic* characteristic = FindGattCharacteristic(
      std::move(remote_addr), std::move(service_id), std::move(char_id));
  if (!characteristic)
    return nullptr;

  return FindGattAttributeByUuid(characteristic->GetDescriptors(),
                                 desc_id->uuid);
}

void ArcBluetoothBridge::SendBluetoothPoweredStateBroadcast(
    AdapterPowerState powered) const {
  auto* intent_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->intent_helper(), SendBroadcast);
  if (!intent_instance)
    return;

  base::DictionaryValue extras;
  extras.SetBoolean("enable", powered == AdapterPowerState::TURN_ON);
  std::string extras_json;
  bool write_success = base::JSONWriter::Write(extras, &extras_json);
  DCHECK(write_success);

  intent_instance->SendBroadcast(
      ArcIntentHelperBridge::AppendStringToIntentHelperPackageName(
          "SET_BLUETOOTH_STATE"),
      ArcIntentHelperBridge::kArcIntentHelperPackageName,
      ArcIntentHelperBridge::AppendStringToIntentHelperPackageName(
          "SettingsReceiver"),
      extras_json);
}

void ArcBluetoothBridge::ReadGattCharacteristic(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    ReadGattCharacteristicCallback callback) {
  BluetoothRemoteGattCharacteristic* characteristic = FindGattCharacteristic(
      std::move(remote_addr), std::move(service_id), std::move(char_id));
  DCHECK(characteristic);
  DCHECK(characteristic->GetPermissions() & kGattReadPermission);

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  characteristic->ReadRemoteCharacteristic(
      base::Bind(&OnGattReadDone, repeating_callback),
      base::Bind(&OnGattReadError, repeating_callback));
}

void ArcBluetoothBridge::WriteGattCharacteristic(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    mojom::BluetoothGattValuePtr value,
    WriteGattCharacteristicCallback callback) {
  BluetoothRemoteGattCharacteristic* characteristic = FindGattCharacteristic(
      std::move(remote_addr), std::move(service_id), std::move(char_id));
  DCHECK(characteristic);
  DCHECK(characteristic->GetPermissions() & kGattWritePermission);

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  characteristic->WriteRemoteCharacteristic(
      value->value, base::Bind(&OnGattOperationDone, repeating_callback),
      base::Bind(&OnGattOperationError, repeating_callback));
}

void ArcBluetoothBridge::ReadGattDescriptor(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    mojom::BluetoothGattIDPtr desc_id,
    ReadGattDescriptorCallback callback) {
  BluetoothRemoteGattDescriptor* descriptor =
      FindGattDescriptor(std::move(remote_addr), std::move(service_id),
                         std::move(char_id), std::move(desc_id));
  DCHECK(descriptor);
  DCHECK(descriptor->GetPermissions() & kGattReadPermission);

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  descriptor->ReadRemoteDescriptor(
      base::Bind(&OnGattReadDone, repeating_callback),
      base::Bind(&OnGattReadError, repeating_callback));
}

void ArcBluetoothBridge::WriteGattDescriptor(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    mojom::BluetoothGattIDPtr desc_id,
    mojom::BluetoothGattValuePtr value,
    WriteGattDescriptorCallback callback) {
  BluetoothRemoteGattDescriptor* descriptor =
      FindGattDescriptor(std::move(remote_addr), std::move(service_id),
                         std::move(char_id), std::move(desc_id));
  DCHECK(descriptor);
  DCHECK(descriptor->GetPermissions() & kGattWritePermission);

  // To register / deregister GATT notification, we need to
  // 1) Write to CCC Descriptor to enable/disable the notification
  // 2) Ask BT hw to register / deregister the notification
  // The Chrome API groups both steps into one API, and does not support writing
  // directly to the CCC Descriptor. Therefore, until we fix
  // https://crbug.com/622832, we return successfully when we encounter this.
  // TODO(http://crbug.com/622832)
  if (descriptor->GetUUID() ==
      BluetoothGattDescriptor::ClientCharacteristicConfigurationUuid()) {
    OnGattOperationDone(std::move(callback));
    return;
  }

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  descriptor->WriteRemoteDescriptor(
      value->value, base::Bind(&OnGattOperationDone, repeating_callback),
      base::Bind(&OnGattOperationError, repeating_callback));
}

void ArcBluetoothBridge::OnGattNotifyStartDone(
    ArcBluetoothBridge::GattStatusCallback callback,
    const std::string char_string_id,
    std::unique_ptr<BluetoothGattNotifySession> notify_session) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  notification_session_[char_string_id] = std::move(notify_session);
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::RegisterForGattNotification(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    RegisterForGattNotificationCallback callback) {
  BluetoothRemoteGattCharacteristic* characteristic = FindGattCharacteristic(
      std::move(remote_addr), std::move(service_id), std::move(char_id));

  if (!characteristic) {
    LOG(WARNING) << __func__ << " Characteristic is not existed.";
    return;
  }

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  characteristic->StartNotifySession(
      base::Bind(&ArcBluetoothBridge::OnGattNotifyStartDone,
                 weak_factory_.GetWeakPtr(), repeating_callback,
                 characteristic->GetIdentifier()),
      base::Bind(&OnGattOperationError, repeating_callback));
}

void ArcBluetoothBridge::DeregisterForGattNotification(
    mojom::BluetoothAddressPtr remote_addr,
    mojom::BluetoothGattServiceIDPtr service_id,
    mojom::BluetoothGattIDPtr char_id,
    DeregisterForGattNotificationCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  BluetoothRemoteGattCharacteristic* characteristic = FindGattCharacteristic(
      std::move(remote_addr), std::move(service_id), std::move(char_id));

  if (!characteristic) {
    LOG(WARNING) << __func__ << " Characteristic is not existed.";
    return;
  }

  // TODO(rkc): Return an error code when failing. crbug.com/771055
  std::string char_id_str = characteristic->GetIdentifier();
  auto it = notification_session_.find(char_id_str);
  if (it == notification_session_.end()) {
    LOG(WARNING) << "Notification session not found " << char_id_str;
    return;
  }
  std::unique_ptr<BluetoothGattNotifySession> notify = std::move(it->second);
  notification_session_.erase(it);
  notify->Stop(
      base::Bind(&OnGattOperationDone, base::Passed(std::move(callback))));
}

void ArcBluetoothBridge::ReadRemoteRssi(mojom::BluetoothAddressPtr remote_addr,
                                        ReadRemoteRssiCallback callback) {
  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());
  if (!device) {
    std::move(callback).Run(mojom::kUnknownPower);
    return;
  }
  std::move(callback).Run(
      device->GetInquiryRSSI().value_or(mojom::kUnknownPower));
}

void ArcBluetoothBridge::OpenBluetoothSocket(
    OpenBluetoothSocketCallback callback) {
  base::ScopedFD sock(socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM));
  if (!sock.is_valid()) {
    LOG(ERROR) << "Failed to open socket.";
    std::move(callback).Run(mojo::ScopedHandle());
    return;
  }
  mojo::edk::ScopedPlatformHandle platform_handle{
      mojo::edk::PlatformHandle(sock.release())};
  MojoHandle wrapped_handle;
  MojoResult wrap_result = mojo::edk::CreatePlatformHandleWrapper(
      std::move(platform_handle), &wrapped_handle);
  if (wrap_result != MOJO_RESULT_OK) {
    LOG(ERROR) << "Failed to wrap handles. Closing: " << wrap_result;
    std::move(callback).Run(mojo::ScopedHandle());
    return;
  }
  mojo::ScopedHandle scoped_handle{mojo::Handle(wrapped_handle)};

  std::move(callback).Run(std::move(scoped_handle));
}

bool ArcBluetoothBridge::IsGattServerAttributeHandleAvailable(int need) {
  return gatt_server_attribute_next_handle_ + need <= kMaxGattAttributeHandle;
}

int32_t ArcBluetoothBridge::GetNextGattServerAttributeHandle() {
  return IsGattServerAttributeHandleAvailable(1)
             ? ++gatt_server_attribute_next_handle_
             : kInvalidGattAttributeHandle;
}

template <class LocalGattAttribute>
int32_t ArcBluetoothBridge::CreateGattAttributeHandle(
    LocalGattAttribute* attribute) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!attribute)
    return kInvalidGattAttributeHandle;
  int32_t handle = GetNextGattServerAttributeHandle();
  if (handle == kInvalidGattAttributeHandle)
    return kInvalidGattAttributeHandle;
  const std::string& identifier = attribute->GetIdentifier();
  gatt_identifier_[handle] = identifier;
  gatt_handle_[identifier] = handle;
  return handle;
}

void ArcBluetoothBridge::AddService(mojom::BluetoothGattServiceIDPtr service_id,
                                    int32_t num_handles,
                                    AddServiceCallback callback) {
  if (!IsGattServerAttributeHandleAvailable(num_handles)) {
    std::move(callback).Run(kInvalidGattAttributeHandle);
    return;
  }
  base::WeakPtr<BluetoothLocalGattService> service =
      BluetoothLocalGattService::Create(
          bluetooth_adapter_.get(), service_id->id->uuid,
          service_id->is_primary, nullptr /* included_service */,
          this /* delegate */);
  std::move(callback).Run(CreateGattAttributeHandle(service.get()));
}

void ArcBluetoothBridge::AddCharacteristic(int32_t service_handle,
                                           const BluetoothUUID& uuid,
                                           int32_t properties,
                                           int32_t permissions,
                                           AddCharacteristicCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(gatt_identifier_.find(service_handle) != gatt_identifier_.end());
  if (!IsGattServerAttributeHandleAvailable(1)) {
    std::move(callback).Run(kInvalidGattAttributeHandle);
    return;
  }
  base::WeakPtr<BluetoothLocalGattCharacteristic> characteristic =
      BluetoothLocalGattCharacteristic::Create(
          uuid, properties, permissions,
          bluetooth_adapter_->GetGattService(gatt_identifier_[service_handle]));
  int32_t characteristic_handle =
      CreateGattAttributeHandle(characteristic.get());
  last_characteristic_[service_handle] = characteristic_handle;
  std::move(callback).Run(characteristic_handle);
}

void ArcBluetoothBridge::AddDescriptor(int32_t service_handle,
                                       const BluetoothUUID& uuid,
                                       int32_t permissions,
                                       AddDescriptorCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!IsGattServerAttributeHandleAvailable(1)) {
    std::move(callback).Run(kInvalidGattAttributeHandle);
    return;
  }
  // Chrome automatically adds a CCC Descriptor to a characteristic when needed.
  // We will generate a bogus handle for Android.
  if (uuid ==
      BluetoothGattDescriptor::ClientCharacteristicConfigurationUuid()) {
    int32_t handle = GetNextGattServerAttributeHandle();
    std::move(callback).Run(handle);
    return;
  }

  DCHECK(gatt_identifier_.find(service_handle) != gatt_identifier_.end());
  BluetoothLocalGattService* service =
      bluetooth_adapter_->GetGattService(gatt_identifier_[service_handle]);
  DCHECK(service);
  // Since the Android API does not give information about which characteristic
  // is the parent of the new descriptor, we assume that it would be the last
  // characteristic that was added to the given service. This matches the
  // Android framework code at android/bluetooth/BluetoothGattServer.java#594.
  // Link: https://goo.gl/cJZl1u
  DCHECK(last_characteristic_.find(service_handle) !=
         last_characteristic_.end());
  int32_t last_characteristic_handle = last_characteristic_[service_handle];

  DCHECK(gatt_identifier_.find(last_characteristic_handle) !=
         gatt_identifier_.end());
  BluetoothLocalGattCharacteristic* characteristic =
      service->GetCharacteristic(gatt_identifier_[last_characteristic_handle]);
  DCHECK(characteristic);

  base::WeakPtr<BluetoothLocalGattDescriptor> descriptor =
      BluetoothLocalGattDescriptor::Create(uuid, permissions, characteristic);
  std::move(callback).Run(CreateGattAttributeHandle(descriptor.get()));
}

void ArcBluetoothBridge::StartService(int32_t service_handle,
                                      StartServiceCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(gatt_identifier_.find(service_handle) != gatt_identifier_.end());
  BluetoothLocalGattService* service =
      bluetooth_adapter_->GetGattService(gatt_identifier_[service_handle]);
  DCHECK(service);
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  service->Register(base::Bind(&OnGattOperationDone, repeating_callback),
                    base::Bind(&OnGattOperationError, repeating_callback));
}

void ArcBluetoothBridge::StopService(int32_t service_handle,
                                     StopServiceCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(gatt_identifier_.find(service_handle) != gatt_identifier_.end());
  BluetoothLocalGattService* service =
      bluetooth_adapter_->GetGattService(gatt_identifier_[service_handle]);
  DCHECK(service);
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  service->Unregister(base::Bind(&OnGattOperationDone, repeating_callback),
                      base::Bind(&OnGattOperationError, repeating_callback));
}

void ArcBluetoothBridge::DeleteService(int32_t service_handle,
                                       DeleteServiceCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(gatt_identifier_.find(service_handle) != gatt_identifier_.end());
  BluetoothLocalGattService* service =
      bluetooth_adapter_->GetGattService(gatt_identifier_[service_handle]);
  DCHECK(service);
  gatt_identifier_.erase(service_handle);
  gatt_handle_.erase(service->GetIdentifier());
  service->Delete();
  OnGattOperationDone(std::move(callback));
}

void ArcBluetoothBridge::SendIndication(int32_t attribute_handle,
                                        mojom::BluetoothAddressPtr address,
                                        bool confirm,
                                        const std::vector<uint8_t>& value,
                                        SendIndicationCallback callback) {}

void ArcBluetoothBridge::GetSdpRecords(mojom::BluetoothAddressPtr remote_addr,
                                       const BluetoothUUID& target_uuid) {
  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(remote_addr->To<std::string>());
  if (!device) {
    OnGetServiceRecordsError(std::move(remote_addr), target_uuid,
                             bluez::BluetoothServiceRecordBlueZ::ErrorCode::
                                 ERROR_DEVICE_DISCONNECTED);
    return;
  }

  bluez::BluetoothDeviceBlueZ* device_bluez =
      static_cast<bluez::BluetoothDeviceBlueZ*>(device);

  mojom::BluetoothAddressPtr remote_addr_clone = remote_addr.Clone();

  device_bluez->GetServiceRecords(
      base::Bind(&ArcBluetoothBridge::OnGetServiceRecordsDone,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr),
                 target_uuid),
      base::Bind(&ArcBluetoothBridge::OnGetServiceRecordsError,
                 weak_factory_.GetWeakPtr(), base::Passed(&remote_addr_clone),
                 target_uuid));
}

void ArcBluetoothBridge::CreateSdpRecord(
    mojom::BluetoothSdpRecordPtr record_mojo,
    CreateSdpRecordCallback callback) {
  auto record = record_mojo.To<bluez::BluetoothServiceRecordBlueZ>();

  // Check if ServiceClassIDList attribute (attribute ID 0x0001) is included
  // after type conversion, since it is mandatory for creating a service record.
  if (!record.IsAttributePresented(kServiceClassIDListAttributeID)) {
    mojom::BluetoothCreateSdpRecordResultPtr result =
        mojom::BluetoothCreateSdpRecordResult::New();
    result->status = mojom::BluetoothStatus::FAIL;
    std::move(callback).Run(std::move(result));
    return;
  }

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  bluetooth_adapter_->CreateServiceRecord(
      record, base::Bind(&OnCreateServiceRecordDone, repeating_callback),
      base::Bind(&OnCreateServiceRecordError, repeating_callback));
}

void ArcBluetoothBridge::RemoveSdpRecord(uint32_t service_handle,
                                         RemoveSdpRecordCallback callback) {
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  bluetooth_adapter_->RemoveServiceRecord(
      service_handle,
      base::Bind(&OnRemoveServiceRecordDone, repeating_callback),
      base::Bind(&OnRemoveServiceRecordError, repeating_callback));
}

template <typename... Args>
void ArcBluetoothBridge::AddAdvertisementTask(
    base::OnceCallback<void(base::OnceCallback<void(Args...)>)> task,
    base::OnceCallback<void(Args...)> callback) {
  advertisement_task_queue_.emplace(base::BindOnce(
      std::move(task),
      base::BindOnce(&ArcBluetoothBridge::CompleteAdvertisementTask<Args...>,
                     weak_factory_.GetWeakPtr(), std::move(callback))));
  if (advertisement_task_queue_.size() != 1)
    return;
  // No task pending, run immediately.
  std::move(advertisement_task_queue_.front()).Run();
}

template <typename... Args>
void ArcBluetoothBridge::CompleteAdvertisementTask(
    base::OnceCallback<void(Args...)> callback,
    Args... args) {
  std::move(callback).Run(std::forward<Args>(args)...);
  advertisement_task_queue_.pop();  // Current task is done. Pop it from queue.
  if (advertisement_task_queue_.empty())
    return;
  std::move(advertisement_task_queue_.front()).Run();  // Run next task.
}

bool ArcBluetoothBridge::GetAdvertisementHandle(int32_t* adv_handle) {
  for (int i = 0; i < kMaxAdvertisements; i++) {
    if (advertisements_.find(i) == advertisements_.end()) {
      *adv_handle = i;
      return true;
    }
  }
  return false;
}

void ArcBluetoothBridge::ReserveAdvertisementHandle(
    ReserveAdvertisementHandleCallback callback) {
  AddAdvertisementTask(
      base::BindOnce(&ArcBluetoothBridge::ReserveAdvertisementHandleImpl,
                     weak_factory_.GetWeakPtr()),
      std::move(callback));
}

void ArcBluetoothBridge::ReserveAdvertisementHandleImpl(
    ReserveAdvertisementHandleCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // Find an empty advertisement slot.
  int32_t adv_handle;
  if (!GetAdvertisementHandle(&adv_handle)) {
    LOG(WARNING) << "Out of space for advertisement data";
    std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE,
                            kInvalidAdvertisementHandle);
    return;
  }

  // We have a handle. Put an entry in the map to reserve it.
  advertisements_[adv_handle] = nullptr;

  // The advertisement will be registered when we get the call
  // to SetAdvertisingData. For now, just return the adv_handle.
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS, adv_handle);
}

void ArcBluetoothBridge::EnableAdvertisement(
    int32_t adv_handle,
    std::unique_ptr<device::BluetoothAdvertisement::Data> advertisement,
    EnableAdvertisementCallback callback) {
  AddAdvertisementTask(
      base::BindOnce(&ArcBluetoothBridge::EnableAdvertisementImpl,
                     weak_factory_.GetWeakPtr(), adv_handle,
                     std::move(advertisement)),
      std::move(callback));
}

void ArcBluetoothBridge::EnableAdvertisementImpl(
    int32_t adv_handle,
    std::unique_ptr<device::BluetoothAdvertisement::Data> advertisement,
    EnableAdvertisementCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by
  // updating the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  base::Callback<void(void)> done_callback =
      base::Bind(&ArcBluetoothBridge::OnReadyToRegisterAdvertisement,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle,
                 base::Passed(std::move(advertisement)));
  base::Callback<void(BluetoothAdvertisement::ErrorCode)> error_callback =
      base::Bind(&ArcBluetoothBridge::OnRegisterAdvertisementError,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle);

  auto it = advertisements_.find(adv_handle);
  if (it == advertisements_.end()) {
    repeating_callback.Run(mojom::BluetoothGattStatus::GATT_FAILURE);
    return;
  }
  if (it->second == nullptr) {
    done_callback.Run();
    return;
  }
  it->second->Unregister(done_callback, error_callback);
}

void ArcBluetoothBridge::DisableAdvertisement(
    int32_t adv_handle,
    EnableAdvertisementCallback callback) {
  AddAdvertisementTask(
      base::BindOnce(&ArcBluetoothBridge::DisableAdvertisementImpl,
                     weak_factory_.GetWeakPtr(), adv_handle),
      std::move(callback));
}

void ArcBluetoothBridge::DisableAdvertisementImpl(
    int32_t adv_handle,
    EnableAdvertisementCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by
  // updating the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  base::Callback<void(void)> done_callback =
      base::Bind(&ArcBluetoothBridge::OnUnregisterAdvertisementDone,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle);
  base::Callback<void(BluetoothAdvertisement::ErrorCode)> error_callback =
      base::Bind(&ArcBluetoothBridge::OnUnregisterAdvertisementError,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle);

  auto it = advertisements_.find(adv_handle);
  if (it == advertisements_.end()) {
    repeating_callback.Run(mojom::BluetoothGattStatus::GATT_FAILURE);
    return;
  }
  if (it->second == nullptr) {
    done_callback.Run();
    return;
  }
  it->second->Unregister(done_callback, error_callback);
}

void ArcBluetoothBridge::ReleaseAdvertisementHandle(
    int32_t adv_handle,
    ReleaseAdvertisementHandleCallback callback) {
  AddAdvertisementTask(
      base::BindOnce(&ArcBluetoothBridge::ReleaseAdvertisementHandleImpl,
                     weak_factory_.GetWeakPtr(), adv_handle),
      std::move(callback));
}

void ArcBluetoothBridge::ReleaseAdvertisementHandleImpl(
    int32_t adv_handle,
    ReleaseAdvertisementHandleCallback callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (advertisements_.find(adv_handle) == advertisements_.end()) {
    std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
    return;
  }

  if (!advertisements_[adv_handle]) {
    advertisements_.erase(adv_handle);
    std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
    return;
  }

  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by
  // updating the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  advertisements_[adv_handle]->Unregister(
      base::Bind(&ArcBluetoothBridge::OnReleaseAdvertisementHandleDone,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle),
      base::Bind(&ArcBluetoothBridge::OnReleaseAdvertisementHandleError,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle));
}

void ArcBluetoothBridge::OnReadyToRegisterAdvertisement(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle,
    std::unique_ptr<device::BluetoothAdvertisement::Data> data) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  bluetooth_adapter_->RegisterAdvertisement(
      std::move(data),
      base::Bind(&ArcBluetoothBridge::OnRegisterAdvertisementDone,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle),
      base::Bind(&ArcBluetoothBridge::OnRegisterAdvertisementError,
                 weak_factory_.GetWeakPtr(), repeating_callback, adv_handle));
}

void ArcBluetoothBridge::OnRegisterAdvertisementDone(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle,
    scoped_refptr<BluetoothAdvertisement> advertisement) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisements_[adv_handle] = std::move(advertisement);
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::OnRegisterAdvertisementError(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle,
    BluetoothAdvertisement::ErrorCode error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(WARNING) << "Failed to register advertisement for handle " << adv_handle
               << ", error code = " << error_code;
  advertisements_[adv_handle] = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
}

void ArcBluetoothBridge::OnUnregisterAdvertisementDone(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisements_[adv_handle] = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::OnUnregisterAdvertisementError(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle,
    BluetoothAdvertisement::ErrorCode error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(WARNING) << "Failed to unregister advertisement for handle " << adv_handle
               << ", error code = " << error_code;
  advertisements_[adv_handle] = nullptr;
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
}

void ArcBluetoothBridge::OnReleaseAdvertisementHandleDone(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  advertisements_.erase(adv_handle);
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_SUCCESS);
}

void ArcBluetoothBridge::OnReleaseAdvertisementHandleError(
    ArcBluetoothBridge::GattStatusCallback callback,
    int32_t adv_handle,
    BluetoothAdvertisement::ErrorCode error_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(WARNING) << "Failed to relase advertisement handle " << adv_handle
               << ", error code = " << error_code;
  advertisements_.erase(adv_handle);
  std::move(callback).Run(mojom::BluetoothGattStatus::GATT_FAILURE);
}

void ArcBluetoothBridge::OnDiscoveryError() {
  LOG(WARNING) << "failed to change discovery state";
}

void ArcBluetoothBridge::OnPairing(mojom::BluetoothAddressPtr addr) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnBondStateChanged);
  if (!bluetooth_instance)
    return;

  bluetooth_instance->OnBondStateChanged(mojom::BluetoothStatus::SUCCESS,
                                         std::move(addr),
                                         mojom::BluetoothBondState::BONDING);
}

void ArcBluetoothBridge::OnPairedDone(mojom::BluetoothAddressPtr addr) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnBondStateChanged);
  if (!bluetooth_instance)
    return;

  bluetooth_instance->OnBondStateChanged(mojom::BluetoothStatus::SUCCESS,
                                         std::move(addr),
                                         mojom::BluetoothBondState::BONDED);
}

void ArcBluetoothBridge::OnPairedError(
    mojom::BluetoothAddressPtr addr,
    BluetoothDevice::ConnectErrorCode error_code) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnBondStateChanged);
  if (!bluetooth_instance)
    return;

  bluetooth_instance->OnBondStateChanged(mojom::BluetoothStatus::FAIL,
                                         std::move(addr),
                                         mojom::BluetoothBondState::NONE);
}

void ArcBluetoothBridge::OnForgetDone(mojom::BluetoothAddressPtr addr) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnBondStateChanged);
  if (!bluetooth_instance)
    return;

  bluetooth_instance->OnBondStateChanged(mojom::BluetoothStatus::SUCCESS,
                                         std::move(addr),
                                         mojom::BluetoothBondState::NONE);
}

void ArcBluetoothBridge::OnForgetError(mojom::BluetoothAddressPtr addr) const {
  auto* bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnBondStateChanged);
  if (!bluetooth_instance)
    return;

  BluetoothDevice* device =
      bluetooth_adapter_->GetDevice(addr->To<std::string>());
  mojom::BluetoothBondState bond_state = mojom::BluetoothBondState::NONE;
  if (device && device->IsPaired()) {
    bond_state = mojom::BluetoothBondState::BONDED;
  }
  bluetooth_instance->OnBondStateChanged(mojom::BluetoothStatus::FAIL,
                                         std::move(addr), bond_state);
}

bool ArcBluetoothBridge::IsPowerChangeInitiatedByRemote(
    ArcBluetoothBridge::AdapterPowerState powered) const {
  return !remote_power_changes_.empty() &&
         remote_power_changes_.front() == powered;
}

bool ArcBluetoothBridge::IsPowerChangeInitiatedByLocal(
    ArcBluetoothBridge::AdapterPowerState powered) const {
  return !local_power_changes_.empty() &&
         local_power_changes_.front() == powered;
}

void ArcBluetoothBridge::MaybeSendInitialPowerChange() {
  if (!bluetooth_adapter_ || !bluetooth_adapter_->IsPowered()) {
    // The default power state of Bluetooth on Android is off, so there is no
    // need to send an intent to turn off Bluetooth if the initial power state
    // is off.
    return;
  }

  // Send initial power state in case both, Intent Helper and App instances are
  // present. Intent Helper is required to dispatch this event and App is sign
  // that ARC is fully started. In case of initial boot, App instance is started
  // after the Intent Helper instance. In case of next boot Intent Helper and
  // App instances are started at almost the same time and order of start is not
  // determined.
  if (!arc_bridge_service_->app()->IsConnected() ||
      !arc_bridge_service_->intent_helper()->IsConnected()) {
    return;
  }

  EnqueueLocalPowerChange(AdapterPowerState::TURN_ON);
}

void ArcBluetoothBridge::EnqueueLocalPowerChange(
    ArcBluetoothBridge::AdapterPowerState powered) {
  local_power_changes_.push(powered);

  if (power_intent_timer_.IsRunning())
    return;

  SendBluetoothPoweredStateBroadcast(local_power_changes_.front());
  power_intent_timer_.Start(
      FROM_HERE, kPowerIntentTimeout,
      base::Bind(&ArcBluetoothBridge::DequeueLocalPowerChange,
                 weak_factory_.GetWeakPtr(), powered));
}

void ArcBluetoothBridge::DequeueLocalPowerChange(
    ArcBluetoothBridge::AdapterPowerState powered) {
  power_intent_timer_.Stop();

  if (!IsPowerChangeInitiatedByLocal(powered))
    return;

  AdapterPowerState current_change = local_power_changes_.front();
  AdapterPowerState last_change = local_power_changes_.back();

  // Compress the queue for power intent to reduce the amount of intents being
  // sent to Android so that the powered state will be synced between Android
  // and Chrome even if the state is toggled repeatedly on Chrome.
  base::queue<AdapterPowerState> empty_queue;
  std::swap(local_power_changes_, empty_queue);

  if (last_change == current_change)
    return;

  local_power_changes_.push(last_change);

  SendBluetoothPoweredStateBroadcast(last_change);
  power_intent_timer_.Start(
      FROM_HERE, kPowerIntentTimeout,
      base::Bind(&ArcBluetoothBridge::DequeueLocalPowerChange,
                 weak_factory_.GetWeakPtr(), last_change));
}

void ArcBluetoothBridge::EnqueueRemotePowerChange(
    ArcBluetoothBridge::AdapterPowerState powered,
    ArcBluetoothBridge::AdapterStateCallback callback) {
  remote_power_changes_.push(powered);

  bool turn_on = (powered == AdapterPowerState::TURN_ON);
  // TODO(crbug.com/730593): Remove AdaptCallbackForRepeating() by updating
  // the callee interface.
  auto repeating_callback =
      base::AdaptCallbackForRepeating(std::move(callback));
  BLUETOOTH_LOG(EVENT) << "ARC bluetooth set power: " << turn_on;
  bluetooth_adapter_->SetPowered(
      turn_on,
      base::Bind(turn_on ? &ArcBluetoothBridge::OnPoweredOn
                         : &ArcBluetoothBridge::OnPoweredOff,
                 weak_factory_.GetWeakPtr(), repeating_callback,
                 true /* save_user_pref */),
      base::Bind(&ArcBluetoothBridge::OnPoweredError,
                 weak_factory_.GetWeakPtr(), repeating_callback));
}

void ArcBluetoothBridge::DequeueRemotePowerChange(
    ArcBluetoothBridge::AdapterPowerState powered) {
  remote_power_changes_.pop();
}

std::vector<mojom::BluetoothPropertyPtr>
ArcBluetoothBridge::GetDeviceProperties(mojom::BluetoothPropertyType type,
                                        const BluetoothDevice* device) const {
  std::vector<mojom::BluetoothPropertyPtr> properties;

  if (!device) {
    return properties;
  }

  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::BDNAME) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_bdname(device->GetName() ? device->GetName().value() : "");
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::BDADDR) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_bdaddr(mojom::BluetoothAddress::From(device->GetAddress()));
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::UUIDS) {
    BluetoothDevice::UUIDSet uuids = device->GetUUIDs();
    if (uuids.size() > 0) {
      mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
      btp->set_uuids(std::vector<BluetoothUUID>(uuids.begin(), uuids.end()));
      properties.push_back(std::move(btp));
    }
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::CLASS_OF_DEVICE) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_device_class(device->GetBluetoothClass());
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::TYPE_OF_DEVICE) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_device_type(device->GetType());
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::REMOTE_FRIENDLY_NAME) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_remote_friendly_name(
        base::UTF16ToUTF8(device->GetNameForDisplay()));
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::REMOTE_RSSI) {
    base::Optional<int8_t> rssi = device->GetInquiryRSSI();
    if (rssi.has_value()) {
      mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
      btp->set_remote_rssi(rssi.value());
      properties.push_back(std::move(btp));
    }
  }
  // TODO(smbarber): Add remote version info

  return properties;
}

std::vector<mojom::BluetoothPropertyPtr>
ArcBluetoothBridge::GetAdapterProperties(
    mojom::BluetoothPropertyType type) const {
  std::vector<mojom::BluetoothPropertyPtr> properties;

  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::BDNAME) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    std::string name = bluetooth_adapter_->GetName();
    btp->set_bdname(name);
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::BDADDR) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_bdaddr(
        mojom::BluetoothAddress::From(bluetooth_adapter_->GetAddress()));
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::UUIDS) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_uuids(bluetooth_adapter_->GetUUIDs());
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::CLASS_OF_DEVICE) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_device_class(kBluetoothComputerClass);
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::TYPE_OF_DEVICE) {
    // Assume that all ChromeOS devices are dual mode Bluetooth device.
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_device_type(device::BLUETOOTH_TRANSPORT_DUAL);
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::ADAPTER_SCAN_MODE) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    mojom::BluetoothScanMode scan_mode = mojom::BluetoothScanMode::CONNECTABLE;

    if (bluetooth_adapter_->IsDiscoverable())
      scan_mode = mojom::BluetoothScanMode::CONNECTABLE_DISCOVERABLE;

    btp->set_adapter_scan_mode(scan_mode);
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::ADAPTER_BONDED_DEVICES) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    BluetoothAdapter::DeviceList devices = bluetooth_adapter_->GetDevices();

    std::vector<mojom::BluetoothAddressPtr> bonded_devices;

    for (auto* device : devices) {
      if (!device->IsPaired())
        continue;

      mojom::BluetoothAddressPtr addr =
          mojom::BluetoothAddress::From(device->GetAddress());
      bonded_devices.push_back(std::move(addr));
    }

    btp->set_bonded_devices(std::move(bonded_devices));
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::ADAPTER_DISCOVERY_TIMEOUT) {
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    btp->set_discovery_timeout(bluetooth_adapter_->GetDiscoverableTimeout());
    properties.push_back(std::move(btp));
  }
  if (type == mojom::BluetoothPropertyType::ALL ||
      type == mojom::BluetoothPropertyType::LOCAL_LE_FEATURES) {
    // TODO(crbug.com/637171) Investigate all the le_features.
    mojom::BluetoothPropertyPtr btp = mojom::BluetoothProperty::New();
    mojom::BluetoothLocalLEFeaturesPtr le_features =
        mojom::BluetoothLocalLEFeatures::New();
    le_features->version_supported = kAndroidMBluetoothVersionNumber;
    le_features->local_privacy_enabled = 0;
    le_features->max_adv_instance = kMaxAdvertisements;
    le_features->rpa_offload_supported = 0;
    le_features->max_irk_list_size = 0;
    le_features->max_adv_filter_supported = 0;
    le_features->activity_energy_info_supported = 0;
    le_features->scan_result_storage_size = 0;
    le_features->total_trackable_advertisers = 0;
    le_features->extended_scan_support = false;
    le_features->debug_logging_supported = false;
    btp->set_local_le_features(std::move(le_features));
    properties.push_back(std::move(btp));
  }

  return properties;
}

// Android support 6 types of Advertising Data which are Advertising Data Flags,
// Local Name, Service UUIDs, Tx Power Level, Service Data, and Manufacturer
// Data. Note that we need to use 16-bit UUID in Service Data section because
// Android does not support 128-bit UUID there.
std::vector<mojom::BluetoothAdvertisingDataPtr>
ArcBluetoothBridge::GetAdvertisingData(const BluetoothDevice* device) const {
  std::vector<mojom::BluetoothAdvertisingDataPtr> advertising_data;

  // Advertising Data Flags
  if (device->GetAdvertisingDataFlags().has_value()) {
    mojom::BluetoothAdvertisingDataPtr flags =
        mojom::BluetoothAdvertisingData::New();
    flags->set_flags(device->GetAdvertisingDataFlags().value());
    advertising_data.push_back(std::move(flags));
  }

  // Local Name
  mojom::BluetoothAdvertisingDataPtr local_name =
      mojom::BluetoothAdvertisingData::New();
  local_name->set_local_name(device->GetName() ? device->GetName().value()
                                               : "");
  advertising_data.push_back(std::move(local_name));

  // Service UUIDs
  const BluetoothDevice::UUIDSet& uuid_set = device->GetUUIDs();
  if (uuid_set.size() > 0) {
    mojom::BluetoothAdvertisingDataPtr service_uuids =
        mojom::BluetoothAdvertisingData::New();
    service_uuids->set_service_uuids(
        std::vector<BluetoothUUID>(uuid_set.begin(), uuid_set.end()));
    advertising_data.push_back(std::move(service_uuids));
  }

  // Tx Power Level
  if (device->GetInquiryTxPower().has_value()) {
    mojom::BluetoothAdvertisingDataPtr tx_power_level_element =
        mojom::BluetoothAdvertisingData::New();
    tx_power_level_element->set_tx_power_level(
        device->GetInquiryTxPower().value());
    advertising_data.push_back(std::move(tx_power_level_element));
  }

  // Service Data
  for (const BluetoothUUID& uuid : device->GetServiceDataUUIDs()) {
    mojom::BluetoothAdvertisingDataPtr service_data_element =
        mojom::BluetoothAdvertisingData::New();
    mojom::BluetoothServiceDataPtr service_data =
        mojom::BluetoothServiceData::New();

    // Android only supports UUID 16 bit here.
    service_data->uuid_16bit = GetUUID16(uuid);

    const std::vector<uint8_t>* data = device->GetServiceDataForUUID(uuid);
    DCHECK(data != nullptr);

    service_data->data = *data;

    service_data_element->set_service_data(std::move(service_data));
    advertising_data.push_back(std::move(service_data_element));
  }

  // Manufacturer Data
  if (!device->GetManufacturerData().empty()) {
    std::vector<uint8_t> manufacturer_data;
    for (const auto& pair : device->GetManufacturerData()) {
      uint16_t id = pair.first;
      // Use little endian here.
      manufacturer_data.push_back(id & 0xff);
      manufacturer_data.push_back(id >> 8);
      manufacturer_data.insert(manufacturer_data.end(), pair.second.begin(),
                               pair.second.end());
    }
    mojom::BluetoothAdvertisingDataPtr manufacturer_data_element =
        mojom::BluetoothAdvertisingData::New();
    manufacturer_data_element->set_manufacturer_data(manufacturer_data);
    advertising_data.push_back(std::move(manufacturer_data_element));
  }

  return advertising_data;
}

void ArcBluetoothBridge::SendCachedDevicesFound() const {
  DCHECK(bluetooth_adapter_);

  // Send devices that have already been discovered, but aren't connected.
  BluetoothAdapter::DeviceList devices = bluetooth_adapter_->GetDevices();
  for (auto* device : devices) {
    if (device->IsPaired())
      continue;

    SendDevice(device);
  }
}

void ArcBluetoothBridge::SendCachedPairedDevices() const {
  DCHECK(bluetooth_adapter_);

  BluetoothAdapter::DeviceList devices = bluetooth_adapter_->GetDevices();
  for (auto* device : devices) {
    if (!device->IsPaired())
      continue;

    SendDevice(device);

    // OnBondStateChanged must be called with mojom::BluetoothBondState::BONDING
    // to make sure the bond state machine on Android is ready to take the
    // pair-done event. Otherwise the pair-done event will be dropped as an
    // invalid change of paired status.
    mojom::BluetoothAddressPtr addr =
        mojom::BluetoothAddress::From(device->GetAddress());
    OnPairing(addr->Clone());
    OnPairedDone(std::move(addr));
  }
}

void ArcBluetoothBridge::OnGetServiceRecordsDone(
    mojom::BluetoothAddressPtr remote_addr,
    const BluetoothUUID& target_uuid,
    const std::vector<bluez::BluetoothServiceRecordBlueZ>& records_bluez) {
  auto* sdp_bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnGetSdpRecords);
  if (!sdp_bluetooth_instance)
    return;

  std::vector<mojom::BluetoothSdpRecordPtr> records;
  for (const auto& r : records_bluez)
    records.push_back(mojom::BluetoothSdpRecord::From(r));

  sdp_bluetooth_instance->OnGetSdpRecords(mojom::BluetoothStatus::SUCCESS,
                                          std::move(remote_addr), target_uuid,
                                          std::move(records));
}

void ArcBluetoothBridge::OnGetServiceRecordsError(
    mojom::BluetoothAddressPtr remote_addr,
    const BluetoothUUID& target_uuid,
    bluez::BluetoothServiceRecordBlueZ::ErrorCode error_code) {
  auto* sdp_bluetooth_instance = ARC_GET_INSTANCE_FOR_METHOD(
      arc_bridge_service_->bluetooth(), OnGetSdpRecords);
  if (!sdp_bluetooth_instance)
    return;

  mojom::BluetoothStatus status;

  switch (error_code) {
    case bluez::BluetoothServiceRecordBlueZ::ErrorCode::ERROR_ADAPTER_NOT_READY:
      status = mojom::BluetoothStatus::NOT_READY;
      break;
    case bluez::BluetoothServiceRecordBlueZ::ErrorCode::
        ERROR_DEVICE_DISCONNECTED:
      status = mojom::BluetoothStatus::RMT_DEV_DOWN;
      break;
    default:
      status = mojom::BluetoothStatus::FAIL;
      break;
  }

  sdp_bluetooth_instance->OnGetSdpRecords(
      status, std::move(remote_addr), target_uuid,
      std::vector<mojom::BluetoothSdpRecordPtr>());
}

void ArcBluetoothBridge::SetPrimaryUserBluetoothPowerSetting(
    bool enabled) const {
  const user_manager::User* const user =
      user_manager::UserManager::Get()->GetPrimaryUser();
  Profile* profile = chromeos::ProfileHelper::Get()->GetProfileByUser(user);
  DCHECK(profile);
  profile->GetPrefs()->SetBoolean(ash::prefs::kUserBluetoothAdapterEnabled,
                                  enabled);
}

ArcBluetoothBridge::GattConnection::GattConnection(
    ArcBluetoothBridge::GattConnection::ConnectionState state,
    std::unique_ptr<device::BluetoothGattConnection> connection)
    : state(state), connection(std::move(connection)) {}
ArcBluetoothBridge::GattConnection::GattConnection() = default;
ArcBluetoothBridge::GattConnection::~GattConnection() = default;
ArcBluetoothBridge::GattConnection::GattConnection(
    ArcBluetoothBridge::GattConnection&&) = default;
ArcBluetoothBridge::GattConnection& ArcBluetoothBridge::GattConnection::
operator=(ArcBluetoothBridge::GattConnection&&) = default;

}  // namespace arc
