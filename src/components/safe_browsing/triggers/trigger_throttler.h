// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SAFE_BROWSING_TRIGGERS_TRIGGER_THROTTLER_H_
#define COMPONENTS_SAFE_BROWSING_TRIGGERS_TRIGGER_THROTTLER_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/time/clock.h"

namespace safe_browsing {
// Default quota for ad sampler trigger.
extern const size_t kAdSamplerTriggerDefaultQuota;

// Param name of the finch param containing the quota for the suspicious site
// trigger.
extern const char kSuspiciousSiteTriggerQuotaParam[];

// Param name of the finch param containing the comma-separated list of trigger
// types and daily quotas.
// TODO(crbug.com/744869): This param should be deprecated after ad sampler
// launch in favour of having a unique quota feature and param per trigger.
// Having a single shared feature makes it impossible to run multiple trigger
// trials simultaneously.
extern const char kTriggerTypeAndQuotaParam[];

enum class TriggerType {
  SECURITY_INTERSTITIAL = 1,
  AD_SAMPLE = 2,
  GAIA_PASSWORD_REUSE = 3,
  SUSPICIOUS_SITE = 4,
};

struct TriggerTypeHash {
  std::size_t operator()(TriggerType trigger_type) const {
    return static_cast<std::size_t>(trigger_type);
  };
};

// A map for storing a list of event timestamps for different trigger types.
using TriggerTimestampMap =
    std::unordered_map<TriggerType, std::vector<time_t>, TriggerTypeHash>;

// A pair containing a TriggerType and its associated daily report quota.
using TriggerTypeAndQuotaItem = std::pair<TriggerType, int>;

// TriggerThrottler keeps track of how often each type of trigger gets fired
// and throttles them if they fire too often.
class TriggerThrottler {
 public:
  TriggerThrottler();
  virtual ~TriggerThrottler();

  // Check if the the specified |trigger_type| has quota available and is
  // allowed to fire at this time.
  virtual bool TriggerCanFire(TriggerType trigger_type) const;

  // Called to notify the throttler that a trigger has just fired and quota
  // should be updated.
  void TriggerFired(TriggerType trigger_type);

 protected:
  void SetClockForTesting(base::Clock* test_clock);

 private:
  friend class TriggerThrottlerTest;
  friend class TriggerThrottlerTestFinch;

  // Called to periodically clean-up the list of event timestamps.
  void CleanupOldEvents();

  // Returns the daily quota for the specified trigger.
  size_t GetDailyQuotaForTrigger(const TriggerType trigger_type) const;

  // Can be set for testing.
  base::Clock* clock_;

  // Stores each trigger type that fired along with the timestamps of when it
  // fired.
  TriggerTimestampMap trigger_events_;

  // List of trigger types and their quotas, controlled by Finch feature
  // |kTriggerThrottlerDailyQuotaFeature|.
  std::vector<TriggerTypeAndQuotaItem> trigger_type_and_quota_list_;

  DISALLOW_COPY_AND_ASSIGN(TriggerThrottler);
};

}  // namespace safe_browsing

#endif  // COMPONENTS_SAFE_BROWSING_TRIGGERS_TRIGGER_THROTTLER_H_
