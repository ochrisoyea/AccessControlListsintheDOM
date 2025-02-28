// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"

#include <stddef.h>
#include <stdint.h>

#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "components/viz/service/display/display.h"
#include "components/viz/service/display_embedder/display_provider.h"
#include "components/viz/service/display_embedder/external_begin_frame_controller_impl.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_impl.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_support.h"
#include "components/viz/service/frame_sinks/primary_begin_frame_source.h"
#include "components/viz/service/frame_sinks/root_compositor_frame_sink_impl.h"
#include "components/viz/service/frame_sinks/video_capture/capturable_frame_sink.h"
#include "components/viz/service/frame_sinks/video_capture/frame_sink_video_capturer_impl.h"

#if DCHECK_IS_ON()
#include <sstream>
#endif

namespace viz {

FrameSinkManagerImpl::FrameSinkSourceMapping::FrameSinkSourceMapping() =
    default;

FrameSinkManagerImpl::FrameSinkSourceMapping::FrameSinkSourceMapping(
    FrameSinkSourceMapping&& other) = default;

FrameSinkManagerImpl::FrameSinkSourceMapping::~FrameSinkSourceMapping() =
    default;

FrameSinkManagerImpl::FrameSinkSourceMapping&
FrameSinkManagerImpl::FrameSinkSourceMapping::operator=(
    FrameSinkSourceMapping&& other) = default;

FrameSinkManagerImpl::FrameSinkManagerImpl(
    base::Optional<uint32_t> activation_deadline_in_frames,
    DisplayProvider* display_provider)
    : display_provider_(display_provider),
      surface_manager_(activation_deadline_in_frames),
      hit_test_manager_(surface_manager()),
      binding_(this) {
  surface_manager_.AddObserver(&hit_test_manager_);
  surface_manager_.AddObserver(this);
}

FrameSinkManagerImpl::~FrameSinkManagerImpl() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  video_capturers_.clear();

  // All mojom::CompositorFrameSinks and BeginFrameSources should be deleted by
  // this point.
  DCHECK(sink_map_.empty());
  DCHECK(registered_sources_.empty());

  surface_manager_.RemoveObserver(this);
  surface_manager_.RemoveObserver(&hit_test_manager_);
}

void FrameSinkManagerImpl::BindAndSetClient(
    mojom::FrameSinkManagerRequest request,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    mojom::FrameSinkManagerClientPtr client) {
  DCHECK(!client_);
  DCHECK(!binding_.is_bound());
  binding_.Bind(std::move(request), std::move(task_runner));
  client_ptr_ = std::move(client);

  client_ = client_ptr_.get();
}

void FrameSinkManagerImpl::SetLocalClient(
    mojom::FrameSinkManagerClient* client) {
  DCHECK(!client_ptr_);

  client_ = client;
}

void FrameSinkManagerImpl::RegisterFrameSinkId(
    const FrameSinkId& frame_sink_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  surface_manager_.RegisterFrameSinkId(frame_sink_id);
  if (video_detector_)
    video_detector_->OnFrameSinkIdRegistered(frame_sink_id);

  for (auto& observer : observer_list_)
    observer.OnRegisteredFrameSinkId(frame_sink_id);
}

void FrameSinkManagerImpl::InvalidateFrameSinkId(
    const FrameSinkId& frame_sink_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  for (auto& observer : observer_list_)
    observer.OnInvalidatedFrameSinkId(frame_sink_id);

  surface_manager_.InvalidateFrameSinkId(frame_sink_id);
  if (video_detector_)
    video_detector_->OnFrameSinkIdInvalidated(frame_sink_id);

  synchronization_event_labels_.erase(frame_sink_id);

  // Destroy the [Root]CompositorFrameSinkImpl if there is one.
  sink_map_.erase(frame_sink_id);
}

void FrameSinkManagerImpl::EnableSynchronizationReporting(
    const FrameSinkId& frame_sink_id,
    const std::string& reporting_label) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // TODO(fsamuel): We should move FrameSink labels over to
  // FrameSinkManagerImpl and unify them with synchronization event labels
  // and other metadata about FrameSinks.
  DCHECK_GT(surface_manager_.valid_frame_sink_labels().count(frame_sink_id),
            0u);
  synchronization_event_labels_.emplace(frame_sink_id, reporting_label);
}

