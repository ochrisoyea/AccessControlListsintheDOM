// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/mirroring/browser/single_client_video_capture_host.h"

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_media_capture_id.h"
#include "media/capture/video/video_capture_buffer_pool.h"

using media::VideoFrameConsumerFeedbackObserver;

namespace mirroring {

namespace {

class DeviceLauncherCallbacks final
    : public content::VideoCaptureDeviceLauncher::Callbacks {
 public:
  explicit DeviceLauncherCallbacks(
      base::WeakPtr<SingleClientVideoCaptureHost> host)
      : video_capture_host_(host) {}

  ~DeviceLauncherCallbacks() override {}

  // content::VideoCaptureDeviceLauncher::Callbacks implementations
  void OnDeviceLaunched(
      std::unique_ptr<content::LaunchedVideoCaptureDevice> device) override {
    if (video_capture_host_)
      video_capture_host_->OnDeviceLaunched(std::move(device));
  }

  void OnDeviceLaunchFailed() override {
    if (video_capture_host_)
      video_capture_host_->OnDeviceLaunchFailed();
  }

  void OnDeviceLaunchAborted() override {
    if (video_capture_host_)
      video_capture_host_->OnDeviceLaunchAborted();
  }

 private:
  base::WeakPtr<SingleClientVideoCaptureHost> video_capture_host_;

  DISALLOW_COPY_AND_ASSIGN(DeviceLauncherCallbacks);
};

}  // namespace

SingleClientVideoCaptureHost::SingleClientVideoCaptureHost(
    const std::string& device_id,
    content::MediaStreamType type,
    const VideoCaptureParams& params,
    DeviceLauncherCreateCallback callback)
    : device_id_(device_id),
      type_(type),
      params_(params),
      device_launcher_callback_(std::move(callback)),
      weak_factory_(this) {
  DCHECK(!device_launcher_callback_.is_null());
}

SingleClientVideoCaptureHost::~SingleClientVideoCaptureHost() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Stop(0);
}

void SingleClientVideoCaptureHost::Start(
    int32_t device_id,
    int32_t session_id,
    const VideoCaptureParams& params,
    media::mojom::VideoCaptureObserverPtr observer) {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!observer_);
  observer_ = std::move(observer);
  DCHECK(observer_);
  DCHECK(!launched_device_);

  // Start a video capture device.
  auto device_launcher_callbacks =
      std::make_unique<DeviceLauncherCallbacks>(weak_factory_.GetWeakPtr());
  DeviceLauncherCallbacks* callbacks = device_launcher_callbacks.get();
  std::unique_ptr<content::VideoCaptureDeviceLauncher> device_launcher =
      device_launcher_callback_.Run();
  content::VideoCaptureDeviceLauncher* launcher = device_launcher.get();
  launcher->LaunchDeviceAsync(
      device_id_, type_, params_, weak_factory_.GetWeakPtr(),
      base::BindOnce(&SingleClientVideoCaptureHost::OnError,
                     weak_factory_.GetWeakPtr()),
      callbacks,
      // The |device_launcher| and |device_launcher_callbacks| must be kept
      // alive until the device launching completes.
      base::BindOnce([](std::unique_ptr<content::VideoCaptureDeviceLauncher>,
                        std::unique_ptr<DeviceLauncherCallbacks>) {},
                     std::move(device_launcher),
                     std::move(device_launcher_callbacks)));
}

void SingleClientVideoCaptureHost::Stop(int32_t device_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(1) << __func__;

  if (!observer_)
    return;

  // Returns all the buffers.
  for (const auto& entry : buffer_context_map_) {
    OnFinishedConsumingBuffer(
        entry.first,
        media::VideoFrameConsumerFeedbackObserver::kNoUtilizationRecorded);
  }
  observer_->OnStateChanged(media::mojom::VideoCaptureState::ENDED);
  observer_ = nullptr;
  weak_factory_.InvalidateWeakPtrs();
  launched_device_ = nullptr;
  id_map_.clear();
  retired_buffers_.clear();
}

void SingleClientVideoCaptureHost::Pause(int32_t device_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (launched_device_)
    launched_device_->MaybeSuspendDevice();
}

void SingleClientVideoCaptureHost::Resume(int32_t device_id,
                                          int32_t session_id,
                                          const VideoCaptureParams& params) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (launched_device_)
    launched_device_->ResumeDevice();
}

void SingleClientVideoCaptureHost::RequestRefreshFrame(int32_t device_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (launched_device_)
    launched_device_->RequestRefreshFrame();
}

void SingleClientVideoCaptureHost::ReleaseBuffer(
    int32_t device_id,
    int32_t buffer_id,
    double consumer_resource_utilization) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  OnFinishedConsumingBuffer(buffer_id, consumer_resource_utilization);
}

