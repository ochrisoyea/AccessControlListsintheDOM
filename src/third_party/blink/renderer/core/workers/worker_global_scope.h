/*
 * Copyright (C) 2008, 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 2. Redistributions in binary form must reproduce the above copyright
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_WORKERS_WORKER_GLOBAL_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_WORKERS_WORKER_GLOBAL_SCOPE_H_

#include <memory>
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "services/service_manager/public/mojom/interface_provider.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/active_script_wrappable.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_cache_options.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/dom/frame_request_callback_collection.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/dom_timer_coordinator.h"
#include "third_party/blink/renderer/core/frame/dom_window_base64.h"
#include "third_party/blink/renderer/core/script/script.h"
#include "third_party/blink/renderer/core/workers/worker_animation_frame_provider.h"
#include "third_party/blink/renderer/core/workers/worker_or_worklet_global_scope.h"
#include "third_party/blink/renderer/core/workers/worker_or_worklet_module_fetch_coordinator_proxy.h"
#include "third_party/blink/renderer/core/workers/worker_settings.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/loader/fetch/cached_metadata_handler.h"

namespace service_manager {
class InterfaceProvider;
}

namespace blink {

class FontFaceSet;
class ConsoleMessage;
class ExceptionState;
class OffscreenFontSelector;
class WorkerLocation;
class WorkerNavigator;
class WorkerThread;
struct GlobalScopeCreationParams;

class CORE_EXPORT WorkerGlobalScope
    : public WorkerOrWorkletGlobalScope,
      public ActiveScriptWrappable<WorkerGlobalScope>,
      public Supplementable<WorkerGlobalScope>,
      public DOMWindowBase64 {
  DEFINE_WRAPPERTYPEINFO();
  USING_GARBAGE_COLLECTED_MIXIN(WorkerGlobalScope);

 public:
  ~WorkerGlobalScope() override;

  // Returns null if caching is not supported.
  virtual SingleCachedMetadataHandler* CreateWorkerScriptCachedMetadataHandler(
      const KURL& script_url,
      const Vector<char>* meta_data) {
    return nullptr;
  }

  // WorkerOrWorkletGlobalScope
  bool IsClosing() const final { return closing_; }
  void Dispose() override;
  WorkerThread* GetThread() const final { return thread_; }

  void ExceptionUnhandled(int exception_id);

  // WorkerGlobalScope
  WorkerGlobalScope* self() { return this; }
  WorkerLocation* location() const;
  WorkerNavigator* navigator() const;
  void close();
  bool isSecureContextForBindings() const {
    return ExecutionContext::IsSecureContext();
  }

  String origin() const;

  DEFINE_ATTRIBUTE_EVENT_LISTENER(error);
  DEFINE_ATTRIBUTE_EVENT_LISTENER(rejectionhandled);
  DEFINE_ATTRIBUTE_EVENT_LISTENER(unhandledrejection);

  // WorkerUtils
  virtual void importScripts(const Vector<String>& urls, ExceptionState&);

  // ExecutionContext
  const KURL& Url() const final { return url_; }
  KURL CompleteURL(const String&) const final;
  bool IsWorkerGlobalScope() const final { return true; }
  bool IsContextThread() const final;
  const KURL& BaseURL() const final { return url_; }
  String UserAgent() const final { return user_agent_; }

  DOMTimerCoordinator* Timers() final { return &timers_; }
  SecurityContext& GetSecurityContext() final { return *this; }
  void AddConsoleMessage(ConsoleMessage*) final;
  bool IsSecureContext(String& error_message) const override;
  service_manager::InterfaceProvider* GetInterfaceProvider() final;

  OffscreenFontSelector* GetFontSelector() { return font_selector_; }

  CoreProbeSink* GetProbeSink() final;
  const base::UnguessableToken& GetParentDevToolsToken() {
    return parent_devtools_token_;
  }

  // EventTarget
  ExecutionContext* GetExecutionContext() const final;

  // Evaluates the given top-level classic script.
  virtual void EvaluateClassicScript(
      const KURL& script_url,
      String source_code,
      std::unique_ptr<Vector<char>> cached_meta_data);

  // Imports the top-level module script for |module_url_record|.
  void ImportModuleScript(const KURL& module_url_record,
                          network::mojom::FetchCredentialsMode);

  double TimeOrigin() const { return time_origin_; }
  WorkerSettings* GetWorkerSettings() const { return worker_settings_.get(); }

  void Trace(blink::Visitor*) override;
  void TraceWrappers(ScriptWrappableVisitor*) const override;

  // TODO(fserb): This can be removed once we WorkerGlobalScope implements
  // FontFaceSource on the IDL.
  FontFaceSet* fonts();

  int requestAnimationFrame(V8FrameRequestCallback* callback);
  void cancelAnimationFrame(int id);

  WorkerAnimationFrameProvider* GetAnimationFrameProvider() {
    return animation_frame_provider_;
  }

 protected:
  WorkerGlobalScope(std::unique_ptr<GlobalScopeCreationParams>,
                    WorkerThread*,
                    double time_origin);
  void ApplyContentSecurityPolicyFromHeaders(
      const ContentSecurityPolicyResponseHeaders&);

  // ExecutionContext
  void ExceptionThrown(ErrorEvent*) override;
  void RemoveURLFromMemoryCache(const KURL&) final;

 private:
  void SetWorkerSettings(std::unique_ptr<WorkerSettings>);

  // |kNotHandled| is used when the script was not in
  // InstalledScriptsManager, which means it was not an installed script.
  enum class LoadResult { kSuccess, kFailed, kNotHandled };

  // Tries to load the script synchronously from the
  // InstalledScriptsManager, which holds scripts that are sent from the browser
  // upon starting an installed worker. This blocks until the script is
  // received. If the script load could not be handled by the
  // InstalledScriptsManager, e.g. when the script was not an installed script,
  // returns LoadResult::kNotHandled.
  // TODO(crbug.com/753350): Factor out LoadScriptFrom* into a new class which
  // provides the worker's scripts.
  LoadResult LoadScriptFromInstalledScriptsManager(
      const KURL& script_url,
      KURL* out_response_url,
      String* out_source_code,
      std::unique_ptr<Vector<char>>* out_cached_meta_data);

  // Tries to load the script synchronously from the WorkerClassicScriptLoader,
  // which requests the script from the browser. This blocks until the script is
  // received.
  LoadResult LoadScriptFromClassicScriptLoader(
      const KURL& script_url,
      KURL* out_response_url,
      String* out_source_code,
      std::unique_ptr<Vector<char>>* out_cached_meta_data);

  // ExecutionContext
  EventTarget* ErrorEventTarget() final { return this; }

  const KURL url_;
  const ScriptType script_type_;
  const String user_agent_;
  const base::UnguessableToken parent_devtools_token_;
  const V8CacheOptions v8_cache_options_;
  std::unique_ptr<WorkerSettings> worker_settings_;

  mutable Member<WorkerLocation> location_;
  mutable TraceWrapperMember<WorkerNavigator> navigator_;

  WorkerThread* thread_;

  bool closing_ = false;

  DOMTimerCoordinator timers_;

  const double time_origin_;

  HeapHashMap<int, Member<ErrorEvent>> pending_error_events_;
  int last_pending_error_event_id_ = 0;

  Member<OffscreenFontSelector> font_selector_;
  Member<WorkerAnimationFrameProvider> animation_frame_provider_;

  service_manager::InterfaceProvider interface_provider_;
};

DEFINE_TYPE_CASTS(WorkerGlobalScope,
                  ExecutionContext,
                  context,
                  context->IsWorkerGlobalScope(),
                  context.IsWorkerGlobalScope());

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_WORKERS_WORKER_GLOBAL_SCOPE_H_