void FrameSinkManagerImpl::SetFrameSinkDebugLabel(
    const FrameSinkId& frame_sink_id,
    const std::string& debug_label) {
  surface_manager_.SetFrameSinkDebugLabel(frame_sink_id, debug_label);
}

void FrameSinkManagerImpl::CreateRootCompositorFrameSink(
    mojom::RootCompositorFrameSinkParamsPtr params) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!base::ContainsKey(sink_map_, params->frame_sink_id));
  DCHECK(display_provider_);

  std::unique_ptr<ExternalBeginFrameControllerImpl>
      external_begin_frame_controller;
  if (params->external_begin_frame_controller.is_pending() &&
      params->external_begin_frame_controller_client) {
    external_begin_frame_controller =
        std::make_unique<ExternalBeginFrameControllerImpl>(
            std::move(params->external_begin_frame_controller),
            mojom::ExternalBeginFrameControllerClientPtr(
                std::move(params->external_begin_frame_controller_client)));
  }

  mojom::DisplayClientPtr display_client(std::move(params->display_client));

  std::unique_ptr<SyntheticBeginFrameSource> begin_frame_source;
  auto display = display_provider_->CreateDisplay(
      params->frame_sink_id, params->widget, params->gpu_compositing,
      display_client.get(), external_begin_frame_controller.get(),
      params->renderer_settings, &begin_frame_source);

  // Creating display failed. Drop the CompositorFrameSink message pipes here
  // and let host send a new request, potential with a different compositing
  // mode.
  if (!display)
    return;

  sink_map_[params->frame_sink_id] =
      std::make_unique<RootCompositorFrameSinkImpl>(
          this, params->frame_sink_id, std::move(display),
          std::move(begin_frame_source),
          std::move(external_begin_frame_controller),
          std::move(params->compositor_frame_sink),
          mojom::CompositorFrameSinkClientPtr(
              std::move(params->compositor_frame_sink_client)),
          std::move(params->display_private), std::move(display_client));
}

void FrameSinkManagerImpl::CreateCompositorFrameSink(
    const FrameSinkId& frame_sink_id,
    mojom::CompositorFrameSinkRequest request,
    mojom::CompositorFrameSinkClientPtr client) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!base::ContainsKey(sink_map_, frame_sink_id));

  sink_map_[frame_sink_id] = std::make_unique<CompositorFrameSinkImpl>(
      this, frame_sink_id, std::move(request), std::move(client));
}

void FrameSinkManagerImpl::DestroyCompositorFrameSink(
    const FrameSinkId& frame_sink_id,
    DestroyCompositorFrameSinkCallback callback) {
  sink_map_.erase(frame_sink_id);
  std::move(callback).Run();
}

void FrameSinkManagerImpl::RegisterFrameSinkHierarchy(
    const FrameSinkId& parent_frame_sink_id,
    const FrameSinkId& child_frame_sink_id) {
  // If it's possible to reach the parent through the child's descendant chain,
  // then this will create an infinite loop.  Might as well just crash here.
  CHECK(!ChildContains(child_frame_sink_id, parent_frame_sink_id));

  auto& children = frame_sink_source_map_[parent_frame_sink_id].children;
  DCHECK(!base::ContainsKey(children, child_frame_sink_id));
  children.insert(child_frame_sink_id);

  for (auto& observer : observer_list_) {
    observer.OnRegisteredFrameSinkHierarchy(parent_frame_sink_id,
                                            child_frame_sink_id);
  }

  // If the parent has no source, then attaching it to this child will
  // not change any downstream sources.
  BeginFrameSource* parent_source =
      frame_sink_source_map_[parent_frame_sink_id].source;
  if (!parent_source)
    return;

  DCHECK_EQ(registered_sources_.count(parent_source), 1u);
  RecursivelyAttachBeginFrameSource(child_frame_sink_id, parent_source);
}

