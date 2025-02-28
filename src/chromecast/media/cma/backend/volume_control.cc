// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/public/volume_control.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "base/values.h"
#include "chromecast/base/init_command_line_shlib.h"
#include "chromecast/base/serializers.h"
#include "chromecast/media/cma/backend/audio_buildflags.h"
#include "chromecast/media/cma/backend/cast_audio_json.h"
#include "chromecast/media/cma/backend/post_processing_pipeline_parser.h"
#include "chromecast/media/cma/backend/stream_mixer.h"
#include "chromecast/media/cma/backend/system_volume_control.h"
#include "chromecast/media/cma/backend/volume_map.h"

namespace chromecast {
namespace media {

namespace {

constexpr float kDefaultMediaDbFS = -25.0f;
constexpr float kDefaultAlarmDbFS = -20.0f;
constexpr float kDefaultCommunicationDbFS = -25.0f;

constexpr float kMinDbFS = -120.0f;

constexpr char kKeyMediaDbFS[] = "dbfs.media";
constexpr char kKeyAlarmDbFS[] = "dbfs.alarm";
constexpr char kKeyCommunicationDbFS[] = "dbfs.communication";
constexpr char kKeyDefaultVolume[] = "default_volume";

float DbFsToScale(float db) {
  if (db <= kMinDbFS) {
    return 0.0f;
  }
  return std::pow(10, db / 20);
}

std::string ContentTypeToDbFSKey(AudioContentType type) {
  switch (type) {
    case AudioContentType::kAlarm:
      return kKeyAlarmDbFS;
    case AudioContentType::kCommunication:
      return kKeyCommunicationDbFS;
    default:
      return kKeyMediaDbFS;
  }
}

VolumeMap& GetVolumeMap() {
  static base::NoDestructor<VolumeMap> volume_map;
  return *volume_map;
}

class VolumeControlInternal : public SystemVolumeControl::Delegate {
 public:
  VolumeControlInternal()
      : thread_("VolumeControl"),
        initialize_complete_event_(
            base::WaitableEvent::ResetPolicy::MANUAL,
            base::WaitableEvent::InitialState::NOT_SIGNALED) {
    // Load volume map to check that the config file is correct.
    GetVolumeMap();

    stored_values_.SetDouble(kKeyMediaDbFS, kDefaultMediaDbFS);
    stored_values_.SetDouble(kKeyAlarmDbFS, kDefaultAlarmDbFS);
    stored_values_.SetDouble(kKeyCommunicationDbFS, kDefaultCommunicationDbFS);

    auto types = {AudioContentType::kMedia, AudioContentType::kAlarm,
                  AudioContentType::kCommunication};
    double volume;

    storage_path_ = base::GetHomeDir().Append("saved_volumes");
    auto old_stored_data = DeserializeJsonFromFile(storage_path_);
    base::DictionaryValue* old_stored_dict;
    if (old_stored_data && old_stored_data->GetAsDictionary(&old_stored_dict)) {
      for (auto type : types) {
        if (old_stored_dict->GetDouble(ContentTypeToDbFSKey(type), &volume)) {
          stored_values_.SetDouble(ContentTypeToDbFSKey(type), volume);
        }
      }
    } else {
      // If saved_volumes does not exist, use per device default if it exists.
      auto cast_audio_config =
          DeserializeJsonFromFile(base::FilePath(kCastAudioJsonFilePath));
      const base::DictionaryValue* cast_audio_dict;
      if (cast_audio_config &&
          cast_audio_config->GetAsDictionary(&cast_audio_dict)) {
        const base::DictionaryValue* default_volume_dict;
        if (cast_audio_dict && cast_audio_dict->GetDictionary(
                                   kKeyDefaultVolume, &default_volume_dict)) {
          for (auto type : types) {
            if (default_volume_dict->GetDouble(ContentTypeToDbFSKey(type),
                                               &volume)) {
              stored_values_.SetDouble(ContentTypeToDbFSKey(type), volume);
              LOG(INFO) << "Setting default volume for "
                        << ContentTypeToDbFSKey(type) << " to " << volume;
            }
          }
        }
      }
    }

    base::Thread::Options options;
    options.message_loop_type = base::MessageLoop::TYPE_IO;
    thread_.StartWithOptions(options);

    thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&VolumeControlInternal::InitializeOnThread,
                                  base::Unretained(this)));
    initialize_complete_event_.Wait();
  }

  ~VolumeControlInternal() override = default;

  void AddVolumeObserver(VolumeObserver* observer) {
    base::AutoLock lock(observer_lock_);
    volume_observers_.push_back(observer);
  }

