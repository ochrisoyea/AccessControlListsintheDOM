// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_MOJO_SERVICES_VIDEO_DECODE_PERF_HISTORY_H_
#define MEDIA_MOJO_SERVICES_VIDEO_DECODE_PERF_HISTORY_H_

#include <stdint.h>
#include <memory>
#include <queue>
#include <string>

#include "base/callback.h"
#include "base/sequence_checker.h"
#include "base/supports_user_data.h"
#include "media/base/video_codecs.h"
#include "media/capabilities/video_decode_stats_db.h"
#include "media/mojo/interfaces/video_decode_perf_history.mojom.h"
#include "media/mojo/services/media_mojo_export.h"
#include "mojo/public/cpp/bindings/binding_set.h"
#include "ui/gfx/geometry/size.h"
#include "url/origin.h"

namespace media {

// This class saves and retrieves video decode performance statistics on behalf
// of the MediaCapabilities API. It also helps to grade the accuracy of the API
// by comparing its history-based assessment of smoothness/power-efficiency to
// the observed performance as new stats are saved.
//
// The database is lazily initialized/loaded upon the first API call requiring
// DB access. DB implementations must take care to perform work on a separate
// task runner.
//
// Retrieving stats is triggered by calls to the GetPerfInfo() Mojo interface.
// The raw values are reduced to booleans (is_smooth, is_power_efficient) which
// are sent along the Mojo callback.
//
// Saving stats is performed by SavePerfRecord(), where a record is defined as a
// continuous playback of a stream with fixed decode characteristics (profile,
// natural size, frame rate).
//
// THREAD SAFETY:
// This class is not thread safe. All API calls should be made on the same
// sequence.
class MEDIA_MOJO_EXPORT VideoDecodePerfHistory
    : public mojom::VideoDecodePerfHistory,
      public base::SupportsUserData::Data {
 public:
  explicit VideoDecodePerfHistory(
      std::unique_ptr<VideoDecodeStatsDBFactory> db_factory);
  ~VideoDecodePerfHistory() override;

  // Bind the mojo request to this instance. Single instance will be used to
  // serve multiple requests.
  void BindRequest(mojom::VideoDecodePerfHistoryRequest request);

  // mojom::VideoDecodePerfHistory implementation:
  void GetPerfInfo(mojom::PredictionFeaturesPtr features,
                   GetPerfInfoCallback got_info_cb) override;

  // Save a record of the given performance stats for the described stream.
  // Saving is generally fire-and-forget, but |save_done_cb| may be optionally
  // for tests to know the save is complete.
  void SavePerfRecord(const url::Origin& untrusted_top_frame_origin,
                      bool is_top_frame,
                      mojom::PredictionFeatures features,
                      mojom::PredictionTargets targets,
                      uint64_t player_id,
                      base::OnceClosure save_done_cb = base::OnceClosure());

  // Clear all history from the underlying database. Run |clear_done_cb| when
  // complete.
  void ClearHistory(base::OnceClosure clear_done_cb);

 private:
  friend class VideoDecodePerfHistoryTest;

  // Track the status of database lazy initialization.
  enum InitStatus {
    UNINITIALIZED,
    PENDING,
    COMPLETE,
    FAILED,
  };

  // Decode capabilities will be described as "smooth" whenever the percentage
  // of dropped frames is less-than-or-equal-to this value. 10% chosen as a
  // lenient value after manual testing.
  static constexpr double kMaxSmoothDroppedFramesPercent = .10;

  // Decode capabilities will be described as "power efficient" whenever the
  // percentage of power efficient decoded frames is higher-than-or-equal-to
  // this value.
  static constexpr double kMinPowerEfficientDecodedFramePercent = .50;

  // Create and initialize the database. Will return early if initialization is
  // already PENDING.
  void InitDatabase();

  // Callback from |db_->Initialize()|.
  void OnDatabaseInit(bool success);

  // Internal callback for database queries made from GetPerfInfo() (mojo API).
  // Assesses performance from database stats and passes results to
  // |got_info_cb|.
  void OnGotStatsForRequest(
      const VideoDecodeStatsDB::VideoDescKey& video_key,
      GetPerfInfoCallback got_info_cb,
      bool database_success,
      std::unique_ptr<VideoDecodeStatsDB::DecodeStatsEntry> stats);

  // Internal callback for database queries made from SavePerfRecord(). Compares
  // past performance to this latest record as means of "grading" the accuracy
  // of the GetPerfInfo() API. Comparison is recorded via UKM. Then saves the
  // |new_*| performance stats to the database.
  void OnGotStatsForSave(
      const url::Origin& top_frame_origin,
      bool is_top_frame,
      uint64_t player_id,
      const VideoDecodeStatsDB::VideoDescKey& video_key,
      const VideoDecodeStatsDB::DecodeStatsEntry& new_stats,
      base::OnceClosure save_done_cb,
      bool success,
      std::unique_ptr<VideoDecodeStatsDB::DecodeStatsEntry> past_stats);

  // Internal callback for saving to database. Will run |save_done_cb| if
  // nonempty.
  void OnSaveDone(base::OnceClosure save_done_cb, bool success);

  // Report UKM metrics to grade the claims of the API by evaluating how well
  // |past_stats| predicts |new_stats|.
  void ReportUkmMetrics(const url::Origin& top_frame_origin,
                        bool is_top_frame,
                        uint64_t player_id,
                        const VideoDecodeStatsDB::VideoDescKey& video_key,
                        const VideoDecodeStatsDB::DecodeStatsEntry& new_stats,
                        VideoDecodeStatsDB::DecodeStatsEntry* past_stats);

  void AssessStats(const VideoDecodeStatsDB::DecodeStatsEntry* stats,
                   bool* is_smooth,
                   bool* is_power_efficient);

  // Internal callback for ClearHistory(). Reinitializes the database and runs
  // |clear_done_cb|.
  void OnClearedHistory(base::OnceClosure clear_done_cb);

  // Factory for creating |db_|.
  std::unique_ptr<VideoDecodeStatsDBFactory> db_factory_;

  // Tracks whether we've received OnDatabaseIniti() callback. All database
  // operations should be deferred until initialization is complete.
  InitStatus db_init_status_;

  // Database helper for managing/coalescing decode stats.
  // TODO(chcunningham): tear down |db_| if idle for extended period.
  std::unique_ptr<VideoDecodeStatsDB> db_;

  // Vector of bound public API calls, to be run once DB initialization
  // completes.
  std::vector<base::OnceClosure> init_deferred_api_calls_;

  // Maps bindings from several render-processes to this single browser-process
  // service.
  mojo::BindingSet<mojom::VideoDecodePerfHistory> bindings_;

  // Ensures all access to class members come on the same sequence.
  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtrFactory<VideoDecodePerfHistory> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(VideoDecodePerfHistory);
};

}  // namespace media

#endif  // MEDIA_MOJO_SERVICES_VIDEO_DECODE_PERF_HISTORY_H_