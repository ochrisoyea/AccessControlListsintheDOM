// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/resource_coordinator/tab_manager.h"

#include <stddef.h>

#include <algorithm>
#include <set>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/feature_list.h"
#include "base/memory/memory_pressure_monitor.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram_macros.h"
#include "base/process/process.h"
#include "base/rand_util.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/sys_info.h"
#include "base/threading/thread.h"
#include "base/trace_event/trace_event_argument.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"
#include "chrome/browser/memory/oom_memory_details.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/resource_coordinator/background_tab_navigation_throttle.h"
#include "chrome/browser/resource_coordinator/resource_coordinator_web_contents_observer.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/resource_coordinator/tab_manager_features.h"
#include "chrome/browser/resource_coordinator/tab_manager_resource_coordinator_signal_observer.h"
#include "chrome/browser/resource_coordinator/tab_manager_stats_collector.h"
#include "chrome/browser/resource_coordinator/tab_manager_web_contents_data.h"
#include "chrome/browser/resource_coordinator/time.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tab_contents/tab_contents_iterator.h"
#include "chrome/browser/ui/tab_ui_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/url_constants.h"
#include "components/metrics/system_memory_stats_recorder.h"
#include "components/variations/variations_associated_data.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/favicon_status.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"
#include "content/public/common/page_importance_signals.h"
#include "third_party/blink/public/platform/web_sudden_termination_disabler_type.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/resource_coordinator/tab_manager_delegate_chromeos.h"
#endif

using base::TimeDelta;
using base::TimeTicks;
using content::BrowserThread;
using content::WebContents;

namespace resource_coordinator {
namespace {

// The default timeout time after which the next background tab gets loaded if
// the previous tab has not finished loading yet. This is ignored in kPaused
// loading mode.
const TimeDelta kDefaultBackgroundTabLoadTimeout = TimeDelta::FromSeconds(10);

// The number of loading slots for background tabs. TabManager will start to
// load the next background tab when the loading slots free up.
const size_t kNumOfLoadingSlots = 1;

// The default interval in seconds after which to adjust the oom_score_adj
// value.
const int kAdjustmentIntervalSeconds = 10;

struct LifecycleUnitAndSortKey {
  explicit LifecycleUnitAndSortKey(LifecycleUnit* lifecycle_unit)
      : lifecycle_unit(lifecycle_unit),
        sort_key(lifecycle_unit->GetSortKey()) {}

  bool operator<(const LifecycleUnitAndSortKey& other) const {
    return sort_key < other.sort_key;
  }
  bool operator>(const LifecycleUnitAndSortKey& other) const {
    return sort_key > other.sort_key;
  }