void FrameSinkManagerImpl::UnregisterFrameSinkHierarchy(
    const FrameSinkId& parent_frame_sink_id,
    const FrameSinkId& child_frame_sink_id) {
  // Deliberately do not check validity of either parent or child FrameSinkId
  // here. They were valid during the registration, so were valid at some point
  // in time. This makes it possible to invalidate parent and child FrameSinkIds
  // independently of each other and not have an ordering dependency of
  // unregistering the hierarchy first before either of them.

  for (auto& observer : observer_list_) {
    observer.OnUnregisteredFrameSinkHierarchy(parent_frame_sink_id,
                                              child_frame_sink_id);
  }

  auto iter = frame_sink_source_map_.find(parent_frame_sink_id);
  DCHECK(iter != frame_sink_source_map_.end());

  // Remove |child_frame_sink_id| from parents list of children.
  auto& mapping = iter->second;
  DCHECK(base::ContainsKey(mapping.children, child_frame_sink_id));
  mapping.children.erase(child_frame_sink_id);

  // Delete the FrameSinkSourceMapping for |parent_frame_sink_id| if empty.
  if (!mapping.has_children() && !mapping.source) {
    frame_sink_source_map_.erase(iter);
    return;
  }

  // If the parent does not have a begin frame source, then disconnecting it
  // will not change any of its children.
  BeginFrameSource* parent_source = iter->second.source;
  if (!parent_source)
    return;

  // TODO(enne): these walks could be done in one step.
  RecursivelyDetachBeginFrameSource(child_frame_sink_id, parent_source);
  for (auto& source_iter : registered_sources_)
    RecursivelyAttachBeginFrameSource(source_iter.second, source_iter.first);
}

void FrameSinkManagerImpl::AssignTemporaryReference(const SurfaceId& surface_id,
                                                    const FrameSinkId& owner) {
  surface_manager_.AssignTemporaryReference(surface_id, owner);
}

void FrameSinkManagerImpl::DropTemporaryReference(const SurfaceId& surface_id) {
  surface_manager_.DropTemporaryReference(surface_id);
}

void FrameSinkManagerImpl::AddVideoDetectorObserver(
    mojom::VideoDetectorObserverPtr observer) {
  if (!video_detector_)
    video_detector_ = std::make_unique<VideoDetector>(&surface_manager_);
  video_detector_->AddObserver(std::move(observer));
}

void FrameSinkManagerImpl::CreateVideoCapturer(
    mojom::FrameSinkVideoCapturerRequest request) {
  video_capturers_.emplace(
      std::make_unique<FrameSinkVideoCapturerImpl>(this, std::move(request)));
}

void FrameSinkManagerImpl::EvictSurfaces(
    const std::vector<SurfaceId>& surface_ids) {
  for (const SurfaceId& surface_id : surface_ids) {
    auto it = support_map_.find(surface_id.frame_sink_id());
    if (it == support_map_.end())
      continue;
    if (it->second->last_activated_surface_id() == surface_id)
      it->second->EvictLastActivatedSurface();
  }
}

void FrameSinkManagerImpl::RequestCopyOfOutput(
    const SurfaceId& surface_id,
    std::unique_ptr<CopyOutputRequest> request) {
  auto it = support_map_.find(surface_id.frame_sink_id());
  if (it == support_map_.end()) {
    // |request| will send an empty result when it goes out of scope.
    return;
  }
  it->second->RequestCopyOfOutput(surface_id.local_surface_id(),
                                  std::move(request));
}

void FrameSinkManagerImpl::OnSurfaceCreated(const SurfaceId& surface_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (client_) {
    client_->OnSurfaceCreated(surface_id);
  } else {
    // There is no client to assign an owner for the temporary reference, so we
    // can drop the temporary reference safely.
    surface_manager_.DropTemporaryReference(surface_id);
  }
}

void FrameSinkManagerImpl::OnFirstSurfaceActivation(
    const SurfaceInfo& surface_info) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK_GT(surface_info.device_scale_factor(), 0.0f);

  if (client_)
    client_->OnFirstSurfaceActivation(surface_info);
}

void FrameSinkManagerImpl::OnSurfaceActivated(
    const SurfaceId& surface_id,
    base::Optional<base::TimeDelta> duration) {
  if (!duration || !client_)
    return;

  // If |duration| is populated then there was a synchronization event prior
  // to this activation.
  auto it = synchronization_event_labels_.find(surface_id.frame_sink_id());
  if (it != synchronization_event_labels_.end()) {
    TRACE_EVENT_INSTANT2(
        "viz", "SurfaceSynchronizationEvent", TRACE_EVENT_SCOPE_THREAD,
        "duration_ms", duration->InMilliseconds(), "client_label", it->second);
    base::UmaHistogramCustomCounts(it->second, duration->InMilliseconds(), 1,
                                   10000, 50);
  }
}

bool FrameSinkManagerImpl::OnSurfaceDamaged(const SurfaceId& surface_id,
                                            const BeginFrameAck& ack) {
  return false;
}

