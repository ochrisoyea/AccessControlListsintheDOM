// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/router/discovery/dial/dial_media_sink_service_impl.h"

#include <algorithm>

#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/media/router/discovery/dial/dial_device_data.h"
#include "services/service_manager/public/cpp/connector.h"

namespace media_router {

using SinkAppStatus = DialMediaSinkServiceImpl::SinkAppStatus;

namespace {

static constexpr const char* kDiscoveryOnlyModelNames[3] = {
    "eureka dongle", "chromecast audio", "chromecast ultra"};

// Returns true if DIAL (SSDP) was only used to discover this sink, and it is
// not expected to support other DIAL features (app discovery, activity
// discovery, etc.)
// |model_name|: device model name.
bool IsDiscoveryOnly(const std::string& model_name) {
  std::string lower_model_name = base::ToLowerASCII(model_name);
  return std::find(std::begin(kDiscoveryOnlyModelNames),
                   std::end(kDiscoveryOnlyModelNames),
                   lower_model_name) != std::end(kDiscoveryOnlyModelNames);
}

SinkAppStatus GetSinkAppStatusFromResponse(const DialAppInfoResult& result) {
  if (!result.app_info) {
    if (result.result_code == DialAppInfoResultCode::kParsingError ||
        result.result_code == DialAppInfoResultCode::kNotFound) {
      return SinkAppStatus::kUnavailable;
    } else {
      return SinkAppStatus::kUnknown;
    }
  }

  return (result.app_info->state == DialAppState::kRunning ||
          result.app_info->state == DialAppState::kStopped)
             ? SinkAppStatus::kAvailable
             : SinkAppStatus::kUnavailable;
}

}  // namespace

DialMediaSinkServiceImpl::DialMediaSinkServiceImpl(
    std::unique_ptr<service_manager::Connector> connector,
    const OnSinksDiscoveredCallback& on_sinks_discovered_cb,
    const scoped_refptr<base::SequencedTaskRunner>& task_runner)
    : MediaSinkServiceBase(on_sinks_discovered_cb),
      connector_(std::move(connector)),
      task_runner_(task_runner) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

DialMediaSinkServiceImpl::~DialMediaSinkServiceImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (dial_registry_) {
    dial_registry_->OnListenerRemoved();
    dial_registry_->UnregisterObserver(this);
    dial_registry_ = nullptr;
  }
}

void DialMediaSinkServiceImpl::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (dial_registry_)
    return;

  description_service_ = std::make_unique<DeviceDescriptionService>(
      connector_.get(),
      base::BindRepeating(
          &DialMediaSinkServiceImpl::OnDeviceDescriptionAvailable,
          base::Unretained(this)),
      base::BindRepeating(&DialMediaSinkServiceImpl::OnDeviceDescriptionError,
                          base::Unretained(this)));

  app_discovery_service_ =
      std::make_unique<DialAppDiscoveryService>(connector_.get());

  StartTimer();

  dial_registry_ =
      test_dial_registry_ ? test_dial_registry_ : DialRegistry::GetInstance();
  dial_registry_->RegisterObserver(this);
  dial_registry_->OnListenerAdded();
}

void DialMediaSinkServiceImpl::OnUserGesture() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RescanAppInfo();
}

DialMediaSinkServiceImpl::SinkQueryByAppSubscription
DialMediaSinkServiceImpl::StartMonitoringAvailableSinksForApp(
    const std::string& app_name,
    const SinkQueryByAppCallback& callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto& callback_list = sink_queries_[app_name];
  if (!callback_list) {
    callback_list = std::make_unique<SinkQueryByAppCallbackList>();
    callback_list->set_removal_callback(base::BindRepeating(
        &DialMediaSinkServiceImpl::MaybeRemoveSinkQueryCallbackList,
        base::Unretained(this), app_name, callback_list.get()));

    // Start checking if |app_name| is available on existing sinks.
    for (const auto& sink : GetSinks())
      FetchAppInfoForSink(sink.second, app_name);
  }

  return callback_list->Add(callback);
}

