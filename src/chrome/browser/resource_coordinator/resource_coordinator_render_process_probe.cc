// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/resource_coordinator/resource_coordinator_render_process_probe.h"

#include <vector>

#include "base/bind.h"
#include "build/build_config.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/service_manager_connection.h"
#include "services/resource_coordinator/public/cpp/process_resource_coordinator.h"
#include "services/resource_coordinator/public/cpp/resource_coordinator_features.h"
#include "services/resource_coordinator/public/mojom/coordination_unit.mojom.h"

#if defined(OS_MACOSX)
#include "content/public/browser/browser_child_process_host.h"
#endif

namespace resource_coordinator {

namespace {

constexpr base::TimeDelta kDefaultMeasurementInterval =
    base::TimeDelta::FromMinutes(10);

}  // namespace

ResourceCoordinatorRenderProcessProbe::RenderProcessInfo::RenderProcessInfo() =
    default;

ResourceCoordinatorRenderProcessProbe::RenderProcessInfo::~RenderProcessInfo() =
    default;

ResourceCoordinatorRenderProcessProbe::ResourceCoordinatorRenderProcessProbe()
    : interval_(kDefaultMeasurementInterval) {
  UpdateWithFieldTrialParams();
}

ResourceCoordinatorRenderProcessProbe::
    ~ResourceCoordinatorRenderProcessProbe() = default;

// static
ResourceCoordinatorRenderProcessProbe*
ResourceCoordinatorRenderProcessProbe::GetInstance() {
  static base::NoDestructor<ResourceCoordinatorRenderProcessProbe> probe;
  return probe.get();
}

// static
bool ResourceCoordinatorRenderProcessProbe::IsEnabled() {
  // Check that service_manager is active, GRC is enabled,
  // and render process CPU profiling is enabled.
  return content::ServiceManagerConnection::GetForProcess() != nullptr &&
         resource_coordinator::IsResourceCoordinatorEnabled() &&
         base::FeatureList::IsEnabled(features::kGRCRenderProcessCPUProfiling);
}

void ResourceCoordinatorRenderProcessProbe::StartGatherCycle() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // TODO(siggi): It irks me to have this bit of policy embedded here.
  //     I feel this should be moved to the caller...
  if (!ResourceCoordinatorRenderProcessProbe::IsEnabled()) {
    return;
  }

  DCHECK(!is_gather_cycle_started_);

  is_gather_cycle_started_ = true;
  if (!is_gathering_) {
    timer_.Start(FROM_HERE, base::TimeDelta(), this,
                 &ResourceCoordinatorRenderProcessProbe::
                     RegisterAliveRenderProcessesOnUIThread);
  }
}

void ResourceCoordinatorRenderProcessProbe::StartSingleGather() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (is_gathering_)
    return;

  // If the gather cycle is started this measurement will go through early,
  // and the interval between measurements will be shortened.
  timer_.Start(FROM_HERE, base::TimeDelta(), this,
               &ResourceCoordinatorRenderProcessProbe::
                   RegisterAliveRenderProcessesOnUIThread);
}

void ResourceCoordinatorRenderProcessProbe::
    RegisterAliveRenderProcessesOnUIThread() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!is_gathering_);

  ++current_gather_cycle_;

  for (content::RenderProcessHost::iterator rph_iter =
           content::RenderProcessHost::AllHostsIterator();
       !rph_iter.IsAtEnd(); rph_iter.Advance()) {
    content::RenderProcessHost* host = rph_iter.GetCurrentValue();
    // Process may not be valid yet.
    if (!host->GetProcess().IsValid()) {
      continue;
    }

    auto& render_process_info = render_process_info_map_[host->GetID()];
    render_process_info.last_gather_cycle_active = current_gather_cycle_;
    if (render_process_info.metrics.get() == nullptr) {
      DCHECK(!render_process_info.process.IsValid());

      // Duplicate the process to retain ownership of it through the thread
      // bouncing.
      render_process_info.process = host->GetProcess().Duplicate();

#if defined(OS_MACOSX)
      render_process_info.metrics = base::ProcessMetrics::CreateProcessMetrics(
          render_process_info.process.Handle(),
          content::BrowserChildProcessHost::GetPortProvider());
#else
      render_process_info.metrics = base::ProcessMetrics::CreateProcessMetrics(
          render_process_info.process.Handle());
#endif
    }
  }

  is_gathering_ = true;

  content::BrowserThread::PostTask(
      content::BrowserThread::IO, FROM_HERE,
      base::BindOnce(
          &ResourceCoordinatorRenderProcessProbe::
              CollectRenderProcessMetricsAndStartMemoryDumpOnIOThread,
          base::Unretained(this)));
}

