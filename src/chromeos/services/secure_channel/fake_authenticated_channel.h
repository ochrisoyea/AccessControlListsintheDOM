// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_SECURE_CHANNEL_FAKE_AUTHENTICATED_CHANNEL_H_
#define CHROMEOS_SERVICES_SECURE_CHANNEL_FAKE_AUTHENTICATED_CHANNEL_H_

#include <string>
#include <tuple>
#include <vector>

#include "base/callback.h"
#include "base/logging.h"
#include "base/macros.h"
#include "chromeos/services/secure_channel/authenticated_channel.h"

namespace chromeos {

namespace secure_channel {

// Test AuthenticatedChannel implementation.
class FakeAuthenticatedChannel : public AuthenticatedChannel {
 public:
  FakeAuthenticatedChannel();
  ~FakeAuthenticatedChannel() override;

  const std::vector<std::tuple<std::string, std::string, base::OnceClosure>>&
  sent_messages() {
    return sent_messages_;
  }

  // AuthenticatedChannel:
  void PerformSendMessage(const std::string& feature,
                          const std::string& payload,
                          base::OnceClosure on_sent_callback) override;

  // Make Notify{Disconnected|MessageReceived}() public for testing.
  using AuthenticatedChannel::NotifyDisconnected;
  using AuthenticatedChannel::NotifyMessageReceived;

 private:
  std::vector<std::tuple<std::string, std::string, base::OnceClosure>>
      sent_messages_;

  DISALLOW_COPY_AND_ASSIGN(FakeAuthenticatedChannel);
};

// Test AuthenticatedChannel::Observer implementation.
class FakeAuthenticatedChannelObserver : public AuthenticatedChannel::Observer {
 public:
  FakeAuthenticatedChannelObserver(const std::string& expected_channel_id);
  ~FakeAuthenticatedChannelObserver() override;

  bool has_been_notified_of_disconnection() {
    return has_been_notified_of_disconnection_;
  }

  const std::vector<std::pair<std::string, std::string>>& received_messages() {
    return received_messages_;
  }

  // AuthenticatedChannel::Observer:
  void OnDisconnected(const std::string& channel_id) override;
  void OnMessageReceived(const std::string& channel_id,
                         const std::string& feature,
                         const std::string& payload) override;

 private:
  const std::string expected_channel_id_;
  bool has_been_notified_of_disconnection_ = false;
  std::vector<std::pair<std::string, std::string>> received_messages_;
};

}  // namespace secure_channel

}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_SECURE_CHANNEL_FAKE_AUTHENTICATED_CHANNEL_H_