  LifecycleUnit* lifecycle_unit;
  LifecycleUnit::SortKey sort_key;
};

std::unique_ptr<base::trace_event::ConvertableToTraceFormat> DataAsTraceValue(
    TabManager::BackgroundTabLoadingMode mode,
    size_t num_of_pending_navigations,
    size_t num_of_loading_contents) {
  std::unique_ptr<base::trace_event::TracedValue> data(
      new base::trace_event::TracedValue());
  data->SetInteger("background_tab_loading_mode", mode);
  data->SetInteger("num_of_pending_navigations", num_of_pending_navigations);
  data->SetInteger("num_of_loading_contents", num_of_loading_contents);
  return std::move(data);
}

int GetNumLoadedLifecycleUnits(LifecycleUnitSet lifecycle_unit_set) {
  int num_loaded_lifecycle_units = 0;
  for (auto* lifecycle_unit : lifecycle_unit_set) {
    if (lifecycle_unit->GetState() != LifecycleState::DISCARDED)
      num_loaded_lifecycle_units++;
  }
  return num_loaded_lifecycle_units;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// TabManager

class TabManager::TabManagerSessionRestoreObserver final
    : public SessionRestoreObserver {
 public:
  explicit TabManagerSessionRestoreObserver(TabManager* tab_manager)
      : tab_manager_(tab_manager) {
    SessionRestore::AddObserver(this);
  }

  ~TabManagerSessionRestoreObserver() { SessionRestore::RemoveObserver(this); }

  // SessionRestoreObserver implementation:
  void OnSessionRestoreStartedLoadingTabs() override {
    tab_manager_->OnSessionRestoreStartedLoadingTabs();
  }

  void OnSessionRestoreFinishedLoadingTabs() override {
    tab_manager_->OnSessionRestoreFinishedLoadingTabs();
  }

  void OnWillRestoreTab(WebContents* web_contents) override {
    tab_manager_->OnWillRestoreTab(web_contents);
  }

 private:
  TabManager* tab_manager_;
};

constexpr base::TimeDelta TabManager::kDefaultMinTimeToPurge;

TabManager::TabManager()
    : browser_tab_strip_tracker_(this, nullptr, nullptr),
      is_session_restore_loading_tabs_(false),
      restored_tab_count_(0u),
      background_tab_loading_mode_(BackgroundTabLoadingMode::kStaggered),
      loading_slots_(kNumOfLoadingSlots),
      weak_ptr_factory_(this) {
#if defined(OS_CHROMEOS)
  delegate_.reset(new TabManagerDelegate(weak_ptr_factory_.GetWeakPtr()));
#endif
  browser_tab_strip_tracker_.Init();
  session_restore_observer_.reset(new TabManagerSessionRestoreObserver(this));
  if (PageSignalReceiver::IsEnabled()) {
    resource_coordinator_signal_observer_.reset(
        new ResourceCoordinatorSignalObserver());
  }
  stats_collector_.reset(new TabManagerStatsCollector());

  proactive_discard_params_ = GetStaticProactiveTabDiscardParams();
}

TabManager::~TabManager() {
  resource_coordinator_signal_observer_.reset();
  Stop();
}

void TabManager::Start() {
  background_tab_loading_mode_ = BackgroundTabLoadingMode::kStaggered;

#if defined(OS_WIN) || defined(OS_MACOSX)
  // Note that discarding is now enabled by default. This check is kept as a
  // kill switch.
  // TODO(georgesak): remote this when deemed not needed anymore.
  if (!base::FeatureList::IsEnabled(features::kAutomaticTabDiscarding))
    return;
#endif

  if (!update_timer_.IsRunning()) {
    update_timer_.Start(FROM_HERE,
                        TimeDelta::FromSeconds(kAdjustmentIntervalSeconds),
                        this, &TabManager::UpdateTimerCallback);
  }

// MemoryPressureMonitor is not implemented on Linux so far and tabs are never
// discarded.
#if defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_CHROMEOS)
  // Create a |MemoryPressureListener| to listen for memory events when
  // MemoryCoordinator is disabled. When MemoryCoordinator is enabled
  // it asks TabManager to do tab discarding.
  base::MemoryPressureMonitor* monitor = base::MemoryPressureMonitor::Get();
  if (monitor && !base::FeatureList::IsEnabled(features::kMemoryCoordinator)) {
    memory_pressure_listener_.reset(new base::MemoryPressureListener(
        base::Bind(&TabManager::OnMemoryPressure, base::Unretained(this))));
    base::MemoryPressureListener::MemoryPressureLevel level =
        monitor->GetCurrentPressureLevel();
    if (level == base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL) {
      OnMemoryPressure(level);
    }
  }
#endif
  // purge-and-suspend param is used for Purge+Suspend finch experiment
  // in the following way:
  // https://docs.google.com/document/d/1hPHkKtXXBTlsZx9s-9U17XC-ofEIzPo9FYbBEc7PPbk/edit?usp=sharing
  std::string purge_and_suspend_time = variations::GetVariationParamValue(
      "PurgeAndSuspendAggressive", "purge-and-suspend-time");
  unsigned int min_time_to_purge_sec = 0;
  if (purge_and_suspend_time.empty() ||
      !base::StringToUint(purge_and_suspend_time, &min_time_to_purge_sec))
    min_time_to_purge_ = kDefaultMinTimeToPurge;
  else
    min_time_to_purge_ = base::TimeDelta::FromSeconds(min_time_to_purge_sec);

  std::string max_purge_and_suspend_time = variations::GetVariationParamValue(
      "PurgeAndSuspendAggressive", "max-purge-and-suspend-time");
  unsigned int max_time_to_purge_sec = 0;
  // If max-purge-and-suspend-time is not specified or
  // max-purge-and-suspend-time is not valid (not number or smaller than
  // min-purge-and-suspend-time), use default max-time-to-purge, i.e.
  // min-time-to-purge times kDefaultMinMaxTimeToPurgeRatio.
  if (max_purge_and_suspend_time.empty() ||
      !base::StringToUint(max_purge_and_suspend_time, &max_time_to_purge_sec) ||
      max_time_to_purge_sec < min_time_to_purge_.InSeconds())
    max_time_to_purge_ = min_time_to_purge_ * kDefaultMinMaxTimeToPurgeRatio;
  else
    max_time_to_purge_ = base::TimeDelta::FromSeconds(max_time_to_purge_sec);
}

void TabManager::Stop() {
  update_timer_.Stop();
  force_load_timer_.reset();
  memory_pressure_listener_.reset();
}

LifecycleUnitVector TabManager::GetSortedLifecycleUnits() {
  std::vector<LifecycleUnitAndSortKey> lifecycle_units_and_sort_keys;
  lifecycle_units_and_sort_keys.reserve(lifecycle_units_.size());
  for (auto* lifecycle_unit : lifecycle_units_)
    lifecycle_units_and_sort_keys.emplace_back(lifecycle_unit);
  std::sort(lifecycle_units_and_sort_keys.begin(),
            lifecycle_units_and_sort_keys.end());

  LifecycleUnitVector sorted_lifecycle_units;
  sorted_lifecycle_units.reserve(lifecycle_units_.size());
  for (auto& lifecycle_unit_and_sort_key : lifecycle_units_and_sort_keys) {
    sorted_lifecycle_units.push_back(
        lifecycle_unit_and_sort_key.lifecycle_unit);
  }

  return sorted_lifecycle_units;
}

void TabManager::DiscardTab(DiscardReason reason) {
  if (reason == DiscardReason::kUrgent)
    stats_collector_->RecordWillDiscardUrgently(GetNumAliveTabs());

#if defined(OS_CHROMEOS)
  // Call Chrome OS specific low memory handling process.
  if (base::FeatureList::IsEnabled(features::kArcMemoryManagement)) {
    delegate_->LowMemoryKill(reason);
    return;
  }
#endif
  DiscardTabImpl(reason);
}

WebContents* TabManager::DiscardTabByExtension(content::WebContents* contents) {
  if (contents) {
    TabLifecycleUnitExternal* tab_lifecycle_unit_external =
        TabLifecycleUnitExternal::FromWebContents(contents);
    DCHECK(tab_lifecycle_unit_external);
    if (tab_lifecycle_unit_external->DiscardTab())
      return tab_lifecycle_unit_external->GetWebContents();
    return nullptr;
  }

  return DiscardTabImpl(DiscardReason::kExternal);
}

void TabManager::LogMemoryAndDiscardTab(DiscardReason reason) {
  LogMemory("Tab Discards Memory details",
            base::Bind(&TabManager::PurgeMemoryAndDiscardTab, reason));
}

void TabManager::LogMemory(const std::string& title,
                           const base::Closure& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  memory::OomMemoryDetails::Log(title, callback);
}

void TabManager::AddObserver(TabLifecycleObserver* observer) {
  TabLifecycleUnitExternal::AddTabLifecycleObserver(observer);
}

void TabManager::RemoveObserver(TabLifecycleObserver* observer) {
  TabLifecycleUnitExternal::RemoveTabLifecycleObserver(observer);
}

bool TabManager::CanPurgeBackgroundedRenderer(int render_process_id) const {
  for (LifecycleUnit* lifecycle_unit : lifecycle_units_) {
    TabLifecycleUnitExternal* tab_lifecycle_unit_external =
        lifecycle_unit->AsTabLifecycleUnitExternal();
    // For now, all LifecycleUnits are TabLifecycleUnitExternals.
    DCHECK(tab_lifecycle_unit_external);
    content::WebContents* content =
        tab_lifecycle_unit_external->GetWebContents();
    DCHECK(content);

    if (content->IsCrashed())
      continue;
    if (content->GetMainFrame()->GetProcess()->GetID() != render_process_id)
      continue;
    if (!lifecycle_unit->CanPurge())
      return false;
  }
  return true;
}

bool TabManager::IsTabInSessionRestore(WebContents* web_contents) const {
  return GetWebContentsData(web_contents)->is_in_session_restore();
}

bool TabManager::IsTabRestoredInForeground(WebContents* web_contents) const {
  return GetWebContentsData(web_contents)->is_restored_in_foreground();
}

size_t TabManager::GetBackgroundTabLoadingCount() const {
  if (!IsInBackgroundTabOpeningSession())
    return 0;

  return loading_contents_.size();
}

size_t TabManager::GetBackgroundTabPendingCount() const {
  if (!IsInBackgroundTabOpeningSession())
    return 0;

  return pending_navigations_.size();
}

int TabManager::GetTabCount() const {
  int tab_count = 0;
  for (auto* browser : *BrowserList::GetInstance())
    tab_count += browser->tab_strip_model()->count();
  return tab_count;
}

int TabManager::restored_tab_count() const {
  return restored_tab_count_;
}

///////////////////////////////////////////////////////////////////////////////
// TabManager, private:

// static
void TabManager::PurgeMemoryAndDiscardTab(DiscardReason reason) {
  TabManager* manager = g_browser_process->GetTabManager();
  manager->PurgeBrowserMemory();
  manager->DiscardTab(reason);
}

// static
bool TabManager::IsInternalPage(const GURL& url) {
  // There are many chrome:// UI URLs, but only look for the ones that users
  // are likely to have open. Most of the benefit is the from NTP URL.
  const char* const kInternalPagePrefixes[] = {
      chrome::kChromeUIDownloadsURL, chrome::kChromeUIHistoryURL,
      chrome::kChromeUINewTabURL, chrome::kChromeUISettingsURL};
  // Prefix-match against the table above. Use strncmp to avoid allocating
  // memory to convert the URL prefix constants into std::strings.
  for (size_t i = 0; i < arraysize(kInternalPagePrefixes); ++i) {
    if (!strncmp(url.spec().c_str(), kInternalPagePrefixes[i],
                 strlen(kInternalPagePrefixes[i])))
      return true;
  }
  return false;
}

void TabManager::PurgeBrowserMemory() {
  // Based on experimental evidence, attempts to free memory from renderers
  // have been too slow to use in OOM situations (V8 garbage collection) or
  // do not lead to persistent decreased usage (image/bitmap caches). This
  // function therefore only targets large blocks of memory in the browser.
  // Note that other objects will listen to MemoryPressureListener events
  // to release memory.
  for (auto* web_contents : AllTabContentses()) {
    // Screenshots can consume ~5 MB per web contents for platforms that do
    // touch back/forward.
    web_contents->GetController().ClearAllScreenshots();
  }
}

// This function is called when |update_timer_| fires. It will adjust the clock
// if needed (if it detects that the machine was asleep) and will fire the stats
// updating on ChromeOS via the delegate. This function also tries to purge
// cache memory.
void TabManager::UpdateTimerCallback() {
  // If Chrome is shutting down, do not do anything.
  if (g_browser_process->IsShuttingDown())
    return;

  if (BrowserList::GetInstance()->empty())
    return;

#if defined(OS_CHROMEOS)
  // This starts the CrOS specific OOM adjustments in /proc/<pid>/oom_score_adj.
  delegate_->AdjustOomPriorities();
#endif

  PurgeBackgroundedTabsIfNeeded();
}

base::TimeDelta TabManager::GetTimeToPurge(
    base::TimeDelta min_time_to_purge,
    base::TimeDelta max_time_to_purge) const {
  return base::TimeDelta::FromSeconds(base::RandInt(
      min_time_to_purge.InSeconds(), max_time_to_purge.InSeconds()));
}

bool TabManager::ShouldPurgeNow(content::WebContents* content) const {
  if (GetWebContentsData(content)->is_purged())
    return false;
  if (TabLifecycleUnitExternal::FromWebContents(content)->IsDiscarded())
    return false;

  base::TimeDelta time_passed =
      NowTicks() - GetWebContentsData(content)->LastInactiveTime();
  return time_passed > GetWebContentsData(content)->time_to_purge();
}

void TabManager::PurgeBackgroundedTabsIfNeeded() {
  for (LifecycleUnit* lifecycle_unit : lifecycle_units_) {
    TabLifecycleUnitExternal* tab_lifecycle_unit_external =
        lifecycle_unit->AsTabLifecycleUnitExternal();
    // For now, all LifecycleUnits are TabLifecycleUnitExternals.
    DCHECK(tab_lifecycle_unit_external);
    content::WebContents* content =
        tab_lifecycle_unit_external->GetWebContents();
    DCHECK(content);

    if (content->IsCrashed())
      continue;

    content::RenderProcessHost* render_process_host =
        content->GetMainFrame()->GetProcess();
    int render_process_id = render_process_host->GetID();

    if (!render_process_host->IsProcessBackgrounded())
      continue;
    if (!CanPurgeBackgroundedRenderer(render_process_id))
      continue;

    bool purge_now = ShouldPurgeNow(content);
    if (!purge_now)
      continue;

    // Since |content|'s tab is kept inactive and background for more than
    // time-to-purge time, its purged state changes: false => true.
    GetWebContentsData(content)->set_is_purged(true);
    // TODO(tasak): rename PurgeAndSuspend with a better name, e.g.
    // RequestPurgeCache, because we don't suspend any renderers.
    render_process_host->PurgeAndSuspend();
  }
}

void TabManager::PauseBackgroundTabOpeningIfNeeded() {
  TRACE_EVENT_INSTANT0("navigation",
                       "TabManager::PauseBackgroundTabOpeningIfNeeded",
                       TRACE_EVENT_SCOPE_THREAD);
  if (IsInBackgroundTabOpeningSession()) {
    stats_collector_->TrackPausedBackgroundTabs(pending_navigations_.size());
    stats_collector_->OnBackgroundTabOpeningSessionEnded();
  }

  background_tab_loading_mode_ = BackgroundTabLoadingMode::kPaused;
}

void TabManager::ResumeBackgroundTabOpeningIfNeeded() {
  TRACE_EVENT_INSTANT0("navigation",
                       "TabManager::ResumeBackgroundTabOpeningIfNeeded",
                       TRACE_EVENT_SCOPE_THREAD);
  background_tab_loading_mode_ = BackgroundTabLoadingMode::kStaggered;
  LoadNextBackgroundTabIfNeeded();

  if (IsInBackgroundTabOpeningSession())
    stats_collector_->OnBackgroundTabOpeningSessionStarted();
}

void TabManager::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  // If Chrome is shutting down, do not do anything.
  if (g_browser_process->IsShuttingDown())
    return;

  // TODO(crbug.com/762775): Pause or resume background tab opening based on
  // memory pressure signal after it becomes more reliable.
  switch (memory_pressure_level) {
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE:
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE:
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL:
      LogMemoryAndDiscardTab(DiscardReason::kUrgent);
      break;
    default:
      NOTREACHED();
  }
  // TODO(skuhne): If more memory pressure levels are introduced, consider
  // calling PurgeBrowserMemory() before CRITICAL is reached.
}

void TabManager::ActiveTabChanged(content::WebContents* old_contents,
                                  content::WebContents* new_contents,
                                  int index,
                                  int reason) {
  // An active tab is not purged.
  // Calling GetWebContentsData() early ensures that the WebContentsData is
  // created for |new_contents|, which |stats_collector_| expects.
  GetWebContentsData(new_contents)->set_is_purged(false);

  // If |old_contents| is set, that tab has switched from being active to
  // inactive, so record the time of that transition.
  if (old_contents) {
    GetWebContentsData(old_contents)->SetLastInactiveTime(NowTicks());
    // Re-setting time-to-purge every time a tab becomes inactive.
    GetWebContentsData(old_contents)
        ->set_time_to_purge(
            GetTimeToPurge(min_time_to_purge_, max_time_to_purge_));
    // Only record switch-to-tab metrics when a switch happens, i.e.
    // |old_contents| is set.
    stats_collector_->RecordSwitchToTab(old_contents, new_contents);
  }

  ResumeTabNavigationIfNeeded(new_contents);
}

void TabManager::TabInsertedAt(TabStripModel* tab_strip_model,
                               content::WebContents* contents,
                               int index,
                               bool foreground) {
  // Only interested in background tabs, as foreground tabs get taken care of by
  // ActiveTabChanged.
  if (foreground)
    return;

  // A new background tab is similar to having a tab switch from being active to
  // inactive.
  GetWebContentsData(contents)->SetLastInactiveTime(NowTicks());
  // Re-setting time-to-purge every time a tab becomes inactive.
  GetWebContentsData(contents)->set_time_to_purge(
      GetTimeToPurge(min_time_to_purge_, max_time_to_purge_));
}

void TabManager::TabReplacedAt(TabStripModel* tab_strip_model,
                               content::WebContents* old_contents,
                               content::WebContents* new_contents,
                               int index) {
  WebContentsData::CopyState(old_contents, new_contents);
}

// static
TabManager::WebContentsData* TabManager::GetWebContentsData(
    content::WebContents* contents) {
  WebContentsData::CreateForWebContents(contents);
  return WebContentsData::FromWebContents(contents);
}

// TODO(jamescook): This should consider tabs with references to other tabs,
// such as tabs created with JavaScript window.open(). Potentially consider
// discarding the entire set together, or use that in the priority computation.
content::WebContents* TabManager::DiscardTabImpl(DiscardReason reason) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  for (LifecycleUnit* lifecycle_unit : GetSortedLifecycleUnits()) {
    if (lifecycle_unit->CanDiscard(reason) && lifecycle_unit->Discard(reason)) {
      TabLifecycleUnitExternal* tab_lifecycle_unit_external =
          lifecycle_unit->AsTabLifecycleUnitExternal();
      // For now, all LifecycleUnits are TabLifecycleUnitExternals.
      DCHECK(tab_lifecycle_unit_external);

      return tab_lifecycle_unit_external->GetWebContents();
    }
  }