void DialMediaSinkServiceImpl::SetDialRegistryForTest(
    DialRegistry* dial_registry) {
  DCHECK(!test_dial_registry_);
  test_dial_registry_ = dial_registry;
}

void DialMediaSinkServiceImpl::SetDescriptionServiceForTest(
    std::unique_ptr<DeviceDescriptionService> description_service) {
  description_service_ = std::move(description_service);
}

void DialMediaSinkServiceImpl::SetAppDiscoveryServiceForTest(
    std::unique_ptr<DialAppDiscoveryService> app_discovery_service) {
  app_discovery_service_ = std::move(app_discovery_service);
}

void DialMediaSinkServiceImpl::OnDiscoveryComplete() {
  std::vector<MediaSinkInternal> sinks_to_update;
  std::vector<MediaSinkInternal> sinks_to_remove;
  for (const auto& sink : GetSinks()) {
    if (!base::ContainsKey(latest_sinks_, sink.first))
      sinks_to_remove.push_back(sink.second);
  }

  for (const auto& latest_sink : latest_sinks_) {
    // Sink is added or updated.
    const MediaSinkInternal* sink = GetSinkById(latest_sink.first);
    if (!sink || *sink != latest_sink.second)
      sinks_to_update.push_back(latest_sink.second);
  }

  // Note: calling |AddOrUpdateSink()| or |RemoveSink()| here won't cause the
  // discovery timer to fire again, since it is considered to be still running.
  for (const auto& sink : sinks_to_update)
    AddOrUpdateSink(sink);

  for (const auto& sink : sinks_to_remove)
    RemoveSink(sink);

  latest_sinks_.clear();

  // If discovered sinks are updated, then query results might have changed.
  for (const auto& query : sink_queries_)
    query.second->Notify(query.first);

  MediaSinkServiceBase::OnDiscoveryComplete();
}

void DialMediaSinkServiceImpl::OnDialDeviceEvent(
    const DialRegistry::DeviceList& devices) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << "DialMediaSinkServiceImpl::OnDialDeviceEvent found "
           << devices.size() << " devices";

  current_devices_ = devices;

  description_service_->GetDeviceDescriptions(devices);

  // Makes sure the timer fires even if there is no device.
  StartTimer();
}

void DialMediaSinkServiceImpl::OnDialError(DialRegistry::DialErrorCode type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << "OnDialError [DialErrorCode]: " << static_cast<int>(type);
}

void DialMediaSinkServiceImpl::OnDeviceDescriptionAvailable(
    const DialDeviceData& device_data,
    const ParsedDialDeviceDescription& description_data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!base::ContainsValue(current_devices_, device_data)) {
    DVLOG(2) << "Device data not found in current device data list...";
    return;
  }

  std::string processed_uuid =
      MediaSinkInternal::ProcessDeviceUUID(description_data.unique_id);
  MediaSink::Id sink_id =
      base::StringPrintf("dial:<%s>", processed_uuid.c_str());
  MediaSink sink(sink_id, description_data.friendly_name, SinkIconType::GENERIC,
                 MediaRouteProviderId::DIAL);
  DialSinkExtraData extra_data;
  extra_data.app_url = description_data.app_url;
  extra_data.model_name = description_data.model_name;
  std::string ip_address = device_data.device_description_url().host();
  if (!extra_data.ip_address.AssignFromIPLiteral(ip_address)) {
    DVLOG(1) << "Invalid ip_address: " << ip_address;
    return;
  }

  MediaSinkInternal dial_sink(sink, extra_data);
  latest_sinks_.insert_or_assign(sink_id, dial_sink);
  StartTimer();

  if (!IsDiscoveryOnly(description_data.model_name)) {
    // Start checking if all registered apps are available on |dial_sink|.
    for (const auto& query : sink_queries_)
      FetchAppInfoForSink(dial_sink, query.first);
  }
}

