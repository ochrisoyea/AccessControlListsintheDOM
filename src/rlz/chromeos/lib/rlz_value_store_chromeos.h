// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RLZ_CHROMEOS_LIB_RLZ_VALUE_STORE_CHROMEOS_H_
#define RLZ_CHROMEOS_LIB_RLZ_VALUE_STORE_CHROMEOS_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "base/files/file_path.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "rlz/lib/rlz_value_store.h"

namespace base {
class DictionaryValue;
class Value;
}

namespace rlz_lib {

// An implementation of RlzValueStore for ChromeOS.
class RlzValueStoreChromeOS : public RlzValueStore {
 public:
  // Ignore |kRlzEmbargoEndDateKey| if it's more than this many days in the
  // future.
  static const int kRlzEmbargoEndDateGarbageDateThresholdDays;

  // The maximum retry times allowed for |SetRlzPingSent|.
  static const int kMaxRetryCount;

  // Creates new instance and synchronously reads data from file.
  explicit RlzValueStoreChromeOS(const base::FilePath& store_path);
  ~RlzValueStoreChromeOS() override;

  // RlzValueStore overrides:
  bool HasAccess(AccessType type) override;

  bool WritePingTime(Product product, int64_t time) override;
  bool ReadPingTime(Product product, int64_t* time) override;
  bool ClearPingTime(Product product) override;

  bool WriteAccessPointRlz(AccessPoint access_point,
                           const char* new_rlz) override;
  bool ReadAccessPointRlz(AccessPoint access_point,
                          char* rlz,
                          size_t rlz_size) override;
  bool ClearAccessPointRlz(AccessPoint access_point) override;

  bool AddProductEvent(Product product, const char* event_rlz) override;
  bool ReadProductEvents(Product product,
                         std::vector<std::string>* events) override;
  bool ClearProductEvent(Product product, const char* event_rlz) override;
  bool ClearAllProductEvents(Product product) override;

  bool AddStatefulEvent(Product product, const char* event_rlz) override;
  bool IsStatefulEvent(Product product, const char* event_rlz) override;
  bool ClearAllStatefulEvents(Product product) override;

  void CollectGarbage() override;

  enum class EmbargoState {
    kMissingOrMalformed,
    kInvalid,
    kNotPassed,
    kPassed
  };

  static EmbargoState GetRlzEmbargoState();

 private:
  // Returns true if the |rlz_embargo_end_date| present in VPD has passed
  // compared to the current time.
  static bool HasRlzEmbargoEndDatePassed();

  // Reads RLZ store from file.
  void ReadStore();

  // Writes RLZ store back to file.
  void WriteStore();

  // Adds |value| to list at |list_name| path in JSON store.
  bool AddValueToList(const std::string& list_name,
                      std::unique_ptr<base::Value> value);
  // Removes |value| from list at |list_name| path in JSON store.
  bool RemoveValueFromList(const std::string& list_name,
                           const base::Value& value);

  // Set |should_send_rlz_ping| to 0 in RW_VPD. This is a wrapper of
  // |DebugDaemonClient::SetRlzPingSent|.
  void SetRlzPingSent();

  // Callback of |SetRlzPingSent|.
  void OnSetRlzPingSent(bool success);

  // In-memory store with RLZ data.
  std::unique_ptr<base::DictionaryValue> rlz_store_;

  base::FilePath store_path_;

  bool read_only_;

  // The number of attempts of |SetRlzPingSent| so far.
  int set_rlz_ping_sent_attempts_;

  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtrFactory<RlzValueStoreChromeOS> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(RlzValueStoreChromeOS);
};

}  // namespace rlz_lib

#endif  // RLZ_CHROMEOS_LIB_RLZ_VALUE_STORE_CHROMEOS_H_
