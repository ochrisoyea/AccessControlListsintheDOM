// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/audio/service_factory.h"

#include <string>

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "media/audio/audio_manager.h"
#include "media/base/media_switches.h"
#include "services/audio/in_process_audio_manager_accessor.h"
#include "services/audio/owning_audio_manager_accessor.h"
#include "services/audio/service.h"
#include "services/service_manager/public/cpp/service.h"

namespace audio {

namespace {

base::TimeDelta GetQuitTimeout() {
  constexpr base::TimeDelta kDefaultTimeout = base::TimeDelta::FromSeconds(10);

  const base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (!cmd_line->HasSwitch(switches::kAudioServiceQuitTimeoutMs))
    return kDefaultTimeout;

  std::string timeout_str(
      cmd_line->GetSwitchValueASCII(switches::kAudioServiceQuitTimeoutMs));
  int timeout_ms = 0;
  if (!base::StringToInt(timeout_str, &timeout_ms) || timeout_ms < 0)
    return kDefaultTimeout;  // Ill-formed value provided, fall back to default.

  return base::TimeDelta::FromMilliseconds(timeout_ms);
}

}  // namespace

std::unique_ptr<service_manager::Service> CreateEmbeddedService(
    media::AudioManager* audio_manager) {
  return std::make_unique<Service>(
      std::make_unique<InProcessAudioManagerAccessor>(audio_manager),
      base::TimeDelta() /* do not quit if all clients disconnected */,
      false /* enable_device_notifications */);
}

std::unique_ptr<service_manager::Service> CreateStandaloneService() {
  return std::make_unique<Service>(
      std::make_unique<audio::OwningAudioManagerAccessor>(
          base::BindOnce(&media::AudioManager::Create)),
      GetQuitTimeout(), true /* enable_device_notifications */);
}

}  // namespace audio