  void RemoveVolumeObserver(VolumeObserver* observer) {
    base::AutoLock lock(observer_lock_);
    volume_observers_.erase(std::remove(volume_observers_.begin(),
                                        volume_observers_.end(), observer),
                            volume_observers_.end());
  }

  float GetVolume(AudioContentType type) {
    base::AutoLock lock(volume_lock_);
    return volumes_[type];
  }

  void SetVolume(AudioContentType type, float level) {
    if (type == AudioContentType::kOther) {
      NOTREACHED() << "Can't set volume for content type kOther";
      return;
    }

    level = std::max(0.0f, std::min(level, 1.0f));
    thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&VolumeControlInternal::SetVolumeOnThread,
                                  base::Unretained(this), type, level,
                                  false /* from_system */));
  }

  bool IsMuted(AudioContentType type) {
    base::AutoLock lock(volume_lock_);
    return muted_[type];
  }

  void SetMuted(AudioContentType type, bool muted) {
    if (type == AudioContentType::kOther) {
      NOTREACHED() << "Can't set mute state for content type kOther";
      return;
    }

    thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&VolumeControlInternal::SetMutedOnThread,
                                  base::Unretained(this), type, muted,
                                  false /* from_system */));
  }

  void SetOutputLimit(AudioContentType type, float limit) {
    if (type == AudioContentType::kOther) {
      NOTREACHED() << "Can't set output limit for content type kOther";
      return;
    }

    if (BUILDFLAG(SYSTEM_OWNS_VOLUME)) {
      return;
    }
    limit = std::max(0.0f, std::min(limit, 1.0f));
    StreamMixer::Get()->SetOutputLimit(
        type, DbFsToScale(VolumeControl::VolumeToDbFS(limit)));
  }

  void SetPowerSaveMode(bool power_save_on) {
    thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&VolumeControlInternal::SetPowerSaveModeOnThread,
                       base::Unretained(this), power_save_on));
  }

 private:
  void InitializeOnThread() {
    DCHECK(thread_.task_runner()->BelongsToCurrentThread());
    system_volume_control_ = SystemVolumeControl::Create(this);

    double dbfs;
    for (auto type : {AudioContentType::kMedia, AudioContentType::kAlarm,
                      AudioContentType::kCommunication}) {
      CHECK(stored_values_.GetDouble(ContentTypeToDbFSKey(type), &dbfs));
      volumes_[type] = VolumeControl::DbFSToVolume(dbfs);
      if (BUILDFLAG(SYSTEM_OWNS_VOLUME)) {
        // If ALSA owns volume, our internal mixer should not apply any scaling
        // multiplier.
        StreamMixer::Get()->SetVolume(type, 1.0f);
      } else {
        StreamMixer::Get()->SetVolume(type, DbFsToScale(dbfs));
      }

      // Note that mute state is not persisted across reboots.
      muted_[type] = false;
    }

    if (BUILDFLAG(SYSTEM_OWNS_VOLUME)) {
      // If ALSA owns the volume, then read the current volume and mute state
      // from the ALSA mixer element(s).
      volumes_[AudioContentType::kMedia] = system_volume_control_->GetVolume();
      muted_[AudioContentType::kMedia] = system_volume_control_->IsMuted();
    } else {
      // Otherwise, make sure the ALSA mixer element correctly reflects the
      // current volume state.
      system_volume_control_->SetVolume(volumes_[AudioContentType::kMedia]);
      system_volume_control_->SetMuted(false);
    }

    volumes_[AudioContentType::kOther] = 1.0;
    muted_[AudioContentType::kOther] = false;

    initialize_complete_event_.Signal();
  }

  void SetVolumeOnThread(AudioContentType type, float level, bool from_system) {
    DCHECK(thread_.task_runner()->BelongsToCurrentThread());
    DCHECK(type != AudioContentType::kOther);
    DCHECK(!from_system || type == AudioContentType::kMedia);

    {
      base::AutoLock lock(volume_lock_);
      if (from_system && system_volume_control_->GetRoundtripVolume(
                             volumes_[AudioContentType::kMedia]) == level) {
        return;
      }
      if (level == volumes_[type]) {
        return;
      }
      volumes_[type] = level;
    }

    float dbfs = VolumeControl::VolumeToDbFS(level);
    if (!BUILDFLAG(SYSTEM_OWNS_VOLUME)) {
      StreamMixer::Get()->SetVolume(type, DbFsToScale(dbfs));
    }

    if (!from_system && type == AudioContentType::kMedia) {
      system_volume_control_->SetVolume(level);
    }

    {
      base::AutoLock lock(observer_lock_);
      for (VolumeObserver* observer : volume_observers_) {
        observer->OnVolumeChange(type, level);
      }
    }

    stored_values_.SetDouble(ContentTypeToDbFSKey(type), dbfs);
    SerializeJsonToFile(storage_path_, stored_values_);
  }

  void SetMutedOnThread(AudioContentType type, bool muted, bool from_system) {
    DCHECK(thread_.task_runner()->BelongsToCurrentThread());
    DCHECK(type != AudioContentType::kOther);

    {
      base::AutoLock lock(volume_lock_);
      if (muted == muted_[type]) {
        return;
      }
      muted_[type] = muted;
    }

    if (!BUILDFLAG(SYSTEM_OWNS_VOLUME)) {
      StreamMixer::Get()->SetMuted(type, muted);
    }

    if (!from_system && type == AudioContentType::kMedia) {
      system_volume_control_->SetMuted(muted);
    }

    {
      base::AutoLock lock(observer_lock_);
      for (VolumeObserver* observer : volume_observers_) {
        observer->OnMuteChange(type, muted);
      }
    }
  }

  void SetPowerSaveModeOnThread(bool power_save_on) {
    DCHECK(thread_.task_runner()->BelongsToCurrentThread());
    system_volume_control_->SetPowerSave(power_save_on);
  }

  // SystemVolumeControl::Delegate implementation:
  void OnSystemVolumeOrMuteChange(float new_volume, bool new_mute) override {
    LOG(INFO) << "System volume/mute change, new volume = " << new_volume
              << ", new mute = " << new_mute;
    DCHECK(thread_.task_runner()->BelongsToCurrentThread());
    SetVolumeOnThread(AudioContentType::kMedia, new_volume,
                      true /* from_system */);
    SetMutedOnThread(AudioContentType::kMedia, new_mute,
                     true /* from_system */);
  }

  base::FilePath storage_path_;
  base::DictionaryValue stored_values_;

  base::Lock volume_lock_;
  std::map<AudioContentType, float> volumes_;
  std::map<AudioContentType, bool> muted_;

  base::Lock observer_lock_;
  std::vector<VolumeObserver*> volume_observers_;

  base::Thread thread_;
  base::WaitableEvent initialize_complete_event_;

  std::unique_ptr<SystemVolumeControl> system_volume_control_;

  DISALLOW_COPY_AND_ASSIGN(VolumeControlInternal);
};