  return nullptr;
}

void TabManager::OnSessionRestoreStartedLoadingTabs() {
  DCHECK(!is_session_restore_loading_tabs_);
  is_session_restore_loading_tabs_ = true;
}

void TabManager::OnSessionRestoreFinishedLoadingTabs() {
  DCHECK(is_session_restore_loading_tabs_);
  is_session_restore_loading_tabs_ = false;
  restored_tab_count_ = 0u;
}

void TabManager::OnWillRestoreTab(WebContents* contents) {
  WebContentsData* data = GetWebContentsData(contents);
  DCHECK(!data->is_in_session_restore());
  data->SetIsInSessionRestore(true);
  data->SetIsRestoredInForeground(contents->GetVisibility() !=
                                  content::Visibility::HIDDEN);
  restored_tab_count_++;

  // TabUIHelper is initialized in TabHelpers::AttachTabHelpers. But this place
  // gets called earlier than that. So for restored tabs, also initialize their
  // TabUIHelper here.
  TabUIHelper::CreateForWebContents(contents);
  TabUIHelper::FromWebContents(contents)->set_created_by_session_restore(true);
}

content::NavigationThrottle::ThrottleCheckResult
TabManager::MaybeThrottleNavigation(BackgroundTabNavigationThrottle* throttle) {
  content::WebContents* contents =
      throttle->navigation_handle()->GetWebContents();
  DCHECK_EQ(contents->GetVisibility(), content::Visibility::HIDDEN);

  // Skip delaying the navigation if this tab is in session restore, whose
  // loading is already controlled by TabLoader.
  if (GetWebContentsData(contents)->is_in_session_restore())
    return content::NavigationThrottle::PROCEED;

  if (background_tab_loading_mode_ == BackgroundTabLoadingMode::kStaggered &&
      !IsInBackgroundTabOpeningSession()) {
    stats_collector_->OnBackgroundTabOpeningSessionStarted();
  }

  stats_collector_->TrackNewBackgroundTab(pending_navigations_.size(),
                                          loading_contents_.size());

  if (!base::FeatureList::IsEnabled(
          features::kStaggeredBackgroundTabOpeningExperiment) ||
      CanLoadNextTab()) {
    loading_contents_.insert(contents);
    stats_collector_->TrackBackgroundTabLoadAutoStarted();
    return content::NavigationThrottle::PROCEED;
  }

  // Notify TabUIHelper that the navigation is delayed, so that the tab UI such
  // as favicon and title can be updated accordingly.
  TabUIHelper::FromWebContents(contents)->NotifyInitialNavigationDelayed(true);

  GetWebContentsData(contents)->SetTabLoadingState(TAB_IS_NOT_LOADING);
  pending_navigations_.push_back(throttle);
  std::stable_sort(pending_navigations_.begin(), pending_navigations_.end(),
                   ComparePendingNavigations);

  TRACE_EVENT_INSTANT1(
      "navigation", "TabManager::MaybeThrottleNavigation",
      TRACE_EVENT_SCOPE_THREAD, "data",
      DataAsTraceValue(background_tab_loading_mode_,
                       pending_navigations_.size(), loading_contents_.size()));

  StartForceLoadTimer();
  return content::NavigationThrottle::DEFER;
}

bool TabManager::IsInBackgroundTabOpeningSession() const {
  if (background_tab_loading_mode_ != BackgroundTabLoadingMode::kStaggered)
    return false;

  return !(pending_navigations_.empty() && loading_contents_.empty());
}

bool TabManager::CanLoadNextTab() const {
  if (background_tab_loading_mode_ != BackgroundTabLoadingMode::kStaggered)
    return false;

  // TabManager can only load the next tab when the loading slots free up. The
  // loading slot limit can be exceeded when |force_load_timer_| fires or when
  // the user selects a background tab.
  if (loading_contents_.size() < loading_slots_)
    return true;

  return false;
}

void TabManager::OnDidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  auto it = pending_navigations_.begin();
  while (it != pending_navigations_.end()) {
    BackgroundTabNavigationThrottle* throttle = *it;
    if (throttle->navigation_handle() == navigation_handle) {
      TRACE_EVENT_INSTANT1("navigation", "TabManager::OnDidFinishNavigation",
                           TRACE_EVENT_SCOPE_THREAD,
                           "found_navigation_handle_to_remove", true);
      pending_navigations_.erase(it);
      break;
    }
    it++;
  }
}

