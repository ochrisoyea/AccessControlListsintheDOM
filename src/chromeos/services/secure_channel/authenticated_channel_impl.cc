// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/secure_channel/authenticated_channel_impl.h"

#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "chromeos/components/proximity_auth/logging/logging.h"

namespace chromeos {

namespace secure_channel {

// static
AuthenticatedChannelImpl::Factory*
    AuthenticatedChannelImpl::Factory::test_factory_ = nullptr;

// static
AuthenticatedChannelImpl::Factory* AuthenticatedChannelImpl::Factory::Get() {
  if (test_factory_)
    return test_factory_;

  static base::NoDestructor<AuthenticatedChannelImpl::Factory> factory;
  return factory.get();
}

// static
void AuthenticatedChannelImpl::Factory::SetFactoryForTesting(
    Factory* test_factory) {
  test_factory_ = test_factory;
}

std::unique_ptr<AuthenticatedChannel>
AuthenticatedChannelImpl::Factory::BuildInstance(
    std::unique_ptr<cryptauth::SecureChannel> secure_channel) {
  return base::WrapUnique(
      new AuthenticatedChannelImpl(std::move(secure_channel)));
}

AuthenticatedChannelImpl::AuthenticatedChannelImpl(
    std::unique_ptr<cryptauth::SecureChannel> secure_channel)
    : AuthenticatedChannel(), secure_channel_(std::move(secure_channel)) {
  // |secure_channel_| should be a valid and already authenticated.
  DCHECK(secure_channel_);
  DCHECK_EQ(secure_channel_->status(),
            cryptauth::SecureChannel::Status::AUTHENTICATED);

  secure_channel_->AddObserver(this);
}

AuthenticatedChannelImpl::~AuthenticatedChannelImpl() {
  secure_channel_->RemoveObserver(this);
}

void AuthenticatedChannelImpl::PerformSendMessage(
    const std::string& feature,
    const std::string& payload,
    base::OnceClosure on_sent_callback) {
  DCHECK_EQ(secure_channel_->status(),
            cryptauth::SecureChannel::Status::AUTHENTICATED);

  int sequence_number = secure_channel_->SendMessage(feature, payload);

  if (base::ContainsKey(sequence_number_to_callback_map_, sequence_number)) {
    PA_LOG(ERROR) << "AuthenticatedChannelImpl::SendMessage(): Started sending "
                  << "a message whose sequence number already exists in the "
                  << "map.";
    NOTREACHED();
  }

  sequence_number_to_callback_map_[sequence_number] =
      std::move(on_sent_callback);
}

void AuthenticatedChannelImpl::OnSecureChannelStatusChanged(
    cryptauth::SecureChannel* secure_channel,
    const cryptauth::SecureChannel::Status& old_status,
    const cryptauth::SecureChannel::Status& new_status) {
  DCHECK_EQ(secure_channel_.get(), secure_channel);

  // The only expected status change is AUTHENTICATED => DISCONNECTED.
  DCHECK_EQ(old_status, cryptauth::SecureChannel::Status::AUTHENTICATED);
  DCHECK_EQ(new_status, cryptauth::SecureChannel::Status::DISCONNECTED);

  NotifyDisconnected();
}

void AuthenticatedChannelImpl::OnMessageReceived(
    cryptauth::SecureChannel* secure_channel,
    const std::string& feature,
    const std::string& payload) {
  DCHECK_EQ(secure_channel_.get(), secure_channel);
  NotifyMessageReceived(feature, payload);
}

void AuthenticatedChannelImpl::OnMessageSent(
    cryptauth::SecureChannel* secure_channel,
    int sequence_number) {
  DCHECK_EQ(secure_channel_.get(), secure_channel);

  if (!base::ContainsKey(sequence_number_to_callback_map_, sequence_number)) {
    PA_LOG(WARNING) << "AuthenticatedChannelImpl::OnMessageSent(): Sent a "
                    << "message whose sequence number did not exist in the "
                    << "map. Disregarding.";
    // Note: No DCHECK() is performed here, since |secure_channel_| could have
    // already been in the process of sending a message before the
    // AuthenticatedChannelImpl object was created.
    return;
  }

  std::move(sequence_number_to_callback_map_[sequence_number]).Run();
  sequence_number_to_callback_map_.erase(sequence_number);
}

}  // namespace secure_channel

}  // namespace chromeos
