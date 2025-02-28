// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/history/core/browser/expire_history_backend.h"

#include <stddef.h>

#include <algorithm>
#include <functional>
#include <limits>

#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/containers/flat_set.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/sequenced_task_runner.h"
#include "build/build_config.h"
#include "components/history/core/browser/history_backend_client.h"
#include "components/history/core/browser/history_backend_notifier.h"
#include "components/history/core/browser/history_database.h"
#include "components/history/core/browser/thumbnail_database.h"

namespace history {

// Helpers --------------------------------------------------------------------

namespace {

// The number of days by which the expiration threshold is advanced for items
// that we want to expire early, such as those of AUTO_SUBFRAME transition type.
//
// Early expiration stuff is kept around only for edge cases, as subframes
// don't appear in history and the vast majority of them are ads anyway. The
// main use case for these is if you're on a site with links to different
// frames, you'll be able to see those links as visited, and we'll also be
// able to get redirect information for those URLs.
//
// But since these uses are most valuable when you're actually on the site,
// and because these can take up the bulk of your history, we get a lot of
// space savings by deleting them quickly.
const int kEarlyExpirationAdvanceDays = 3;

// Reads all types of visits starting from beginning of time to the given end
// time. This is the most general reader.
class AllVisitsReader : public ExpiringVisitsReader {
 public:
  bool Read(base::Time end_time,
            HistoryDatabase* db,
            VisitVector* visits,
            int max_visits) const override {
    DCHECK(db) << "must have a database to operate upon";
    DCHECK(visits) << "visit vector has to exist in order to populate it";

    db->GetAllVisitsInRange(base::Time(), end_time, max_visits, visits);
    // When we got the maximum number of visits we asked for, we say there could
    // be additional things to expire now.
    return static_cast<int>(visits->size()) == max_visits;
  }
};

// Reads only AUTO_SUBFRAME visits, within a computed range. The range is
// computed as follows:
// * |begin_time| is read from the meta table. This value is updated whenever
//   there are no more additional visits to expire by this reader.
// * |end_time| is advanced forward by a constant (kEarlyExpirationAdvanceDay),
//   but not past the current time.
class AutoSubframeVisitsReader : public ExpiringVisitsReader {
 public:
  bool Read(base::Time end_time,
            HistoryDatabase* db,
            VisitVector* visits,
            int max_visits) const override {
    DCHECK(db) << "must have a database to operate upon";
    DCHECK(visits) << "visit vector has to exist in order to populate it";

    base::Time begin_time = db->GetEarlyExpirationThreshold();
    // Advance |end_time| to expire early.
    base::Time early_end_time = end_time +
        base::TimeDelta::FromDays(kEarlyExpirationAdvanceDays);

    // We don't want to set the early expiration threshold to a time in the
    // future.
    base::Time now = base::Time::Now();
    if (early_end_time > now)
      early_end_time = now;

    db->GetVisitsInRangeForTransition(begin_time, early_end_time,
                                      max_visits,
                                      ui::PAGE_TRANSITION_AUTO_SUBFRAME,
                                      visits);
    bool more = static_cast<int>(visits->size()) == max_visits;
    if (!more)
      db->UpdateEarlyExpirationThreshold(early_end_time);

    return more;
  }
};

// The number of visits we will expire very time we check for old items. This
// Prevents us from doing too much work any given time.
const int kNumExpirePerIteration = 32;

// The number of seconds between checking for items that should be expired when
// we think there might be more items to expire. This timeout is used when the
// last expiration found at least kNumExpirePerIteration and we want to check
// again "soon."
const int kExpirationDelaySec = 30;

// The number of minutes between checking, as with kExpirationDelaySec, but
// when we didn't find enough things to expire last time. If there was no
// history to expire last iteration, it's likely there is nothing next
// iteration, so we want to wait longer before checking to avoid wasting CPU.
const int kExpirationEmptyDelayMin = 5;

// If the expiration timer is delayed by over an hour, then assume that the
// machine went to sleep.
constexpr base::TimeDelta kExpirationSleepWakeupThreshold =
    base::TimeDelta::FromHours(1);

// The minimum number of hours between checking for old on-demand favicons that
// should be cleared.
const int kClearOnDemandFaviconsIntervalHours = 24;

bool IsAnyURLBookmarked(HistoryBackendClient* backend_client,
                        const std::vector<GURL>& urls) {
  for (const GURL& url : urls) {
    if (backend_client->IsBookmarked(url))
      return true;
  }
  return false;
}

}  // namespace

namespace internal {

// Clearing old on-demand favicons is only enabled on mobile.
#if defined(OS_ANDROID) || defined(OS_IOS)
constexpr bool kClearOldOnDemandFaviconsEnabled = true;
#else
constexpr bool kClearOldOnDemandFaviconsEnabled = false;
#endif

const int kOnDemandFaviconIsOldAfterDays = 30;

}  // namespace internal

// ExpireHistoryBackend::DeleteEffects ----------------------------------------

ExpireHistoryBackend::DeleteEffects::DeleteEffects() {
}

ExpireHistoryBackend::DeleteEffects::~DeleteEffects() {
}


// ExpireHistoryBackend -------------------------------------------------------

ExpireHistoryBackend::ExpireHistoryBackend(
    HistoryBackendNotifier* notifier,
    HistoryBackendClient* backend_client,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : notifier_(notifier),
      main_db_(nullptr),
      thumb_db_(nullptr),
      backend_client_(backend_client),
      task_runner_(task_runner),
      weak_factory_(this) {
  DCHECK(notifier_);
}

ExpireHistoryBackend::~ExpireHistoryBackend() {
}

void ExpireHistoryBackend::SetDatabases(HistoryDatabase* main_db,
                                        ThumbnailDatabase* thumb_db) {
  main_db_ = main_db;
  thumb_db_ = thumb_db;
}

void ExpireHistoryBackend::DeleteURL(const GURL& url) {
  DeleteURLs(std::vector<GURL>(1, url));
}

void ExpireHistoryBackend::DeleteURLs(const std::vector<GURL>& urls) {
  if (!main_db_)
    return;

  DeleteEffects effects;
  for (std::vector<GURL>::const_iterator url = urls.begin(); url != urls.end();
       ++url) {
    const bool is_bookmarked =
        backend_client_ && backend_client_->IsBookmarked(*url);
    URLRow url_row;
    if (!main_db_->GetRowForURL(*url, &url_row) && !is_bookmarked) {
      // If the URL isn't in the database and not bookmarked, we should still
      // check to see if any favicons need to be deleted.
      DeleteIcons(*url, &effects);
      continue;
    }

    // Collect all the visits and delete them. Note that we don't give up if
    // there are no visits, since the URL could still have an entry that we
    // should delete.
    VisitVector visits;
    main_db_->GetVisitsForURL(url_row.id(), &visits);

    DeleteVisitRelatedInfo(visits, &effects);

    // We skip ExpireURLsForVisits (since we are deleting from the
    // URL, and not starting with visits in a given time range). We
    // therefore need to call the deletion and favicon update
    // functions manually.
    DeleteOneURL(url_row, is_bookmarked, &effects);
  }

  DeleteFaviconsIfPossible(&effects);

  BroadcastNotifications(&effects, DELETION_USER_INITIATED,
                         DeletionTimeRange::Invalid(), base::nullopt);
}

void ExpireHistoryBackend::ExpireHistoryBetween(
    const std::set<GURL>& restrict_urls,
    base::Time begin_time,
    base::Time end_time) {
  if (!main_db_)
    return;

  // Find the affected visits and delete them.
  VisitVector visits;
  main_db_->GetAllVisitsInRange(begin_time, end_time, 0, &visits);
  if (!restrict_urls.empty()) {
    std::set<URLID> url_ids;
    for (std::set<GURL>::const_iterator url = restrict_urls.begin();
         url != restrict_urls.end(); ++url)
      url_ids.insert(main_db_->GetRowForURL(*url, nullptr));
    VisitVector all_visits;
    all_visits.swap(visits);
    for (VisitVector::iterator visit = all_visits.begin();
         visit != all_visits.end(); ++visit) {
      if (url_ids.find(visit->url_id) != url_ids.end())
        visits.push_back(*visit);
    }
  }
  DeletionTimeRange time_range(begin_time, end_time);
  ExpireVisitsInternal(visits, time_range, restrict_urls);
}

void ExpireHistoryBackend::ExpireHistoryForTimes(
    const std::vector<base::Time>& times) {
  // |times| must be in reverse chronological order and have no
  // duplicates, i.e. each member must be earlier than the one before
  // it.
  DCHECK(
      std::adjacent_find(
          times.begin(), times.end(), std::less_equal<base::Time>()) ==
      times.end());

  if (!main_db_)
    return;

  // Find the affected visits and delete them.
  VisitVector visits;
  main_db_->GetVisitsForTimes(times, &visits);
  ExpireVisits(visits);
}

void ExpireHistoryBackend::ExpireVisits(const VisitVector& visits) {
  ExpireVisitsInternal(visits, DeletionTimeRange::Invalid(), {});
}

void ExpireHistoryBackend::ExpireVisitsInternal(
    const VisitVector& visits,
    const DeletionTimeRange& time_range,
    const std::set<GURL>& restrict_urls) {
  if (visits.empty())
    return;

  base::TimeTicks start = base::TimeTicks::Now();

  const VisitVector visits_and_redirects = GetVisitsAndRedirectParents(visits);
  base::TimeDelta get_redirects_time = base::TimeTicks::Now() - start;

  DeleteEffects effects;
  DeleteVisitRelatedInfo(visits_and_redirects, &effects);

  // Delete or update the URLs affected. We want to update the visit counts
  // since this is called by the user who wants to delete their recent history,
  // and we don't want to leave any evidence.
  ExpireURLsForVisits(visits_and_redirects, &effects);
  DeleteFaviconsIfPossible(&effects);
  BroadcastNotifications(
      &effects, DELETION_USER_INITIATED, time_range,
      restrict_urls.empty() ? base::Optional<std::set<GURL>>() : restrict_urls);

  // Pick up any bits possibly left over.
  ParanoidExpireHistory();

  base::TimeDelta expire_visits_time = base::TimeTicks::Now() - start;
  UMA_HISTOGRAM_TIMES("History.ExpireVisits.TotalDuration", expire_visits_time);
  if (!expire_visits_time.is_zero()) {
    UMA_HISTOGRAM_PERCENTAGE(
        "History.ExpireVisits.GetRedirectsDurationPercentage",
        get_redirects_time * 100 / expire_visits_time);
  }
}

void ExpireHistoryBackend::ExpireHistoryBefore(base::Time end_time) {
  if (!main_db_)
    return;

  // Expire as much history as possible before the given date.
  ExpireSomeOldHistory(end_time, GetAllVisitsReader(),
                       std::numeric_limits<int>::max());
  ParanoidExpireHistory();
}

void ExpireHistoryBackend::InitWorkQueue() {
  DCHECK(work_queue_.empty()) << "queue has to be empty prior to init";

  for (size_t i = 0; i < readers_.size(); i++)
    work_queue_.push(readers_[i]);
}

const ExpiringVisitsReader* ExpireHistoryBackend::GetAllVisitsReader() {
  if (!all_visits_reader_)
    all_visits_reader_.reset(new AllVisitsReader());
  return all_visits_reader_.get();
}

const ExpiringVisitsReader*
    ExpireHistoryBackend::GetAutoSubframeVisitsReader() {
  if (!auto_subframe_visits_reader_)
    auto_subframe_visits_reader_.reset(new AutoSubframeVisitsReader());
  return auto_subframe_visits_reader_.get();
}

void ExpireHistoryBackend::StartExpiringOldStuff(
    base::TimeDelta expiration_threshold) {
  expiration_threshold_ = expiration_threshold;

  // Remove all readers, just in case this was method was called before.
  readers_.clear();
  // For now, we explicitly add all known readers. If we come up with more
  // reader types (in case we want to expire different types of visits in
  // different ways), we can make it be populated by creator/owner of
  // ExpireHistoryBackend.
  readers_.push_back(GetAllVisitsReader());
  readers_.push_back(GetAutoSubframeVisitsReader());

  // Initialize the queue with all tasks for the first set of iterations.
  InitWorkQueue();
  ScheduleExpire();
}

void ExpireHistoryBackend::DeleteFaviconsIfPossible(DeleteEffects* effects) {
  if (!thumb_db_)
    return;

  for (std::set<favicon_base::FaviconID>::const_iterator i =
           effects->affected_favicons.begin();
       i != effects->affected_favicons.end(); ++i) {
    if (!thumb_db_->HasMappingFor(*i)) {
      GURL icon_url;
      favicon_base::IconType icon_type;
      if (thumb_db_->GetFaviconHeader(*i,
                                      &icon_url,
                                      &icon_type) &&
          thumb_db_->DeleteFavicon(*i)) {
        effects->deleted_favicons.insert(icon_url);
      }
    }
  }
}

void ExpireHistoryBackend::BroadcastNotifications(
    DeleteEffects* effects,
    DeletionType type,
    const DeletionTimeRange& time_range,
    base::Optional<std::set<GURL>> restrict_urls) {
  if (!effects->modified_urls.empty()) {
    notifier_->NotifyURLsModified(effects->modified_urls);
  }
  if (!effects->deleted_urls.empty() || time_range.IsValid()) {
    notifier_->NotifyURLsDeleted(DeletionInfo(
        time_range, type == DELETION_EXPIRED, std::move(effects->deleted_urls),
        std::move(effects->deleted_favicons), std::move(restrict_urls)));
  }
}

VisitVector ExpireHistoryBackend::GetVisitsAndRedirectParents(
    const VisitVector& visits) {
  base::flat_set<VisitID> seen_visits;
  VisitVector visits_and_redirects;
  for (const auto v : visits) {
    VisitRow current_visit = v;
    do {
      if (!seen_visits.insert(current_visit.visit_id).second)
        break;

      visits_and_redirects.push_back(current_visit);
    } while (current_visit.referring_visit &&
             main_db_->GetRowForVisit(current_visit.referring_visit,
                                      &current_visit));
  }
  return visits_and_redirects;
}

void ExpireHistoryBackend::DeleteVisitRelatedInfo(const VisitVector& visits,
                                                  DeleteEffects* effects) {
  for (size_t i = 0; i < visits.size(); i++) {
    // Delete the visit itself.
    main_db_->DeleteVisit(visits[i]);

    // Add the URL row to the affected URL list.
    if (!effects->affected_urls.count(visits[i].url_id)) {
      URLRow row;
      if (main_db_->GetURLRow(visits[i].url_id, &row))
        effects->affected_urls[visits[i].url_id] = row;
    }
  }
}

void ExpireHistoryBackend::DeleteOneURL(const URLRow& url_row,
                                        bool is_bookmarked,
                                        DeleteEffects* effects) {
  main_db_->DeleteSegmentForURL(url_row.id());
  effects->deleted_urls.push_back(url_row);
  // If the URL is bookmarked we should still keep its favicon around to show
  // in bookmark-related UI.  We'll delete this icon if the URL is unbookmarked.
  // (See comments in DeleteURLs().)
  if (!is_bookmarked)
    DeleteIcons(url_row.url(), effects);
  main_db_->DeleteURLRow(url_row.id());
}

void ExpireHistoryBackend::DeleteIcons(const GURL& gurl,
                                       DeleteEffects* effects) {
  // Collect shared information.
  std::vector<IconMapping> icon_mappings;
  if (thumb_db_ && thumb_db_->GetIconMappingsForPageURL(gurl, &icon_mappings)) {
    for (std::vector<IconMapping>::iterator m = icon_mappings.begin();
         m != icon_mappings.end(); ++m) {
      effects->affected_favicons.insert(m->icon_id);
    }
    // Delete the mapping entries for the url.
    thumb_db_->DeleteIconMappings(gurl);
  }
}

namespace {

struct ChangedURL {
  ChangedURL() : visit_count(0), typed_count(0) {}
  int visit_count;
  int typed_count;
};

}  // namespace

void ExpireHistoryBackend::ExpireURLsForVisits(const VisitVector& visits,
                                               DeleteEffects* effects) {
  // First find all unique URLs and the number of visits we're deleting for
  // each one.
  std::map<URLID, ChangedURL> changed_urls;
  for (size_t i = 0; i < visits.size(); i++) {
    ChangedURL& cur = changed_urls[visits[i].url_id];
    // NOTE: This code must stay in sync with HistoryBackend::AddPageVisit().
    if (!ui::PageTransitionCoreTypeIs(visits[i].transition,
                                      ui::PAGE_TRANSITION_RELOAD)) {
      cur.visit_count++;
    }
    if (visits[i].incremented_omnibox_typed_score)
      cur.typed_count++;
  }

  // Check each unique URL with deleted visits.
  for (std::map<URLID, ChangedURL>::const_iterator i = changed_urls.begin();
       i != changed_urls.end(); ++i) {
    // The unique URL rows should already be filled in.
    URLRow& url_row = effects->affected_urls[i->first];
    if (!url_row.id())
      continue;  // URL row doesn't exist in the database.

    // Check if there are any other visits for this URL and update the time
    // (the time change may not actually be synced to disk below when we're
    // archiving).
    VisitRow last_visit;
    if (main_db_->GetMostRecentVisitForURL(url_row.id(), &last_visit))
      url_row.set_last_visit(last_visit.visit_time);
    else
      url_row.set_last_visit(base::Time());

    // Don't delete URLs with visits still in the DB, or bookmarked.
    bool is_bookmarked =
        (backend_client_ && backend_client_->IsBookmarked(url_row.url()));
    if (!is_bookmarked && url_row.last_visit().is_null()) {
      // Not bookmarked and no more visits. Nuke the url.
      DeleteOneURL(url_row, is_bookmarked, effects);
    } else {
      // NOTE: The calls to std::max() below are a backstop, but they should
      // never actually be needed unless the database is corrupt (I think).
      url_row.set_visit_count(
          std::max(0, url_row.visit_count() - i->second.visit_count));
      url_row.set_typed_count(
          std::max(0, url_row.typed_count() - i->second.typed_count));

      // Update the db with the new details.
      main_db_->UpdateURLRow(url_row.id(), url_row);

      effects->modified_urls.push_back(url_row);
    }
  }
}

void ExpireHistoryBackend::ScheduleExpire() {
  base::TimeDelta delay;
  if (work_queue_.empty()) {
    // If work queue is empty, reset the work queue to contain all tasks and
    // schedule next iteration after a longer delay.
    InitWorkQueue();
    delay = base::TimeDelta::FromMinutes(kExpirationEmptyDelayMin);
  } else {
    delay = base::TimeDelta::FromSeconds(kExpirationDelaySec);
  }

  expected_expiration_time_ = base::Time::Now() + delay;
  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::Bind(&ExpireHistoryBackend::DoExpireIteration,
                 weak_factory_.GetWeakPtr()),
      delay);
}

void ExpireHistoryBackend::DoExpireIteration() {
  DCHECK(!work_queue_.empty()) << "queue has to be non-empty";

  // If the timer is firing more than an hour later than expected, than the
  // machine likely just woke from sleep/hibernation. There is potentially a lot
  // of expiring that needs to happen. Wait for 5 minutes before starting to do
  // any expiry, to avoid conflicting with other work that happens on waking
  // from sleep.
  if (base::Time::Now() - expected_expiration_time_ >
      kExpirationSleepWakeupThreshold) {
    task_runner_->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ExpireHistoryBackend::ScheduleExpire,
                   weak_factory_.GetWeakPtr()),
        base::TimeDelta::FromMinutes(kExpirationEmptyDelayMin));
    return;
  }

  const ExpiringVisitsReader* reader = work_queue_.front();
  bool more_to_expire = ExpireSomeOldHistory(
      GetCurrentExpirationTime(), reader, kNumExpirePerIteration);

  work_queue_.pop();
  if (more_to_expire) {
    // If there are more items to expire, add the reader back to the queue, thus
    // creating a new task for future iterations.
    work_queue_.push(reader);
  } else if (internal::kClearOldOnDemandFaviconsEnabled) {
    // Otherwise do a final clean-up - remove old favicons not bound to visits.
    ClearOldOnDemandFaviconsIfPossible(
        base::Time::Now() -
        base::TimeDelta::FromDays(internal::kOnDemandFaviconIsOldAfterDays));
  }

  ScheduleExpire();
}

