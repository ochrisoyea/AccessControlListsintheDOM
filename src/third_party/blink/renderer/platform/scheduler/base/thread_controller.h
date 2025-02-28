// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_BASE_THREAD_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_BASE_THREAD_CONTROLLER_H_

#include "base/location.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/platform/platform_export.h"

namespace base {
class TickClock;
struct PendingTask;
};

namespace base {
namespace sequence_manager {
namespace internal {

class SequencedTaskSource;

// Interface for TaskQueueManager to schedule work to be run.
class PLATFORM_EXPORT ThreadController {
 public:
  virtual ~ThreadController() = default;

  // Set the number of tasks executed in a single invocation of DoWork.
  // Increasing the batch size can reduce the overhead of yielding back to the
  // main message loop. The batch size is 1 by default.
  virtual void SetWorkBatchSize(int work_batch_size) = 0;

  // Notifies that |pending_task| was enqueued. Needed for tracing purposes.
  virtual void DidQueueTask(const PendingTask& pending_task) = 0;

  // Notify the controller that its associated sequence has immediate work
  // to run. Shortly after this is called, the thread associated with this
  // controller will run a task returned by sequence->TakeTask(). Can be called
  // from any sequence.
  //
  // TODO(altimin): Change this to "the thread associated with this
  // controller will run tasks returned by sequence->TakeTask() until it
  // returns null or sequence->DidRunTask() returns false" once the
  // code is changed to work that way.
  virtual void ScheduleWork() = 0;

  // Notify the controller that its associated sequence will have
  // delayed work to run at |run_time|. The thread associated with this
  // controller will run a task returned by sequence->TakeTask() at that time.
  // This call cancels any previously scheduled delayed work. Will be called
  // from the main sequence.
  //
  // TODO(altimin): Change this to "the thread associated with this
  // controller will run tasks returned by sequence->TakeTask() until
  // it returns null or sequence->DidRunTask() returns false" once the
  // code is changed to work that way.
  virtual void ScheduleDelayedWork(TimeTicks now, TimeTicks run_time) = 0;

  // Notify thread controller that sequence no longer has delayed work at
  // |run_time| and previously scheduled callbacks should be cancelled.
  virtual void CancelDelayedWork(TimeTicks run_time) = 0;

  // Sets the sequenced task source from which to take tasks after
  // a Schedule*Work() call is made.
  // Must be called before the first call to Schedule*Work().
  virtual void SetSequencedTaskSource(SequencedTaskSource*) = 0;

  // TODO(altimin): Get rid of the methods below.
  // These methods exist due to current integration of TaskQueueManager
  // with MessageLoop.

  virtual bool RunsTasksInCurrentSequence() = 0;

  virtual const TickClock* GetClock() = 0;

  virtual void SetDefaultTaskRunner(scoped_refptr<SingleThreadTaskRunner>) = 0;

  virtual void RestoreDefaultTaskRunner() = 0;

  virtual void AddNestingObserver(RunLoop::NestingObserver* observer) = 0;

  virtual void RemoveNestingObserver(RunLoop::NestingObserver* observer) = 0;
};

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_BASE_THREAD_CONTROLLER_H_
