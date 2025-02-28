/*
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_WORKER_THREADABLE_LOADER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_WORKER_THREADABLE_LOADER_H_

#include <memory>
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/single_thread_task_runner.h"
#include "third_party/blink/renderer/core/loader/threadable_loader.h"
#include "third_party/blink/renderer/core/loader/threadable_loader_client.h"
#include "third_party/blink/renderer/core/workers/worker_thread.h"
#include "third_party/blink/renderer/core/workers/worker_thread_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/waitable_event.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class ThreadableLoadingContext;
class ResourceError;
class ResourceRequest;
class ResourceResponse;
class WorkerGlobalScope;
class WorkerThreadLifecycleContext;
struct CrossThreadResourceRequestData;
struct CrossThreadResourceTimingInfoData;

// A WorkerThreadableLoader is a ThreadableLoader implementation intended to
// be used in a WebWorker thread. Because Blink's ResourceFetcher and
// ResourceLoader work only in the main thread, a WorkerThreadableLoader holds
// a ThreadableLoader in the main thread and delegates tasks asynchronously
// to the loader.
//
// CTP: CrossThreadPersistent
// CTWP: CrossThreadWeakPersistent
//
// ----------------------------------------------------------------
//                 +------------------------+
//       raw ptr   | ThreadableLoaderClient |
//      +--------> | worker thread          |
//      |          +------------------------+
//      |
// +----+------------------+    CTP  +------------------------+
// + WorkerThreadableLoader|<--------+ MainThreadLoaderHolder |
// | worker thread         +-------->| main thread            |
// +-----------------------+   CTWP  +----------------------+-+
//                                                          |
//                                 +------------------+     | Member
//                                 | ThreadableLoader | <---+
//                                 |      main thread |
//                                 +------------------+
//
class WorkerThreadableLoader final : public ThreadableLoader {
 public:
  static void LoadResourceSynchronously(WorkerGlobalScope&,
                                        const ResourceRequest&,
                                        ThreadableLoaderClient&,
                                        const ThreadableLoaderOptions&,
                                        const ResourceLoaderOptions&);
  ~WorkerThreadableLoader() override;

  // ThreadableLoader functions
  void Start(const ResourceRequest&) override;
  void OverrideTimeout(unsigned long timeout) override;
  void Cancel() override;
  void Detach() override;

  void Trace(blink::Visitor*) override;

 private:
  // A TaskForwarder forwards a task to the worker thread.
  class TaskForwarder : public GarbageCollectedFinalized<TaskForwarder> {
   public:
    virtual ~TaskForwarder() = default;
    virtual void ForwardTask(const base::Location&, CrossThreadClosure) = 0;
    virtual void ForwardTaskWithDoneSignal(const base::Location&,
                                           CrossThreadClosure) = 0;
    virtual void Abort() = 0;

    virtual void Trace(blink::Visitor* visitor) {}
  };
  class AsyncTaskForwarder;
  struct TaskWithLocation;
  class WaitableEventWithTasks;
  class SyncTaskForwarder;

  // An instance of this class lives in the main thread. It is a
  // ThreadableLoaderClient for a DocumentThreadableLoader and forward
  // notifications to the associated WorkerThreadableLoader living in the
  // worker thread.
  class MainThreadLoaderHolder final
      : public GarbageCollectedFinalized<MainThreadLoaderHolder>,
        public ThreadableLoaderClient,
        public WorkerThreadLifecycleObserver {
    USING_GARBAGE_COLLECTED_MIXIN(MainThreadLoaderHolder);
    USING_PRE_FINALIZER(MainThreadLoaderHolder, Cancel);

   public:
    static void CreateAndStart(WorkerThreadableLoader*,
                               ThreadableLoadingContext*,
                               scoped_refptr<base::SingleThreadTaskRunner>,
                               WorkerThreadLifecycleContext*,
                               std::unique_ptr<CrossThreadResourceRequestData>,
                               const ThreadableLoaderOptions&,
                               const ResourceLoaderOptions&,
                               scoped_refptr<WaitableEventWithTasks>);
    ~MainThreadLoaderHolder() override;

    void OverrideTimeout(unsigned long timeout_millisecond);
    void Cancel();

    void DidSendData(unsigned long long bytes_sent,
                     unsigned long long total_bytes_to_be_sent) override;
    void DidReceiveRedirectTo(const KURL&) override;
    void DidReceiveResponse(unsigned long identifier,
                            const ResourceResponse&,
                            std::unique_ptr<WebDataConsumerHandle>) override;
    void DidReceiveData(const char*, unsigned data_length) override;
    void DidDownloadData(int data_length) override;
    void DidDownloadToBlob(scoped_refptr<BlobDataHandle>) override;
    void DidReceiveCachedMetadata(const char*, int data_length) override;
    void DidFinishLoading(unsigned long identifier) override;
    void DidFail(const ResourceError&) override;
    void DidFailRedirectCheck() override;
    void DidReceiveResourceTiming(const ResourceTimingInfo&) override;

    void ContextDestroyed(WorkerThreadLifecycleContext*) override;

    void Trace(blink::Visitor*) override;

   private:
    MainThreadLoaderHolder(TaskForwarder*, WorkerThreadLifecycleContext*);
    void Start(ThreadableLoadingContext&,
               std::unique_ptr<CrossThreadResourceRequestData>,
               const ThreadableLoaderOptions&,
               const ResourceLoaderOptions&);

    Member<TaskForwarder> forwarder_;
    Member<ThreadableLoader> main_thread_loader_;

    // |*m_workerLoader| lives in the worker thread.
    CrossThreadWeakPersistent<WorkerThreadableLoader> worker_loader_;
  };

  WorkerThreadableLoader(WorkerGlobalScope&,
                         ThreadableLoaderClient*,
                         const ThreadableLoaderOptions&,
                         const ResourceLoaderOptions&);
  void DidStart(MainThreadLoaderHolder*);

  void DidSendData(unsigned long long bytes_sent,
                   unsigned long long total_bytes_to_be_sent);
  void DidReceiveRedirectTo(const KURL&);
  void DidReceiveResponse(unsigned long identifier,
                          std::unique_ptr<CrossThreadResourceResponseData>,
                          std::unique_ptr<WebDataConsumerHandle>);
  void DidReceiveData(std::unique_ptr<Vector<char>> data);
  void DidReceiveCachedMetadata(std::unique_ptr<Vector<char>> data);
  void DidFinishLoading(unsigned long identifier);
  void DidFail(const ResourceError&);
  void DidFailRedirectCheck();
  void DidDownloadData(int data_length);
  void DidDownloadToBlob(scoped_refptr<BlobDataHandle>);
  void DidReceiveResourceTiming(
      std::unique_ptr<CrossThreadResourceTimingInfoData>);

  Member<WorkerGlobalScope> worker_global_scope_;
  CrossThreadPersistent<ParentExecutionContextTaskRunners>
      parent_execution_context_task_runners_;
  ThreadableLoaderClient* client_;

  ThreadableLoaderOptions threadable_loader_options_;
  ResourceLoaderOptions resource_loader_options_;

  // |*m_mainThreadLoaderHolder| lives in the main thread.
  CrossThreadPersistent<MainThreadLoaderHolder> main_thread_loader_holder_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_WORKER_THREADABLE_LOADER_H_