void FrameSinkManagerImpl::OnSurfaceDiscarded(const SurfaceId& surface_id) {}

void FrameSinkManagerImpl::OnSurfaceDestroyed(const SurfaceId& surface_id) {}

void FrameSinkManagerImpl::OnSurfaceDamageExpected(const SurfaceId& surface_id,
                                                   const BeginFrameArgs& args) {
}

void FrameSinkManagerImpl::OnAggregatedHitTestRegionListUpdated(
    const FrameSinkId& frame_sink_id,
    const std::vector<AggregatedHitTestRegion>& hit_test_data) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (client_) {
    client_->OnAggregatedHitTestRegionListUpdated(frame_sink_id, hit_test_data);
  }
}

void FrameSinkManagerImpl::RegisterCompositorFrameSinkSupport(
    const FrameSinkId& frame_sink_id,
    CompositorFrameSinkSupport* support) {
  DCHECK(support);
  DCHECK(!base::ContainsKey(support_map_, frame_sink_id));

  support_map_[frame_sink_id] = support;

  for (auto& capturer : video_capturers_) {
    if (capturer->requested_target() == frame_sink_id)
      capturer->SetResolvedTarget(support);
  }

  auto it = frame_sink_source_map_.find(frame_sink_id);
  if (it != frame_sink_source_map_.end() && it->second.source)
    support->SetBeginFrameSource(it->second.source);

  for (auto& observer : observer_list_)
    observer.OnCreatedCompositorFrameSink(frame_sink_id, support->is_root());
}

void FrameSinkManagerImpl::UnregisterCompositorFrameSinkSupport(
    const FrameSinkId& frame_sink_id) {
  DCHECK(base::ContainsKey(support_map_, frame_sink_id));

  for (auto& observer : observer_list_)
    observer.OnDestroyedCompositorFrameSink(frame_sink_id);

  for (auto& capturer : video_capturers_) {
    if (capturer->requested_target() == frame_sink_id)
      capturer->OnTargetWillGoAway();
  }

  support_map_.erase(frame_sink_id);
}

void FrameSinkManagerImpl::RegisterBeginFrameSource(
    BeginFrameSource* source,
    const FrameSinkId& frame_sink_id) {
  DCHECK(source);
  DCHECK_EQ(registered_sources_.count(source), 0u);

  registered_sources_[source] = frame_sink_id;
  RecursivelyAttachBeginFrameSource(frame_sink_id, source);

  primary_source_.OnBeginFrameSourceAdded(source);
}

void FrameSinkManagerImpl::UnregisterBeginFrameSource(
    BeginFrameSource* source) {
  DCHECK(source);
  DCHECK_EQ(registered_sources_.count(source), 1u);

  FrameSinkId frame_sink_id = registered_sources_[source];
  registered_sources_.erase(source);

  primary_source_.OnBeginFrameSourceRemoved(source);

  if (frame_sink_source_map_.count(frame_sink_id) == 0u)
    return;

  // TODO(enne): these walks could be done in one step.
  // Remove this begin frame source from its subtree.
  RecursivelyDetachBeginFrameSource(frame_sink_id, source);
  // Then flush every remaining registered source to fix any sources that
  // became null because of the previous step but that have an alternative.
  for (auto source_iter : registered_sources_)
    RecursivelyAttachBeginFrameSource(source_iter.second, source_iter.first);
}

BeginFrameSource* FrameSinkManagerImpl::GetPrimaryBeginFrameSource() {
  return &primary_source_;
}

void FrameSinkManagerImpl::RecursivelyAttachBeginFrameSource(
    const FrameSinkId& frame_sink_id,
    BeginFrameSource* source) {
  FrameSinkSourceMapping& mapping = frame_sink_source_map_[frame_sink_id];
  if (!mapping.source) {
    mapping.source = source;
    auto iter = support_map_.find(frame_sink_id);
    if (iter != support_map_.end())
      iter->second->SetBeginFrameSource(source);
  }

  // Copy the list of children because RecursivelyAttachBeginFrameSource() can
  // modify |frame_sink_source_map_| and invalidate iterators.
  base::flat_set<FrameSinkId> children = mapping.children;
  for (const FrameSinkId& child : children)
    RecursivelyAttachBeginFrameSource(child, source);
}