void ExpireHistoryBackend::ClearOldOnDemandFaviconsIfPossible(
    base::Time expiration_threshold) {
  if (!thumb_db_)
    return;

  // Extra precaution to avoid repeated calls to GetOldOnDemandFavicons() close
  // in time, since it can be fairly expensive.
  if (expiration_threshold <
      last_on_demand_expiration_threshold_ +
          base::TimeDelta::FromHours(kClearOnDemandFaviconsIntervalHours)) {
    return;
  }

  last_on_demand_expiration_threshold_ = expiration_threshold;

  std::map<favicon_base::FaviconID, IconMappingsForExpiry> icon_mappings =
      thumb_db_->GetOldOnDemandFavicons(expiration_threshold);
  DeleteEffects effects;

  for (auto id_and_mappings_pair : icon_mappings) {
    favicon_base::FaviconID icon_id = id_and_mappings_pair.first;
    const IconMappingsForExpiry& mappings = id_and_mappings_pair.second;

    if (backend_client_ &&
        IsAnyURLBookmarked(backend_client_, mappings.page_urls)) {
      continue;
    }

    thumb_db_->DeleteFavicon(icon_id);
    thumb_db_->DeleteIconMappingsForFaviconId(icon_id);
    effects.deleted_favicons.insert(mappings.icon_url);
  }

  BroadcastNotifications(&effects, DELETION_EXPIRED,
                         DeletionTimeRange::Invalid(), base::nullopt);
}

bool ExpireHistoryBackend::ExpireSomeOldHistory(
    base::Time end_time,
    const ExpiringVisitsReader* reader,
    int max_visits) {
  if (!main_db_)
    return false;

  // Add an extra time unit to given end time, because
  // GetAllVisitsInRange, et al. queries' end value is non-inclusive.
  base::Time effective_end_time =
      base::Time::FromInternalValue(end_time.ToInternalValue() + 1);

  VisitVector deleted_visits;
  bool more_to_expire = reader->Read(effective_end_time, main_db_,
                                     &deleted_visits, max_visits);

  DeleteEffects deleted_effects;
  DeleteVisitRelatedInfo(deleted_visits, &deleted_effects);
  ExpireURLsForVisits(deleted_visits, &deleted_effects);
  DeleteFaviconsIfPossible(&deleted_effects);

  BroadcastNotifications(&deleted_effects, DELETION_EXPIRED,
                         DeletionTimeRange::Invalid(), base::nullopt);

  return more_to_expire;
}

void ExpireHistoryBackend::ParanoidExpireHistory() {
  // TODO(brettw): Bug 1067331: write this to clean up any errors.
}

}  // namespace history