void TabManager::OnTabIsLoaded(content::WebContents* contents) {
  DCHECK_EQ(TAB_IS_LOADED, GetWebContentsData(contents)->tab_loading_state());
  bool was_in_background_tab_opening_session =
      IsInBackgroundTabOpeningSession();

  loading_contents_.erase(contents);
  stats_collector_->OnTabIsLoaded(contents);
  LoadNextBackgroundTabIfNeeded();

  if (was_in_background_tab_opening_session &&
      !IsInBackgroundTabOpeningSession()) {
    stats_collector_->OnBackgroundTabOpeningSessionEnded();
  }
}

void TabManager::OnWebContentsDestroyed(content::WebContents* contents) {
  bool was_in_background_tab_opening_session =
      IsInBackgroundTabOpeningSession();

  RemovePendingNavigationIfNeeded(contents);
  loading_contents_.erase(contents);
  stats_collector_->OnWebContentsDestroyed(contents);
  LoadNextBackgroundTabIfNeeded();

  if (was_in_background_tab_opening_session &&
      !IsInBackgroundTabOpeningSession()) {
    stats_collector_->OnBackgroundTabOpeningSessionEnded();
  }
}

void TabManager::StartForceLoadTimer() {
  TRACE_EVENT_INSTANT1(
      "navigation", "TabManager::StartForceLoadTimer", TRACE_EVENT_SCOPE_THREAD,
      "data",
      DataAsTraceValue(background_tab_loading_mode_,
                       pending_navigations_.size(), loading_contents_.size()));

  if (force_load_timer_)
    force_load_timer_->Stop();
  else
    force_load_timer_ = std::make_unique<base::OneShotTimer>(GetTickClock());

  force_load_timer_->Start(FROM_HERE,
                           GetTabLoadTimeout(kDefaultBackgroundTabLoadTimeout),
                           this, &TabManager::LoadNextBackgroundTabIfNeeded);
}