void SingleClientVideoCaptureHost::GetDeviceSupportedFormats(
    int32_t device_id,
    int32_t session_id,
    GetDeviceSupportedFormatsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NOTIMPLEMENTED();
  std::move(callback).Run(media::VideoCaptureFormats());
}

void SingleClientVideoCaptureHost::GetDeviceFormatsInUse(
    int32_t device_id,
    int32_t session_id,
    GetDeviceFormatsInUseCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NOTIMPLEMENTED();
  std::move(callback).Run(media::VideoCaptureFormats());
}

void SingleClientVideoCaptureHost::OnNewBuffer(
    int buffer_id,
    media::mojom::VideoBufferHandlePtr buffer_handle) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;
  DCHECK(observer_);
  DCHECK_NE(buffer_id, media::VideoCaptureBufferPool::kInvalidId);
  const auto insert_result =
      id_map_.emplace(std::make_pair(buffer_id, next_buffer_context_id_));
  DCHECK(insert_result.second);
  observer_->OnNewBuffer(next_buffer_context_id_++, std::move(buffer_handle));
}

void SingleClientVideoCaptureHost::OnFrameReadyInBuffer(
    int buffer_id,
    int frame_feedback_id,
    std::unique_ptr<Buffer::ScopedAccessPermission> buffer_read_permission,
    media::mojom::VideoFrameInfoPtr frame_info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;
  DCHECK(observer_);
  const auto id_iter = id_map_.find(buffer_id);
  DCHECK(id_iter != id_map_.end());
  const int buffer_context_id = id_iter->second;
  const auto insert_result = buffer_context_map_.emplace(std::make_pair(
      buffer_context_id,
      std::make_pair(frame_feedback_id, std::move(buffer_read_permission))));
  DCHECK(insert_result.second);
  observer_->OnBufferReady(buffer_context_id, std::move(frame_info));
}

void SingleClientVideoCaptureHost::OnBufferRetired(int buffer_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  const auto id_iter = id_map_.find(buffer_id);
  DCHECK(id_iter != id_map_.end());
  const int buffer_context_id = id_iter->second;
  id_map_.erase(id_iter);
  if (buffer_context_map_.find(buffer_context_id) ==
      buffer_context_map_.end()) {
    DCHECK(observer_);
    observer_->OnBufferDestroyed(buffer_context_id);
  } else {
    // The consumer is still using the buffer. The BufferContext needs to be
    // held until the consumer finishes.
    const auto insert_result = retired_buffers_.insert(buffer_context_id);
    DCHECK(insert_result.second);
  }
}

void SingleClientVideoCaptureHost::OnError() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(1) << __func__;
  if (observer_)
    observer_->OnStateChanged(media::mojom::VideoCaptureState::FAILED);
}

void SingleClientVideoCaptureHost::OnLog(const std::string& message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(3) << message;
}

void SingleClientVideoCaptureHost::OnStarted() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << __func__;
  DCHECK(observer_);
  observer_->OnStateChanged(media::mojom::VideoCaptureState::STARTED);
}

void SingleClientVideoCaptureHost::OnStartedUsingGpuDecode() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NOTIMPLEMENTED();
}

void SingleClientVideoCaptureHost::OnDeviceLaunched(
    std::unique_ptr<content::LaunchedVideoCaptureDevice> device) {
  DVLOG(1) << __func__;
  launched_device_ = std::move(device);
}

void SingleClientVideoCaptureHost::OnDeviceLaunchFailed() {
  DVLOG(1) << __func__;
  launched_device_ = nullptr;
  OnError();
}

void SingleClientVideoCaptureHost::OnDeviceLaunchAborted() {
  DVLOG(1) << __func__;
  launched_device_ = nullptr;
  OnError();
}

void SingleClientVideoCaptureHost::OnFinishedConsumingBuffer(
    int buffer_context_id,
    double consumer_resource_utilization) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(observer_);
  const auto buffer_context_iter = buffer_context_map_.find(buffer_context_id);
  if (buffer_context_iter == buffer_context_map_.end()) {
    // Stop() must have been called before.
    DCHECK(!launched_device_);
    return;
  }
  VideoFrameConsumerFeedbackObserver* feedback_observer =
      launched_device_.get();
  if (feedback_observer &&
      consumer_resource_utilization !=
          VideoFrameConsumerFeedbackObserver::kNoUtilizationRecorded) {
    feedback_observer->OnUtilizationReport(buffer_context_iter->second.first,
                                           consumer_resource_utilization);
  }
  buffer_context_map_.erase(buffer_context_iter);
  const auto retired_iter = retired_buffers_.find(buffer_context_id);
  if (retired_iter != retired_buffers_.end()) {
    retired_buffers_.erase(retired_iter);
    observer_->OnBufferDestroyed(buffer_context_id);
  }
}

}  // namespace mirroring
