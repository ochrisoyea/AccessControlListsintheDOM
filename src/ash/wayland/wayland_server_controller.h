// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_WAYLAND_WAYLAND_SERVER_CONTROLLER_H_
#define ASH_WAYLAND_WAYLAND_SERVER_CONTROLLER_H_

#include <memory>

#include "base/macros.h"

namespace arc {
class ArcNotificationSurfaceManagerImpl;
}  // namespace arc

namespace exo {
class Display;
class FileHelper;
class WMHelper;
namespace wayland {
class Server;
}
}  // namespace exo

namespace ash {

class WaylandServerController {
 public:
  // Creates WaylandServerController. Returns null if controller should not be
  // created.
  static std::unique_ptr<WaylandServerController> CreateIfNecessary(
      std::unique_ptr<exo::FileHelper> file_helper);

  ~WaylandServerController();

 private:
  explicit WaylandServerController(
      std::unique_ptr<exo::FileHelper> file_helper);

  std::unique_ptr<exo::WMHelper> wm_helper_;
  std::unique_ptr<exo::Display> display_;
  std::unique_ptr<exo::wayland::Server> wayland_server_;
  class WaylandWatcher;
  std::unique_ptr<WaylandWatcher> wayland_watcher_;

  std::unique_ptr<arc::ArcNotificationSurfaceManagerImpl>
      arc_notification_surface_manager_;

  DISALLOW_COPY_AND_ASSIGN(WaylandServerController);
};

}  // namespace ash

#endif  // ASH_WAYLAND_WAYLAND_SERVER_CONTROLLER_H_