void TabManager::LoadNextBackgroundTabIfNeeded() {
  TRACE_EVENT_INSTANT2(
      "navigation", "TabManager::LoadNextBackgroundTabIfNeeded",
      TRACE_EVENT_SCOPE_THREAD, "is_force_load_timer_running",
      IsForceLoadTimerRunning(), "data",
      DataAsTraceValue(background_tab_loading_mode_,
                       pending_navigations_.size(), loading_contents_.size()));

  if (background_tab_loading_mode_ != BackgroundTabLoadingMode::kStaggered)
    return;

  // Do not load more background tabs until TabManager can load the next tab.
  // Ignore this constraint if the timer fires to force loading the next
  // background tab.
  if (IsForceLoadTimerRunning() && !CanLoadNextTab())
    return;

  if (pending_navigations_.empty())
    return;

  stats_collector_->OnWillLoadNextBackgroundTab(!IsForceLoadTimerRunning());
  BackgroundTabNavigationThrottle* throttle = pending_navigations_.front();
  pending_navigations_.erase(pending_navigations_.begin());
  ResumeNavigation(throttle);
  stats_collector_->TrackBackgroundTabLoadAutoStarted();

  StartForceLoadTimer();
}

void TabManager::ResumeTabNavigationIfNeeded(content::WebContents* contents) {
  BackgroundTabNavigationThrottle* throttle =
      RemovePendingNavigationIfNeeded(contents);
  if (throttle) {
    ResumeNavigation(throttle);
    stats_collector_->TrackBackgroundTabLoadUserInitiated();
  }
}