void ResourceCoordinatorRenderProcessProbe::
    CollectRenderProcessMetricsAndStartMemoryDumpOnIOThread() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  DCHECK(is_gathering_);

  // Dispatch the memory collection request.
  memory_instrumentation::MemoryInstrumentation::GetInstance()
      ->RequestGlobalDump(
          base::BindRepeating(&ResourceCoordinatorRenderProcessProbe::
                                  ProcessGlobalMemoryDumpAndDispatchOnIOThread,
                              base::Unretained(this)));

  RenderProcessInfoMap::iterator iter = render_process_info_map_.begin();
  while (iter != render_process_info_map_.end()) {
    auto& render_process_info = iter->second;

    // If the last gather cycle the render process was marked as active is
    // not current then it is assumed dead and should not be measured anymore.
    if (render_process_info.last_gather_cycle_active == current_gather_cycle_) {
      render_process_info.cpu_usage =
          render_process_info.metrics->GetPlatformIndependentCPUUsage();
      ++iter;
    } else {
      render_process_info_map_.erase(iter++);
      continue;
    }
  }
}

void ResourceCoordinatorRenderProcessProbe::
    ProcessGlobalMemoryDumpAndDispatchOnIOThread(
        bool global_success,
        std::unique_ptr<memory_instrumentation::GlobalMemoryDump> dump) {
  // Create the measurement batch.
  mojom::ProcessResourceMeasurementBatchPtr batch =
      mojom::ProcessResourceMeasurementBatch::New();

  // TODO(siggi): Add start/end times. This will need some support from
  //     the memory_instrumentation code.

  // Start by adding the render process hosts we know about to the batch.
  for (const auto& render_process_info_map_entry : render_process_info_map_) {
    auto& render_process_info = render_process_info_map_entry.second;
    // TODO(oysteine): Move the multiplier used to avoid precision loss
    // into a shared location, when this property gets used.
    mojom::ProcessResourceMeasurementPtr measurement =
        mojom::ProcessResourceMeasurement::New();

    measurement->pid = render_process_info.process.Pid();
    measurement->cpu_usage = render_process_info.cpu_usage;

    batch->measurements.push_back(std::move(measurement));
  }

  if (dump) {
    // Then amend the ones we have memory metrics for with their private
    // footprint. The global dump may contain non-renderer processes, it may
    // contain renderer processes we didn't capture at the start of the cycle,
    // and it may not contain all the renderer processes we know about.
    // This may happen due to the inherent race between the request and
    // starting/stopping renderers, or because of other failures
    // This may therefore provide incomplete information.
    for (const auto& dump_entry : dump->process_dumps()) {
      base::ProcessId pid = dump_entry.pid();

      for (const auto& measurement : batch->measurements) {
        if (measurement->pid == pid) {
          measurement->private_footprint_kb =
              dump_entry.os_dump().private_footprint_kb;
          break;
        }
      }
    }
  } else {
    // We should only get a nullptr in case of failure.
    DCHECK(!global_success);
  }

  bool should_restart = DispatchMetrics(std::move(batch));
  content::BrowserThread::PostTask(
      content::BrowserThread::UI, FROM_HERE,
      base::BindOnce(
          &ResourceCoordinatorRenderProcessProbe::FinishCollectionOnUIThread,
          base::Unretained(this), should_restart));
}

void ResourceCoordinatorRenderProcessProbe::FinishCollectionOnUIThread(
    bool restart_cycle) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(is_gathering_);
  is_gathering_ = false;

  if (restart_cycle && is_gather_cycle_started_) {
    timer_.Start(FROM_HERE, interval_, this,
                 &ResourceCoordinatorRenderProcessProbe::
                     RegisterAliveRenderProcessesOnUIThread);
  } else {
    is_gather_cycle_started_ = false;
  }
}

void ResourceCoordinatorRenderProcessProbe::UpdateWithFieldTrialParams() {
  int64_t interval_ms = GetGRCRenderProcessCPUProfilingIntervalInMs();

  if (interval_ms > 0) {
    interval_ = base::TimeDelta::FromMilliseconds(interval_ms);
  }
}

SystemResourceCoordinator*
ResourceCoordinatorRenderProcessProbe::EnsureSystemResourceCoordinator() {
  if (!system_resource_coordinator_) {
    content::ServiceManagerConnection* connection =
        content::ServiceManagerConnection::GetForProcess();
    if (connection)
      system_resource_coordinator_ =
          std::make_unique<SystemResourceCoordinator>(
              connection->GetConnector());
  }

  return system_resource_coordinator_.get();
}

bool ResourceCoordinatorRenderProcessProbe::DispatchMetrics(
    mojom::ProcessResourceMeasurementBatchPtr batch) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  SystemResourceCoordinator* system_resource_coordinator =
      EnsureSystemResourceCoordinator();

  if (system_resource_coordinator && !batch->measurements.empty())
    system_resource_coordinator->DistributeMeasurementBatch(std::move(batch));

  return true;
}

}  // namespace resource_coordinator
