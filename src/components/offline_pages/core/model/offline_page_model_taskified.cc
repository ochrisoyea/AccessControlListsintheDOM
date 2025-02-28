// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_pages/core/model/offline_page_model_taskified.h"

#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/file_path.h"
#include "base/location.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string16.h"
#include "base/task_scheduler/post_task.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/clock.h"
#include "base/time/default_clock.h"
#include "base/time/time.h"
#include "components/offline_pages/core/archive_manager.h"
#include "components/offline_pages/core/client_namespace_constants.h"
#include "components/offline_pages/core/model/add_page_task.h"
#include "components/offline_pages/core/model/cleanup_thumbnails_task.h"
#include "components/offline_pages/core/model/delete_page_task.h"
#include "components/offline_pages/core/model/get_pages_task.h"
#include "components/offline_pages/core/model/get_thumbnail_task.h"
#include "components/offline_pages/core/model/has_thumbnail_task.h"
#include "components/offline_pages/core/model/mark_page_accessed_task.h"
#include "components/offline_pages/core/model/offline_page_model_utils.h"
#include "components/offline_pages/core/model/persistent_page_consistency_check_task.h"
#include "components/offline_pages/core/model/startup_maintenance_task.h"
#include "components/offline_pages/core/model/store_thumbnail_task.h"
#include "components/offline_pages/core/model/update_file_path_task.h"
#include "components/offline_pages/core/offline_page_feature.h"
#include "components/offline_pages/core/offline_page_metadata_store_sql.h"
#include "components/offline_pages/core/offline_page_model.h"
#include "components/offline_pages/core/offline_store_utils.h"
#include "components/offline_pages/core/system_download_manager.h"
#include "url/gurl.h"