// base::LazyInstance<VolumeControlInternal>::Leaky g_volume_control =
//     LAZY_INSTANCE_INITIALIZER;
VolumeControlInternal& GetVolumeControl() {
  static base::NoDestructor<VolumeControlInternal> g_volume_control;
  return *g_volume_control;
}

}  // namespace

// static
void VolumeControl::Initialize(const std::vector<std::string>& argv) {
  chromecast::InitCommandLineShlib(argv);
  GetVolumeControl();
}

// static
void VolumeControl::Finalize() {
  // Nothing to do.
}

// static
void VolumeControl::AddVolumeObserver(VolumeObserver* observer) {
  GetVolumeControl().AddVolumeObserver(observer);
}

// static
void VolumeControl::RemoveVolumeObserver(VolumeObserver* observer) {
  GetVolumeControl().RemoveVolumeObserver(observer);
}

// static
float VolumeControl::GetVolume(AudioContentType type) {
  return GetVolumeControl().GetVolume(type);
}

// static
void VolumeControl::SetVolume(AudioContentType type, float level) {
  GetVolumeControl().SetVolume(type, level);
}

// static
bool VolumeControl::IsMuted(AudioContentType type) {
  return GetVolumeControl().IsMuted(type);
}

// static
void VolumeControl::SetMuted(AudioContentType type, bool muted) {
  GetVolumeControl().SetMuted(type, muted);
}

// static
void VolumeControl::SetOutputLimit(AudioContentType type, float limit) {
  GetVolumeControl().SetOutputLimit(type, limit);
}

// static
float VolumeControl::VolumeToDbFS(float volume) {
  return GetVolumeMap().VolumeToDbFS(volume);
}

// static
float VolumeControl::DbFSToVolume(float db) {
  return GetVolumeMap().DbFSToVolume(db);
}

// static
void VolumeControl::SetPowerSaveMode(bool power_save_on) {
  GetVolumeControl().SetPowerSaveMode(power_save_on);
}

}  // namespace media
}  // namespace chromecast
