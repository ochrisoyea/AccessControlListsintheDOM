// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_SYSTEM_NOTIFICATION_CONTROLLER_H_
#define ASH_SYSTEM_SYSTEM_NOTIFICATION_CONTROLLER_H_

#include <memory>

#include "base/macros.h"

namespace ash {

class CapsLockNotificationController;
class PowerNotificationController;
class ScreenSecurityNotificationController;
class SessionLimitNotificationController;
class SupervisedNotificationController;
class WifiToggleNotificationController;

// Class that owns individual notification controllers.
class SystemNotificationController {
 public:
  SystemNotificationController();
  ~SystemNotificationController();

 private:
  const std::unique_ptr<CapsLockNotificationController> caps_lock_;
  const std::unique_ptr<PowerNotificationController> power_;
  const std::unique_ptr<ScreenSecurityNotificationController> screen_security_;
  const std::unique_ptr<SessionLimitNotificationController> session_limit_;
  const std::unique_ptr<SupervisedNotificationController> supervised_;
  const std::unique_ptr<WifiToggleNotificationController> wifi_toggle_;

  DISALLOW_COPY_AND_ASSIGN(SystemNotificationController);
};

}  // namespace ash

#endif  // ASH_SYSTEM_SYSTEM_NOTIFICATION_CONTROLLER_H_