void FrameSinkManagerImpl::RecursivelyDetachBeginFrameSource(
    const FrameSinkId& frame_sink_id,
    BeginFrameSource* source) {
  auto iter = frame_sink_source_map_.find(frame_sink_id);
  if (iter == frame_sink_source_map_.end())
    return;

  auto& mapping = iter->second;
  if (mapping.source == source) {
    mapping.source = nullptr;
    auto client_iter = support_map_.find(frame_sink_id);
    if (client_iter != support_map_.end())
      client_iter->second->SetBeginFrameSource(nullptr);
  }

  // Delete the FrameSinkSourceMapping for |frame_sink_id| if empty.
  if (!mapping.has_children()) {
    frame_sink_source_map_.erase(iter);
    return;
  }

  // Copy the list of children because RecursivelyDetachBeginFrameSource() can
  // modify |frame_sink_source_map_| and invalidate iterators.
  base::flat_set<FrameSinkId> children = mapping.children;
  for (const FrameSinkId& child : children)
    RecursivelyDetachBeginFrameSource(child, source);
}

CapturableFrameSink* FrameSinkManagerImpl::FindCapturableFrameSink(
    const FrameSinkId& frame_sink_id) {
  const auto it = support_map_.find(frame_sink_id);
  if (it == support_map_.end())
    return nullptr;
  return it->second;
}

void FrameSinkManagerImpl::OnCapturerConnectionLost(
    FrameSinkVideoCapturerImpl* capturer) {
  video_capturers_.erase(capturer);
}

bool FrameSinkManagerImpl::ChildContains(
    const FrameSinkId& child_frame_sink_id,
    const FrameSinkId& search_frame_sink_id) const {
  auto iter = frame_sink_source_map_.find(child_frame_sink_id);
  if (iter == frame_sink_source_map_.end())
    return false;

  for (const FrameSinkId& child : iter->second.children) {
    if (child == search_frame_sink_id)
      return true;
    if (ChildContains(child, search_frame_sink_id))
      return true;
  }
  return false;
}

void FrameSinkManagerImpl::SubmitHitTestRegionList(
    const SurfaceId& surface_id,
    uint64_t frame_index,
    mojom::HitTestRegionListPtr hit_test_region_list) {
  hit_test_manager_.SubmitHitTestRegionList(surface_id, frame_index,
                                            std::move(hit_test_region_list));
}

void FrameSinkManagerImpl::OnFrameTokenChanged(const FrameSinkId& frame_sink_id,
                                               uint32_t frame_token) {
  if (client_)
    client_->OnFrameTokenChanged(frame_sink_id, frame_token);
}

VideoDetector* FrameSinkManagerImpl::CreateVideoDetectorForTesting(
    const base::TickClock* tick_clock,
    scoped_refptr<base::SequencedTaskRunner> task_runner) {
  DCHECK(!video_detector_);
  video_detector_ = std::make_unique<VideoDetector>(surface_manager(),
                                                    tick_clock, task_runner);
  return video_detector_.get();
}

void FrameSinkManagerImpl::AddObserver(FrameSinkObserver* obs) {
  observer_list_.AddObserver(obs);
}

void FrameSinkManagerImpl::RemoveObserver(FrameSinkObserver* obs) {
  observer_list_.RemoveObserver(obs);
}

std::vector<FrameSinkId> FrameSinkManagerImpl::GetCreatedFrameSinkIds() const {
  std::vector<FrameSinkId> frame_sink_ids;
  for (auto& map_entry : support_map_)
    frame_sink_ids.push_back(map_entry.first);
  return frame_sink_ids;
}

std::vector<FrameSinkId> FrameSinkManagerImpl::GetRegisteredFrameSinkIds()
    const {
  std::vector<FrameSinkId> frame_sink_ids;
  for (auto& map_entry : surface_manager_.valid_frame_sink_labels())
    frame_sink_ids.push_back(map_entry.first);
  return frame_sink_ids;
}

base::flat_set<FrameSinkId> FrameSinkManagerImpl::GetChildrenByParent(
    const FrameSinkId& parent_frame_sink_id) const {
  auto it = frame_sink_source_map_.find(parent_frame_sink_id);
  if (it != frame_sink_source_map_.end())
    return it->second.children;
  return {};
}

const CompositorFrameSinkSupport* FrameSinkManagerImpl::GetFrameSinkForId(
    const FrameSinkId& frame_sink_id) const {
  auto it = support_map_.find(frame_sink_id);
  if (it != support_map_.end())
    return it->second;
  return nullptr;
}

}  // namespace viz