void TabManager::ResumeNavigation(BackgroundTabNavigationThrottle* throttle) {
  content::WebContents* contents =
      throttle->navigation_handle()->GetWebContents();
  GetWebContentsData(contents)->SetTabLoadingState(TAB_IS_LOADING);
  loading_contents_.insert(contents);
  TabUIHelper::FromWebContents(contents)->NotifyInitialNavigationDelayed(false);

  throttle->ResumeNavigation();
}

BackgroundTabNavigationThrottle* TabManager::RemovePendingNavigationIfNeeded(
    content::WebContents* contents) {
  auto it = pending_navigations_.begin();
  while (it != pending_navigations_.end()) {
    BackgroundTabNavigationThrottle* throttle = *it;
    if (throttle->navigation_handle()->GetWebContents() == contents) {
      pending_navigations_.erase(it);
      return throttle;
    }
    it++;
  }
  return nullptr;
}

// static
bool TabManager::ComparePendingNavigations(
    const BackgroundTabNavigationThrottle* first,
    const BackgroundTabNavigationThrottle* second) {
  bool first_is_internal_page =
      IsInternalPage(first->navigation_handle()->GetURL());
  bool second_is_internal_page =
      IsInternalPage(second->navigation_handle()->GetURL());

  if (first_is_internal_page != second_is_internal_page)
    return !first_is_internal_page;

  return false;
}

