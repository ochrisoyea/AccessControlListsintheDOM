// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/config/gpu_info_collector.h"

#include "base/trace_event/trace_event.h"

namespace gpu {

bool CollectContextGraphicsInfo(GPUInfo* gpu_info) {
  DCHECK(gpu_info);

  TRACE_EVENT0("gpu", "gpu_info_collector::CollectGraphicsInfo");

  return CollectGraphicsInfoGL(gpu_info);
}

bool CollectBasicGraphicsInfo(GPUInfo* gpu_info) {
  // TODO(crbug.com/707031): Implement this.
  NOTIMPLEMENTED();
  return false;
}

void CollectDriverInfoGL(GPUInfo* gpu_info) {
  // TODO(crbug.com/707031): Implement this.
  NOTIMPLEMENTED();
}

}  // namespace gpu
