// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/secure_channel/fake_pending_connection_request_delegate.h"

#include "base/stl_util.h"

namespace chromeos {

namespace secure_channel {

FakePendingConnectionRequestDelegate::FakePendingConnectionRequestDelegate() =
    default;

FakePendingConnectionRequestDelegate::~FakePendingConnectionRequestDelegate() =
    default;

const base::Optional<PendingConnectionRequestDelegate::FailedConnectionReason>&
FakePendingConnectionRequestDelegate::GetFailedConnectionReasonForId(
    const std::string& request_id) {
  return request_id_to_failed_connection_reason_map_[request_id];
}

void FakePendingConnectionRequestDelegate::OnRequestFinishedWithoutConnection(
    const std::string& request_id,
    FailedConnectionReason reason) {
  request_id_to_failed_connection_reason_map_[request_id] = reason;

  if (closure_for_next_delegate_callback_)
    std::move(closure_for_next_delegate_callback_).Run();
}

}  // namespace secure_channel

}  // namespace chromeos