int TabManager::GetNumAliveTabs() const {
  int tab_count = 0;
  for (auto* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip_model = browser->tab_strip_model();
    for (int index = 0; index < tab_strip_model->count(); ++index) {
      content::WebContents* contents = tab_strip_model->GetWebContentsAt(index);
      if (!TabLifecycleUnitExternal::FromWebContents(contents)->IsDiscarded())
        ++tab_count;
    }
  }

  tab_count -= pending_navigations_.size();
  DCHECK_GE(tab_count, 0);

  return tab_count;
}

bool TabManager::IsTabLoadingForTest(content::WebContents* contents) const {
  if (loading_contents_.count(contents) == 1) {
    DCHECK_EQ(TAB_IS_LOADING,
              GetWebContentsData(contents)->tab_loading_state());
    return true;
  }

  DCHECK_NE(TAB_IS_LOADING, GetWebContentsData(contents)->tab_loading_state());
  return false;
}

bool TabManager::IsNavigationDelayedForTest(
    const content::NavigationHandle* navigation_handle) const {
  for (const auto* it : pending_navigations_) {
    if (it->navigation_handle() == navigation_handle)
      return true;
  }
  return false;
}

bool TabManager::IsForceLoadTimerRunning() const {
  return force_load_timer_ && force_load_timer_->IsRunning();
}

base::TimeDelta TabManager::GetTimeInBackgroundBeforeProactiveDiscard() const {
  // Exceed high threshold - in excessive state.
  if (num_loaded_lifecycle_units_ >=
      proactive_discard_params_.high_loaded_tab_count) {
    return base::TimeDelta();
  }

  // Exceed moderate threshold - in high state.
  if (num_loaded_lifecycle_units_ >=
      proactive_discard_params_.moderate_loaded_tab_count) {
    return proactive_discard_params_.high_occluded_timeout;
  }

  // Exceed low threshold - in moderate state.
  if (num_loaded_lifecycle_units_ >=
      proactive_discard_params_.low_loaded_tab_count) {
    return proactive_discard_params_.moderate_occluded_timeout;
  }

  // Didn't meet any thresholds - in low state.
  return proactive_discard_params_.low_occluded_timeout;
}

LifecycleUnit* TabManager::GetNextDiscardableLifecycleUnit() const {
  LifecycleUnit* next_to_discard = nullptr;
  base::TimeTicks earliest_backgrounded_time = base::TimeTicks::Max();

  // TODO(fdoray), TODO(varunmohan) : switch this to use
  // GetSortedLifecycleUnits(). Eventually, there will be some more advanced
  // logic around LifecycleUnit importance (perhaps ML LifecycleUnit ranking)
  // which will be used to determine which LifecycleUnit to discard. For now,
  // choosing which LifecycleUnit to discard based on the longest duration a
  // LifecycleUnit was backgrounded.
  for (LifecycleUnit* lifecycle_unit : lifecycle_units_) {
    // LifecycleUnits that aren't hidden cannot be discarded at this time.
    if (lifecycle_unit->GetVisibility() != content::Visibility::HIDDEN)
      continue;

    // Ignore LifecycleUnits that cannot be discarded.
    if (!lifecycle_unit->CanDiscard(DiscardReason::kProactive))
      continue;

    base::TimeTicks time_backgrounded =
        lifecycle_unit->GetLastVisibilityChangeTime();

    // This LifecycleUnit was backgrounded earlier than the current earliest
    // LifecycleUnit, so it will be discarded earlier.
    if (time_backgrounded < earliest_backgrounded_time) {
      earliest_backgrounded_time = time_backgrounded;
      next_to_discard = lifecycle_unit;
    }
  }

  return next_to_discard;
}

