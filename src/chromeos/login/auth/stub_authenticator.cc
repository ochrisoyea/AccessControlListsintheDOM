// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/login/auth/stub_authenticator.h"

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/threading/thread_task_runner_handle.h"

namespace chromeos {

namespace {

// As defined in /chromeos/dbus/cryptohome_client.cc.
static const char kUserIdHashSuffix[] = "-hash";

}  // anonymous namespace

StubAuthenticator::StubAuthenticator(AuthStatusConsumer* consumer,
                                     const UserContext& expected_user_context)
    : Authenticator(consumer),
      expected_user_context_(expected_user_context),
      task_runner_(base::ThreadTaskRunnerHandle::Get()) {
}

void StubAuthenticator::CompleteLogin(content::BrowserContext* context,
                                      const UserContext& user_context) {
  authentication_context_ = context;
  if (expected_user_context_ != user_context)
    NOTREACHED();
  OnAuthSuccess();
}

void StubAuthenticator::AuthenticateToLogin(content::BrowserContext* context,
                                            const UserContext& user_context) {
  authentication_context_ = context;
  // Don't compare the entire |expected_user_context_| to |user_context| because
  // during non-online re-auth |user_context| does not have a gaia id.
  if (expected_user_context_.GetAccountId() == user_context.GetAccountId() &&
      *expected_user_context_.GetKey() == *user_context.GetKey()) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&StubAuthenticator::OnAuthSuccess, this));
    return;
  }
  GoogleServiceAuthError error =
      GoogleServiceAuthError::FromInvalidGaiaCredentialsReason(
          GoogleServiceAuthError::InvalidGaiaCredentialsReason::
              CREDENTIALS_REJECTED_BY_SERVER);
  task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&StubAuthenticator::OnAuthFailure, this,
                                AuthFailure::FromNetworkAuthFailure(error)));
}

void StubAuthenticator::AuthenticateToUnlock(const UserContext& user_context) {
  AuthenticateToLogin(NULL /* not used */, user_context);
}

void StubAuthenticator::LoginAsSupervisedUser(const UserContext& user_context) {
  UserContext new_user_context = user_context;
  new_user_context.SetUserIDHash(user_context.GetAccountId().GetUserEmail() +
                                 kUserIdHashSuffix);
  consumer_->OnAuthSuccess(new_user_context);
}

void StubAuthenticator::LoginOffTheRecord() {
  consumer_->OnOffTheRecordAuthSuccess();
}

void StubAuthenticator::LoginAsPublicSession(const UserContext& user_context) {
  UserContext logged_in_user_context = user_context;
  logged_in_user_context.SetIsUsingOAuth(false);
  logged_in_user_context.SetUserIDHash(
      logged_in_user_context.GetAccountId().GetUserEmail() + kUserIdHashSuffix);
  consumer_->OnAuthSuccess(logged_in_user_context);
}

void StubAuthenticator::LoginAsKioskAccount(
    const AccountId& /* app_account_id */,
    bool use_guest_mount) {
  UserContext user_context(user_manager::UserType::USER_TYPE_KIOSK_APP,
                           expected_user_context_.GetAccountId());
  user_context.SetIsUsingOAuth(false);
  user_context.SetUserIDHash(
      expected_user_context_.GetAccountId().GetUserEmail() + kUserIdHashSuffix);
  consumer_->OnAuthSuccess(user_context);
}

void StubAuthenticator::LoginAsArcKioskAccount(
    const AccountId& /* app_account_id */) {
  UserContext user_context(user_manager::USER_TYPE_ARC_KIOSK_APP,
                           expected_user_context_.GetAccountId());
  user_context.SetIsUsingOAuth(false);
  user_context.SetUserIDHash(
      expected_user_context_.GetAccountId().GetUserEmail() + kUserIdHashSuffix);
  consumer_->OnAuthSuccess(user_context);
}

void StubAuthenticator::OnAuthSuccess() {
  // If we want to be more like the real thing, we could save the user ID
  // in AuthenticateToLogin, but there's not much of a point.
  UserContext user_context(expected_user_context_);
  user_context.SetUserIDHash(
      expected_user_context_.GetAccountId().GetUserEmail() + kUserIdHashSuffix);
  consumer_->OnAuthSuccess(user_context);
}

void StubAuthenticator::OnAuthFailure(const AuthFailure& failure) {
  consumer_->OnAuthFailure(failure);
}

void StubAuthenticator::RecoverEncryptedData(const std::string& old_password) {
}

void StubAuthenticator::ResyncEncryptedData() {
}

void StubAuthenticator::SetExpectedCredentials(
    const UserContext& user_context) {
  expected_user_context_ = user_context;
}

StubAuthenticator::~StubAuthenticator() = default;

}  // namespace chromeos