namespace offline_pages {

using ArchiverResult = OfflinePageArchiver::ArchiverResult;
using ClearStorageResult = ClearStorageTask::ClearStorageResult;

namespace {

void WrapInMultipleItemsCallback(const MultipleOfflineIdCallback& callback,
                                 const MultipleOfflinePageItemResult& pages) {
  std::vector<int64_t> results;
  for (const auto& page : pages)
    results.push_back(page.offline_id);
  callback.Run(results);
}

SavePageResult ArchiverResultToSavePageResult(ArchiverResult archiver_result) {
  switch (archiver_result) {
    case ArchiverResult::SUCCESSFULLY_CREATED:
      return SavePageResult::SUCCESS;
    case ArchiverResult::ERROR_DEVICE_FULL:
      return SavePageResult::DEVICE_FULL;
    case ArchiverResult::ERROR_CONTENT_UNAVAILABLE:
      return SavePageResult::CONTENT_UNAVAILABLE;
    case ArchiverResult::ERROR_ARCHIVE_CREATION_FAILED:
      return SavePageResult::ARCHIVE_CREATION_FAILED;
    case ArchiverResult::ERROR_CANCELED:
      return SavePageResult::CANCELLED;
    case ArchiverResult::ERROR_SECURITY_CERTIFICATE:
      return SavePageResult::SECURITY_CERTIFICATE_ERROR;
    case ArchiverResult::ERROR_ERROR_PAGE:
      return SavePageResult::ERROR_PAGE;
    case ArchiverResult::ERROR_INTERSTITIAL_PAGE:
      return SavePageResult::INTERSTITIAL_PAGE;
    case ArchiverResult::ERROR_SKIPPED:
      return SavePageResult::SKIPPED;
    case ArchiverResult::ERROR_DIGEST_CALCULATION_FAILED:
      return SavePageResult::DIGEST_CALCULATION_FAILED;
  }
  NOTREACHED();
  return SavePageResult::CONTENT_UNAVAILABLE;
}

SavePageResult AddPageResultToSavePageResult(AddPageResult add_page_result) {
  switch (add_page_result) {
    case AddPageResult::SUCCESS:
      return SavePageResult::SUCCESS;
    case AddPageResult::ALREADY_EXISTS:
      return SavePageResult::ALREADY_EXISTS;
    case AddPageResult::STORE_FAILURE:
      return SavePageResult::STORE_FAILURE;
    case AddPageResult::RESULT_COUNT:
      break;
  }
  NOTREACHED();
  return SavePageResult::STORE_FAILURE;
}

void ReportPageHistogramAfterSuccessfulSaving(
    const OfflinePageItem& offline_page,
    const base::Time& save_time) {
  base::UmaHistogramCustomTimes(
      model_utils::AddHistogramSuffix(offline_page.client_id.name_space,
                                      "OfflinePages.SavePageTime"),
      save_time - offline_page.creation_time,
      base::TimeDelta::FromMilliseconds(1), base::TimeDelta::FromSeconds(10),
      50);

  base::UmaHistogramCustomCounts(
      model_utils::AddHistogramSuffix(offline_page.client_id.name_space,
                                      "OfflinePages.PageSize"),
      offline_page.file_size / 1024, 1, 10000, 50);
}

void ReportSavedPagesCount(const MultipleOfflinePageItemCallback& callback,
                           const MultipleOfflinePageItemResult& all_items) {
  UMA_HISTOGRAM_COUNTS_10000("OfflinePages.SavedPageCountUponQuery",
                             all_items.size());
  callback.Run(all_items);
}

void ReportStorageUsage(const ArchiveManager::StorageStats& storage_stats) {
  const int kMiB = 1024 * 1024;
  int internal_free_disk_space_mib =
      static_cast<int>(storage_stats.internal_free_disk_space / kMiB);
  UMA_HISTOGRAM_CUSTOM_COUNTS("OfflinePages.StorageInfo.InternalFreeSpaceMiB",
                              internal_free_disk_space_mib, 1, 500000, 50);
  int external_free_disk_space_mib =
      static_cast<int>(storage_stats.external_free_disk_space / kMiB);
  UMA_HISTOGRAM_CUSTOM_COUNTS("OfflinePages.StorageInfo.ExternalFreeSpaceMiB",
                              external_free_disk_space_mib, 1, 500000, 50);
  int internal_page_size_mib =
      static_cast<int>(storage_stats.internal_archives_size() / kMiB);
  UMA_HISTOGRAM_COUNTS_10000("OfflinePages.StorageInfo.InternalArchiveSizeMiB",
                             internal_page_size_mib);
  int external_page_size_mib =
      static_cast<int>(storage_stats.public_archives_size / kMiB);
  UMA_HISTOGRAM_COUNTS_10000("OfflinePages.StorageInfo.ExternalArchiveSizeMiB",
                             external_page_size_mib);

  int64_t internal_volume_storage = storage_stats.internal_archives_size() +
                                    storage_stats.internal_free_disk_space;
  if (internal_volume_storage > 0) {
    int internal_percentage =
        static_cast<int>(100.0 * storage_stats.internal_archives_size() /
                         internal_volume_storage);
    UMA_HISTOGRAM_PERCENTAGE("OfflinePages.StorageInfo.InternalUsagePercentage",
                             internal_percentage);
  }

  int64_t external_volume_storage = storage_stats.public_archives_size +
                                    storage_stats.external_free_disk_space;
  if (external_volume_storage > 0) {
    int external_percentage = static_cast<int>(
        100.0 * storage_stats.public_archives_size / external_volume_storage);
    UMA_HISTOGRAM_PERCENTAGE("OfflinePages.StorageInfo.ExternalUsagePercentage",
                             external_percentage);
  }
}

void OnUpdateFilePathDone(PublishPageCallback publish_done_callback,
                          const base::FilePath& new_file_path,
                          SavePageResult result,
                          bool update_file_result) {
  if (update_file_result) {
    std::move(publish_done_callback).Run(new_file_path, result);
    return;
  }
  // If the file path wasn't updated successfully, just invoke the callback with
  // store failure.
  std::move(publish_done_callback)
      .Run(new_file_path, SavePageResult::STORE_FAILURE);
}

}  // namespace

// static
constexpr base::TimeDelta
    OfflinePageModelTaskified::kInitialUpgradeSelectionDelay;

// static
constexpr base::TimeDelta OfflinePageModelTaskified::kMaintenanceTasksDelay;

// static
constexpr base::TimeDelta OfflinePageModelTaskified::kClearStorageInterval;

OfflinePageModelTaskified::OfflinePageModelTaskified(
    std::unique_ptr<OfflinePageMetadataStoreSQL> store,
    std::unique_ptr<ArchiveManager> archive_manager,
    std::unique_ptr<SystemDownloadManager> download_manager,
    const scoped_refptr<base::SequencedTaskRunner>& task_runner,
    base::Clock* clock)
    : store_(std::move(store)),
      archive_manager_(std::move(archive_manager)),
      download_manager_(std::move(download_manager)),
      policy_controller_(new ClientPolicyController()),
      clock_(clock),
      task_queue_(this),
      skip_clearing_original_url_for_testing_(false),
      skip_maintenance_tasks_for_testing_(false),
      task_runner_(task_runner),
      weak_ptr_factory_(this) {
  DCHECK_LT(kMaintenanceTasksDelay, OfflinePageMetadataStoreSQL::kClosingDelay);
  CreateArchivesDirectoryIfNeeded();
  // TODO(fgorski): Call from here, when upgrade task is available:
  // PostSelectItemsMarkedForUpgrade();
}

OfflinePageModelTaskified::~OfflinePageModelTaskified() {}

void OfflinePageModelTaskified::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void OfflinePageModelTaskified::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void OfflinePageModelTaskified::OnTaskQueueIsIdle() {}

void OfflinePageModelTaskified::SavePage(
    const SavePageParams& save_page_params,
    std::unique_ptr<OfflinePageArchiver> archiver,
    content::WebContents* web_contents,
    const SavePageCallback& callback) {
  // Skip saving the page that is not intended to be saved, like local file
  // page.
  if (!OfflinePageModel::CanSaveURL(save_page_params.url)) {
    InformSavePageDone(callback, SavePageResult::SKIPPED,
                       save_page_params.client_id, kInvalidOfflineId);
    return;
  }

  // The web contents is not available if archiver is not created and passed.
  if (!archiver) {
    InformSavePageDone(callback, SavePageResult::CONTENT_UNAVAILABLE,
                       save_page_params.client_id, kInvalidOfflineId);
    return;
  }

  // If we already have an offline id, use it.  If not, generate one.
  int64_t offline_id = save_page_params.proposed_offline_id;
  if (offline_id == kInvalidOfflineId)
    offline_id = store_utils::GenerateOfflineId();

  OfflinePageArchiver::CreateArchiveParams create_archive_params;
  // If the page is being saved in the background, we should try to remove the
  // popup overlay that obstructs viewing the normal content.
  create_archive_params.remove_popup_overlay = save_page_params.is_background;
  create_archive_params.use_page_problem_detectors =
      save_page_params.use_page_problem_detectors;
  archiver->CreateArchive(
      GetInternalArchiveDirectory(save_page_params.client_id.name_space),
      create_archive_params, web_contents,
      base::Bind(&OfflinePageModelTaskified::OnCreateArchiveDone,
                 weak_ptr_factory_.GetWeakPtr(), save_page_params, offline_id,
                 GetCurrentTime(), callback));
  pending_archivers_.push_back(std::move(archiver));
}

void OfflinePageModelTaskified::AddPage(const OfflinePageItem& page,
                                        const AddPageCallback& callback) {
  auto task = std::make_unique<AddPageTask>(
      store_.get(), page,
      base::BindOnce(&OfflinePageModelTaskified::OnAddPageDone,
                     weak_ptr_factory_.GetWeakPtr(), page, callback));
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::MarkPageAccessed(int64_t offline_id) {
  auto task = std::make_unique<MarkPageAccessedTask>(store_.get(), offline_id,
                                                     GetCurrentTime());
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::DeletePagesByOfflineId(
    const std::vector<int64_t>& offline_ids,
    const DeletePageCallback& callback) {
  auto task = DeletePageTask::CreateTaskMatchingOfflineIds(
      store_.get(),
      base::BindOnce(&OfflinePageModelTaskified::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), callback),
      offline_ids);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::DeletePagesByClientIds(
    const std::vector<ClientId>& client_ids,
    const DeletePageCallback& callback) {
  auto task = DeletePageTask::CreateTaskMatchingClientIds(
      store_.get(),
      base::BindOnce(&OfflinePageModelTaskified::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), callback),
      client_ids);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::DeletePagesByClientIdsAndOrigin(
    const std::vector<ClientId>& client_ids,
    const std::string& origin,
    const DeletePageCallback& callback) {
  auto task = DeletePageTask::CreateTaskMatchingClientIdsAndOrigin(
      store_.get(),
      base::BindOnce(&OfflinePageModelTaskified::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), callback),
      client_ids, origin);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::DeleteCachedPagesByURLPredicate(
    const UrlPredicate& predicate,
    const DeletePageCallback& callback) {
  auto task = DeletePageTask::CreateTaskMatchingUrlPredicateForCachedPages(
      store_.get(),
      base::BindOnce(&OfflinePageModelTaskified::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), callback),
      policy_controller_.get(), predicate);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetAllPages(
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingAllPages(
      store_.get(), base::BindRepeating(&ReportSavedPagesCount, callback));
  task_queue_.AddTask(std::move(task));
  ScheduleMaintenanceTasks();
}

void OfflinePageModelTaskified::GetPageByOfflineId(
    int64_t offline_id,
    const SingleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingOfflineId(store_.get(), callback,
                                                        offline_id);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPageByGuid(
    const std::string& guid,
    const SingleOfflinePageItemCallback& callback) {
  auto task =
      GetPagesTask::CreateTaskMatchingGuid(store_.get(), callback, guid);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesByClientIds(
    const std::vector<ClientId>& client_ids,
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingClientIds(store_.get(), callback,
                                                        client_ids);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesByURL(
    const GURL& url,
    URLSearchMode url_search_mode,
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingUrl(store_.get(), callback, url);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesByNamespace(
    const std::string& name_space,
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingNamespace(store_.get(), callback,
                                                        name_space);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesRemovedOnCacheReset(
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingPagesRemovedOnCacheReset(
      store_.get(), callback, policy_controller_.get());
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesSupportedByDownloads(
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingPagesSupportedByDownloads(
      store_.get(), callback, policy_controller_.get());
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPagesByRequestOrigin(
    const std::string& request_origin,
    const MultipleOfflinePageItemCallback& callback) {
  auto task = GetPagesTask::CreateTaskMatchingRequestOrigin(
      store_.get(), callback, request_origin);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetPageBySizeAndDigest(
    int64_t file_size,
    const std::string& digest,
    const SingleOfflinePageItemCallback& callback) {
  DCHECK_GT(file_size, 0);
  DCHECK(!digest.empty());
  auto task = GetPagesTask::CreateTaskMatchingSizeAndDigest(
      store_.get(), callback, file_size, digest);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::GetOfflineIdsForClientId(
    const ClientId& client_id,
    const MultipleOfflineIdCallback& callback) {
  // We're currently getting offline IDs by querying offline items based on
  // client ids, and then extract the offline IDs from the items. This is fine
  // since we're not expecting many pages with the same client ID.
  auto task = GetPagesTask::CreateTaskMatchingClientIds(
      store_.get(), base::Bind(&WrapInMultipleItemsCallback, callback),
      {client_id});
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::StoreThumbnail(
    const OfflinePageThumbnail& thumb) {
  task_queue_.AddTask(std::make_unique<StoreThumbnailTask>(
      store_.get(), thumb,
      base::BindOnce(&OfflinePageModelTaskified::OnStoreThumbnailDone,
                     weak_ptr_factory_.GetWeakPtr(), thumb)));
}

void OfflinePageModelTaskified::GetThumbnailByOfflineId(
    int64_t offline_id,
    base::OnceCallback<void(std::unique_ptr<OfflinePageThumbnail>)> callback) {
  task_queue_.AddTask(std::make_unique<GetThumbnailTask>(
      store_.get(), offline_id, std::move(callback)));
}

void OfflinePageModelTaskified::HasThumbnailForOfflineId(
    int64_t offline_id,
    base::OnceCallback<void(bool)> callback) {
  task_queue_.AddTask(std::make_unique<HasThumbnailTask>(
      store_.get(), offline_id, std::move(callback)));
}

const base::FilePath& OfflinePageModelTaskified::GetInternalArchiveDirectory(
    const std::string& name_space) const {
  if (policy_controller_->IsRemovedOnCacheReset(name_space))
    return archive_manager_->GetTemporaryArchivesDir();
  return archive_manager_->GetPrivateArchivesDir();
}

bool OfflinePageModelTaskified::IsArchiveInInternalDir(
    const base::FilePath& file_path) const {
  DCHECK(!file_path.empty());

  // TODO(jianli): Update this once persistent archives are moved into the
  // public directory.
  return archive_manager_->GetTemporaryArchivesDir().IsParent(file_path) ||
         archive_manager_->GetPrivateArchivesDir().IsParent(file_path);
}

ClientPolicyController* OfflinePageModelTaskified::GetPolicyController() {
  return policy_controller_.get();
}

OfflineEventLogger* OfflinePageModelTaskified::GetLogger() {
  return &offline_event_logger_;
}

void OfflinePageModelTaskified::InformSavePageDone(
    const SavePageCallback& callback,
    SavePageResult result,
    const ClientId& client_id,
    int64_t offline_id) {
  UMA_HISTOGRAM_ENUMERATION("OfflinePages.SavePageCount",
                            model_utils::ToNamespaceEnum(client_id.name_space),
                            OfflinePagesNamespaceEnumeration::RESULT_COUNT);
  base::UmaHistogramEnumeration(
      model_utils::AddHistogramSuffix(client_id.name_space,
                                      "OfflinePages.SavePageResult"),
      result, SavePageResult::RESULT_COUNT);

  // Report storage usage if saving page succeeded.
  if (result == SavePageResult::SUCCESS)
    archive_manager_->GetStorageStats(base::BindOnce(&ReportStorageUsage));

  if (result == SavePageResult::ARCHIVE_CREATION_FAILED)
    CreateArchivesDirectoryIfNeeded();
  if (!callback.is_null())
    callback.Run(result, offline_id);
}

void OfflinePageModelTaskified::OnCreateArchiveDone(
    const SavePageParams& save_page_params,
    int64_t offline_id,
    const base::Time& start_time,
    const SavePageCallback& callback,
    OfflinePageArchiver* archiver,
    ArchiverResult archiver_result,
    const GURL& saved_url,
    const base::FilePath& file_path,
    const base::string16& title,
    int64_t file_size,
    const std::string& file_hash) {
  if (archiver_result != ArchiverResult::SUCCESSFULLY_CREATED) {
    SavePageResult result = ArchiverResultToSavePageResult(archiver_result);
    InformSavePageDone(callback, result, save_page_params.client_id,
                       offline_id);
    ErasePendingArchiver(archiver);
    return;
  }
  if (save_page_params.url != saved_url) {
    DVLOG(1) << "Saved URL does not match requested URL.";
    InformSavePageDone(callback, SavePageResult::ARCHIVE_CREATION_FAILED,
                       save_page_params.client_id, offline_id);
    ErasePendingArchiver(archiver);
    return;
  }

  OfflinePageItem offline_page(saved_url, offline_id,
                               save_page_params.client_id, file_path, file_size,
                               start_time);
  offline_page.title = title;
  offline_page.digest = file_hash;
  offline_page.request_origin = save_page_params.request_origin;
  // Don't record the original URL if it is identical to the final URL. This is
  // because some websites might route the redirect finally back to itself upon
  // the completion of certain action, i.e., authentication, in the middle.
  if (skip_clearing_original_url_for_testing_ ||
      save_page_params.original_url != offline_page.url) {
    offline_page.original_url = save_page_params.original_url;
  }

  if (IsOfflinePagesSharingEnabled() &&
      policy_controller_->IsUserRequestedDownload(
          offline_page.client_id.name_space)) {
    // If the user intentionally downloaded the page, move it to a public place.
    PublishArchive(offline_page, callback, archiver);
  } else {
    // For pages that we download on the user's behalf, we keep them in an
    // internal chrome directory, and add them here to the OfflinePageModel
    // database.
    AddPage(offline_page,
            base::Bind(&OfflinePageModelTaskified::OnAddPageForSavePageDone,
                       weak_ptr_factory_.GetWeakPtr(), callback, offline_page));
  }
  ErasePendingArchiver(archiver);
}

void OfflinePageModelTaskified::ErasePendingArchiver(
    OfflinePageArchiver* archiver) {
  pending_archivers_.erase(
      std::find_if(pending_archivers_.begin(), pending_archivers_.end(),
                   [archiver](const std::unique_ptr<OfflinePageArchiver>& a) {
                     return a.get() == archiver;
                   }));
}

void OfflinePageModelTaskified::PublishArchive(
    const OfflinePageItem& offline_page,
    const SavePageCallback& save_page_callback,
    OfflinePageArchiver* archiver) {
  archiver->PublishArchive(
      offline_page, task_runner_, archive_manager_->GetPublicArchivesDir(),
      download_manager_.get(),
      base::BindOnce(&OfflinePageModelTaskified::PublishArchiveDone,
                     weak_ptr_factory_.GetWeakPtr(), save_page_callback));
}

void OfflinePageModelTaskified::PublishArchiveDone(
    const SavePageCallback& save_page_callback,
    const OfflinePageItem& offline_page,
    PublishArchiveResult* move_results) {
  if (move_results->move_result != SavePageResult::SUCCESS) {
    save_page_callback.Run(move_results->move_result, 0LL);
    return;
  }
  OfflinePageItem page = offline_page;
  page.file_path = move_results->new_file_path;
  page.system_download_id = move_results->download_id;

  AddPage(page,
          base::Bind(&OfflinePageModelTaskified::OnAddPageForSavePageDone,
                     weak_ptr_factory_.GetWeakPtr(), save_page_callback, page));
}

void OfflinePageModelTaskified::PublishInternalArchive(
    const OfflinePageItem& offline_page,
    std::unique_ptr<OfflinePageArchiver> archiver,
    PublishPageCallback publish_done_callback) {
  archiver->PublishArchive(
      offline_page, task_runner_, archive_manager_->GetPublicArchivesDir(),
      download_manager_.get(),
      base::BindOnce(&OfflinePageModelTaskified::PublishInternalArchiveDone,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(publish_done_callback)));
}

void OfflinePageModelTaskified::PublishInternalArchiveDone(
    PublishPageCallback publish_done_callback,
    const OfflinePageItem& offline_page,
    PublishArchiveResult* publish_results) {
  base::FilePath file_path = publish_results->new_file_path;
  SavePageResult result = publish_results->move_result;
  // Call the callback with success == false if we failed to move the page.
  if (result != SavePageResult::SUCCESS) {
    std::move(publish_done_callback).Run(file_path, result);
    return;
  }

  // Update the OfflinePageModel with the new location for the page, which is
  // found in move_results.new_file_path, and with the download ID found at
  // move_results.download_id.  Return the updated offline_page to the callback.
  auto task = std::make_unique<UpdateFilePathTask>(
      store_.get(), offline_page.offline_id, file_path,
      base::BindOnce(&OnUpdateFilePathDone, std::move(publish_done_callback),
                     file_path, result));
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::OnAddPageForSavePageDone(
    const SavePageCallback& callback,
    const OfflinePageItem& page_attempted,
    AddPageResult add_page_result,
    int64_t offline_id) {
  SavePageResult save_page_result =
      AddPageResultToSavePageResult(add_page_result);
  InformSavePageDone(callback, save_page_result, page_attempted.client_id,
                     offline_id);
  if (save_page_result == SavePageResult::SUCCESS) {
    ReportPageHistogramAfterSuccessfulSaving(page_attempted, GetCurrentTime());
    // TODO(romax): Just keep the same with logic in OPMImpl (which was wrong).
    // This should be fixed once we have the new strategy for clearing pages.
    if (policy_controller_->GetPolicy(page_attempted.client_id.name_space)
            .pages_allowed_per_url != kUnlimitedPages) {
      RemovePagesMatchingUrlAndNamespace(page_attempted);
    }
    offline_event_logger_.RecordPageSaved(page_attempted.client_id.name_space,
                                          page_attempted.url.spec(),
                                          page_attempted.offline_id);
  }
  ScheduleMaintenanceTasks();
}

void OfflinePageModelTaskified::OnAddPageDone(const OfflinePageItem& page,
                                              const AddPageCallback& callback,
                                              AddPageResult result) {
  callback.Run(result, page.offline_id);
  if (result == AddPageResult::SUCCESS) {
    for (Observer& observer : observers_)
      observer.OfflinePageAdded(this, page);
  }
}

void OfflinePageModelTaskified::OnDeleteDone(
    const DeletePageCallback& callback,
    DeletePageResult result,
    const std::vector<OfflinePageModel::DeletedPageInfo>& infos) {
  UMA_HISTOGRAM_ENUMERATION("OfflinePages.DeletePageResult", result,
                            DeletePageResult::RESULT_COUNT);
  std::vector<int64_t> system_download_ids;

  // Notify observers and run callback.
  for (const auto& info : infos) {
    UMA_HISTOGRAM_ENUMERATION(
        "OfflinePages.DeletePageCount",
        model_utils::ToNamespaceEnum(info.client_id.name_space),
        OfflinePagesNamespaceEnumeration::RESULT_COUNT);
    offline_event_logger_.RecordPageDeleted(info.offline_id);
    for (Observer& observer : observers_)
      observer.OfflinePageDeleted(info);
    if (info.system_download_id != 0)
      system_download_ids.push_back(info.system_download_id);
  }

  // Remove the page from the system download manager. We don't need to wait for
  // completion before calling the delete page callback.
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&OfflinePageModelTaskified::RemoveFromDownloadManager,
                     download_manager_.get(), system_download_ids));

  if (!callback.is_null())
    callback.Run(result);
}

void OfflinePageModelTaskified::OnStoreThumbnailDone(
    const OfflinePageThumbnail& thumbnail,
    bool success) {
  if (success) {
    for (Observer& observer : observers_)
      observer.ThumbnailAdded(this, thumbnail);
  }
}

void OfflinePageModelTaskified::RemoveFromDownloadManager(
    SystemDownloadManager* download_manager,
    const std::vector<int64_t>& system_download_ids) {
  if (system_download_ids.size() > 0)
    download_manager->Remove(system_download_ids);
}

void OfflinePageModelTaskified::ScheduleMaintenanceTasks() {
  if (skip_maintenance_tasks_for_testing_)
    return;
  // If not enough time has passed, don't queue maintenance tasks.
  base::Time now = GetCurrentTime();
  if (now - last_maintenance_tasks_schedule_time_ < kClearStorageInterval)
    return;

  bool first_run = last_maintenance_tasks_schedule_time_.is_null();
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&OfflinePageModelTaskified::RunMaintenanceTasks,
                     weak_ptr_factory_.GetWeakPtr(), now, first_run),
      kMaintenanceTasksDelay);

  last_maintenance_tasks_schedule_time_ = now;
}

void OfflinePageModelTaskified::RunMaintenanceTasks(const base::Time now,
                                                    bool first_run) {
  DCHECK(!skip_maintenance_tasks_for_testing_);
  // If this is the first run of this session, enqueue the startup maintenance
  // task, including consistency checks, legacy archive directory cleaning and
  // reporting storage usage UMA.
  if (first_run) {
    task_queue_.AddTask(std::make_unique<StartupMaintenanceTask>(
        store_.get(), archive_manager_.get(), policy_controller_.get()));

    task_queue_.AddTask(std::make_unique<CleanupThumbnailsTask>(
        store_.get(), GetCurrentTime(), base::DoNothing()));
  }

  task_queue_.AddTask(std::make_unique<ClearStorageTask>(
      store_.get(), archive_manager_.get(), policy_controller_.get(), now,
      base::BindOnce(&OfflinePageModelTaskified::OnClearCachedPagesDone,
                     weak_ptr_factory_.GetWeakPtr())));

  // TODO(https://crbug.com/834902) This might need a better execution plan.
  task_queue_.AddTask(std::make_unique<PersistentPageConsistencyCheckTask>(
      store_.get(), archive_manager_.get(), policy_controller_.get(), now,
      base::BindOnce(
          &OfflinePageModelTaskified::OnPersistentPageConsistencyCheckDone,
          weak_ptr_factory_.GetWeakPtr())));
}

void OfflinePageModelTaskified::OnPersistentPageConsistencyCheckDone(
    bool success,
    const std::vector<int64_t>& pages_deleted) {
  // If there's no persistent page expired, save some effort by exiting early.
  // TODO(https://crbug.com/834909), use the temporary hidden bit in
  // DownloadUIAdapter instead of calling remove directly.
  if (pages_deleted.size() > 0)
    download_manager_->Remove(pages_deleted);
}

void OfflinePageModelTaskified::OnClearCachedPagesDone(
    size_t deleted_page_count,
    ClearStorageResult result) {
  UMA_HISTOGRAM_ENUMERATION("OfflinePages.ClearTemporaryPages.Result", result,
                            ClearStorageResult::RESULT_COUNT);
  if (deleted_page_count > 0) {
    UMA_HISTOGRAM_COUNTS("OfflinePages.ClearTemporaryPages.BatchSize",
                         deleted_page_count);
  }
}

void OfflinePageModelTaskified::PostSelectItemsMarkedForUpgrade() {
  // TODO(fgorski): Make storage permission check. Here or later?
  // TODO(fgorski): Check disk space here.
  if (!IsOfflinePagesSharingEnabled())
    return;

  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::BindRepeating(
          &OfflinePageModelTaskified::SelectItemsMarkedForUpgrade,
          weak_ptr_factory_.GetWeakPtr()),
      kInitialUpgradeSelectionDelay);
}

void OfflinePageModelTaskified::SelectItemsMarkedForUpgrade() {
  // TODO(fgorski): Add legacy Persistent path in archive manager to know which
  // files still need upgrade.
  auto task = GetPagesTask::CreateTaskSelectingItemsMarkedForUpgrade(
      store_.get(),
      base::BindRepeating(
          &OfflinePageModelTaskified::OnSelectItemsMarkedForUpgradeDone,
          weak_ptr_factory_.GetWeakPtr()));
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::OnSelectItemsMarkedForUpgradeDone(
    const MultipleOfflinePageItemResult& pages_for_upgrade) {
  // TODO(fgorski): Save the list of ID to feed them into the upgrade task.
}

void OfflinePageModelTaskified::RemovePagesMatchingUrlAndNamespace(
    const OfflinePageItem& page) {
  auto task = DeletePageTask::CreateTaskDeletingForPageLimit(
      store_.get(),
      base::BindOnce(&OfflinePageModelTaskified::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), base::DoNothing()),
      policy_controller_.get(), page);
  task_queue_.AddTask(std::move(task));
}

void OfflinePageModelTaskified::CreateArchivesDirectoryIfNeeded() {
  // No callback is required here.
  // TODO(romax): Remove the callback from the interface once the other
  // consumers of this API can also drop the callback.
  archive_manager_->EnsureArchivesDirCreated(base::DoNothing());
}

base::Time OfflinePageModelTaskified::GetCurrentTime() {
  CHECK(clock_);
  return clock_->Now();
}

}  // namespace offline_pages