void TabManager::UpdateProactiveDiscardTimerIfNecessary() {
  // If the proactive discarding feature is not enabled, do nothing.
  if (!base::FeatureList::IsEnabled(features::kProactiveTabDiscarding))
    return;

  // Create or stop |proactive_discard_timer_|.
  if (!proactive_discard_timer_) {
    proactive_discard_timer_ =
        std::make_unique<base::OneShotTimer>(GetTickClock());
  } else {
    proactive_discard_timer_->Stop();
  }

  // No loaded LifecycleUnits. In Low threshold, but no timer is necessary.
  if (num_loaded_lifecycle_units_ == 0)
    return;

  base::TimeDelta time_until_discard =
      GetTimeInBackgroundBeforeProactiveDiscard();

  LifecycleUnit* next_to_discard = GetNextDiscardableLifecycleUnit();

  // There were no discardable LifecycleUnits. No timer is necessary.
  if (!next_to_discard)
    return;

  // Get the time |next_to_discard| was backgrounded and subtract it from
  // |time_until_discard| to get the exact time until the next LifecycleUnit
  // should be discarded.
  base::TimeDelta time_backgrounded =
      NowTicks() - next_to_discard->GetLastVisibilityChangeTime();
  time_until_discard -= time_backgrounded;

  // Ensure |time_until_discard| is a non-negative base::TimeDelta.
  time_until_discard = std::max(base::TimeDelta(), time_until_discard);

  // On a |time_until_discard| long timer, discard |next_to_discard|. If for
  // some reason |next_to_discard| is destroyed while this timer is waiting,
  // OnLifecycleUnitDestroyed() will be called and the timer will be reset. When
  // a tab is discarded, OnLifecycleUnitStateChanged() will be called, which
  // will set up the timer for the next tab to be discarded.
  proactive_discard_timer_->Start(
      FROM_HERE, time_until_discard,
      base::BindRepeating(
          [](LifecycleUnit* lifecycle_unit, TabManager* tab_manager) {
            // If |lifecycle_unit| can still be discarded, discard it.
            // Otherwise, find a new tab to be discarded and reset
            // |proactive_discard_timer|.
            if (lifecycle_unit->CanDiscard(DiscardReason::kProactive))
              lifecycle_unit->Discard(DiscardReason::kProactive);
            else
              tab_manager->UpdateProactiveDiscardTimerIfNecessary();
          },
          next_to_discard, this));
}

void TabManager::OnLifecycleUnitStateChanged(LifecycleUnit* lifecycle_unit,
                                             LifecycleState last_state) {
  if (lifecycle_unit->GetState() == LifecycleState::DISCARDED)
    num_loaded_lifecycle_units_--;
  else if (last_state == LifecycleState::DISCARDED)
    num_loaded_lifecycle_units_++;

  DCHECK_EQ(num_loaded_lifecycle_units_,
            GetNumLoadedLifecycleUnits(lifecycle_units_));

  UpdateProactiveDiscardTimerIfNecessary();
}

void TabManager::OnLifecycleUnitVisibilityChanged(
    LifecycleUnit* lifecycle_unit,
    content::Visibility visibility) {
  UpdateProactiveDiscardTimerIfNecessary();
}

void TabManager::OnLifecycleUnitDestroyed(LifecycleUnit* lifecycle_unit) {
  if (lifecycle_unit->GetState() != LifecycleState::DISCARDED)
    num_loaded_lifecycle_units_--;
  lifecycle_units_.erase(lifecycle_unit);

  DCHECK_EQ(num_loaded_lifecycle_units_,
            GetNumLoadedLifecycleUnits(lifecycle_units_));

  UpdateProactiveDiscardTimerIfNecessary();
}

void TabManager::OnLifecycleUnitCreated(LifecycleUnit* lifecycle_unit) {
  lifecycle_units_.insert(lifecycle_unit);
  if (lifecycle_unit->GetState() != LifecycleState::DISCARDED)
    num_loaded_lifecycle_units_++;

  // Add an observer to be notified of destruction.
  lifecycle_unit->AddObserver(this);

  DCHECK_EQ(num_loaded_lifecycle_units_,
            GetNumLoadedLifecycleUnits(lifecycle_units_));

  UpdateProactiveDiscardTimerIfNecessary();
}

}  // namespace resource_coordinator