void DialMediaSinkServiceImpl::OnDeviceDescriptionError(
    const DialDeviceData& device,
    const std::string& error_message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << "OnDeviceDescriptionError [message]: " << error_message;
}

void DialMediaSinkServiceImpl::OnAppInfoParseCompleted(
    const std::string& sink_id,
    const std::string& app_name,
    DialAppInfoResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  SinkAppStatus app_status = GetSinkAppStatusFromResponse(result);
  if (app_status == SinkAppStatus::kUnknown) {
    DVLOG(2) << "Unknown app status for " << sink_id << ", " << app_name;
    return;
  }

  DVLOG(2) << "Get parsed DIAL app info from, [sink_id]: " << sink_id
           << " [name]: " << app_name << " [status]: " << app_status;

  SinkAppStatus old_status = GetAppStatus(sink_id, app_name);
  SetAppStatus(sink_id, app_name, app_status);

  if (old_status == app_status)
    return;

  // The sink might've been removed before the parse was complete. In that case
  // the callbacks won't be notified, but the app status will be saved for later
  // use.
  if (!GetSinkById(sink_id))
    return;

  auto query_it = sink_queries_.find(app_name);
  if (query_it != sink_queries_.end())
    query_it->second->Notify(app_name);
}

void DialMediaSinkServiceImpl::FetchAppInfoForSink(
    const MediaSinkInternal& dial_sink,
    const std::string& app_name) {
  std::string model_name = dial_sink.dial_data().model_name;
  if (IsDiscoveryOnly(model_name)) {
    DVLOG(2) << "Model name does not support DIAL app availability: "
             << model_name;
    return;
  }

  std::string sink_id = dial_sink.sink().id();
  SinkAppStatus app_status = GetAppStatus(sink_id, app_name);
  if (app_status != SinkAppStatus::kUnknown)
    return;

  app_discovery_service_->FetchDialAppInfo(
      dial_sink, app_name,
      base::BindOnce(&DialMediaSinkServiceImpl::OnAppInfoParseCompleted,
                     base::Unretained(this)));
}

void DialMediaSinkServiceImpl::RescanAppInfo() {
  for (const auto& sink : GetSinks()) {
    std::string model_name = sink.second.dial_data().model_name;
    if (IsDiscoveryOnly(model_name)) {
      DVLOG(2) << "Model name does not support DIAL app availability: "
               << model_name;
      continue;
    }

    for (const auto& query : sink_queries_)
      FetchAppInfoForSink(sink.second, query.first);
  }
}

SinkAppStatus DialMediaSinkServiceImpl::GetAppStatus(
    const std::string& sink_id,
    const std::string& app_name) const {
  std::string key = sink_id + ':' + app_name;
  auto status_it = app_statuses_.find(key);
  return status_it == app_statuses_.end() ? SinkAppStatus::kUnknown
                                          : status_it->second;
}

void DialMediaSinkServiceImpl::SetAppStatus(const std::string& sink_id,
                                            const std::string& app_name,
                                            SinkAppStatus app_status) {
  std::string key = sink_id + ':' + app_name;
  app_statuses_[key] = app_status;
}

void DialMediaSinkServiceImpl::RecordDeviceCounts() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  metrics_.RecordDeviceCountsIfNeeded(GetSinks().size(),
                                      current_devices_.size());
}

void DialMediaSinkServiceImpl::MaybeRemoveSinkQueryCallbackList(
    const std::string& app_name,
    SinkQueryByAppCallbackList* callback_list) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // There are no more profiles monitoring |app_name|.
  if (callback_list->empty())
    sink_queries_.erase(app_name);
}

std::vector<MediaSinkInternal> DialMediaSinkServiceImpl::GetAvailableSinks(
    const std::string& app_name) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<MediaSinkInternal> sinks;
  for (const auto& sink : GetSinks()) {
    if (GetAppStatus(sink.first, app_name) == SinkAppStatus::kAvailable)
      sinks.push_back(sink.second);
  }
  return sinks;
}

}  // namespace media_router
