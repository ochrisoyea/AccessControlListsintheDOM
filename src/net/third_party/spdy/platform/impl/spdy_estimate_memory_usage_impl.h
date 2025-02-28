// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_ESTIMATE_MEMORY_USAGE_IMPL_H_
#define NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_ESTIMATE_MEMORY_USAGE_IMPL_H_

#include <cstddef>

#include "base/trace_event/memory_usage_estimator.h"

namespace net {

template <class T>
size_t SpdyEstimateMemoryUsageImpl(const T& object) {
  return base::trace_event::EstimateMemoryUsage(object);
}

}  // namespace net

#endif  // NET_THIRD_PARTY_SPDY_PLATFORM_IMPL_SPDY_ESTIMATE_MEMORY_USAGE_IMPL_H_
