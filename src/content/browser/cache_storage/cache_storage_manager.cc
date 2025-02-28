// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/cache_storage/cache_storage_manager.h"

#include <stdint.h>

#include <map>
#include <numeric>
#include <set>
#include <utility>

#include "base/barrier_closure.h"
#include "base/bind.h"
#include "base/containers/id_map.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/memory/ptr_util.h"
#include "base/sequenced_task_runner.h"
#include "base/sha1.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "content/browser/cache_storage/cache_storage.h"
#include "content/browser/cache_storage/cache_storage.pb.h"
#include "content/browser/cache_storage/cache_storage_quota_client.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/url_util.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "storage/common/database/database_identifier.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

namespace {

bool DeleteDirs(const std::vector<base::FilePath>& paths) {
  return std::accumulate(
      paths.begin(), paths.end(), true,
      [](bool result, const base::FilePath& path) {
        return base::DeleteFile(path, true /* recursive */) && result;
      });
}

void DeleteOriginDidDeleteDirs(storage::QuotaClient::DeletionCallback callback,
                               bool rv) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback),
                     rv ? blink::mojom::QuotaStatusCode::kOk
                        : blink::mojom::QuotaStatusCode::kErrorAbort));
}

// Calculate the sum of all cache sizes in this store, but only if all sizes are
// known. If one or more sizes are not known then return kSizeUnknown.
int64_t GetCacheStorageSize(const proto::CacheStorageIndex& index) {
  int64_t storage_size = 0;
  for (int i = 0, max = index.cache_size(); i < max; ++i) {
    const proto::CacheStorageIndex::Cache& cache = index.cache(i);
    if (!cache.has_size() || cache.size() == CacheStorage::kSizeUnknown)
      return CacheStorage::kSizeUnknown;
    storage_size += cache.size();
  }
  return storage_size;
}

// Open the various cache directories' index files and extract their origins,
// sizes (if current), and last modified times.
void ListOriginsAndLastModifiedOnTaskRunner(
    std::vector<CacheStorageUsageInfo>* usages,
    base::FilePath root_path) {
  base::FileEnumerator file_enum(root_path, false /* recursive */,
                                 base::FileEnumerator::DIRECTORIES);

  base::FilePath path;
  while (!(path = file_enum.Next()).empty()) {
    base::FilePath index_path = path.AppendASCII(CacheStorage::kIndexFileName);
    base::File::Info file_info;
    base::Time index_last_modified;
    if (GetFileInfo(index_path, &file_info))
      index_last_modified = file_info.last_modified;
    std::string protobuf;
    base::ReadFileToString(path.AppendASCII(CacheStorage::kIndexFileName),
                           &protobuf);
    proto::CacheStorageIndex index;
    // TODO(crbug.com/838908): Clarify behavior with multiple cache owners.
    if (index.ParseFromString(protobuf)) {
      if (index.has_origin()) {
        if (base::GetFileInfo(path, &file_info)) {
          int64_t storage_size = CacheStorage::kSizeUnknown;
          if (file_info.last_modified < index_last_modified)
            storage_size = GetCacheStorageSize(index);
          usages->push_back(CacheStorageUsageInfo(
              GURL(index.origin()), storage_size, file_info.last_modified));
        }
      }
    }
  }
}

std::set<url::Origin> ListOriginsOnTaskRunner(base::FilePath root_path) {
  std::vector<CacheStorageUsageInfo> usages;
  ListOriginsAndLastModifiedOnTaskRunner(&usages, root_path);

  std::set<url::Origin> out_origins;
  for (const CacheStorageUsageInfo& usage : usages)
    out_origins.insert(url::Origin::Create(usage.origin));

  return out_origins;
}

void GetOriginsForHostDidListOrigins(
    const std::string& host,
    storage::QuotaClient::GetOriginsCallback callback,
    const std::set<url::Origin>& origins) {
  std::set<url::Origin> out_origins;
  for (const url::Origin& origin : origins) {
    if (host == net::GetHostOrSpecFromURL(origin.GetURL()))
      out_origins.insert(origin);
  }
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), out_origins));
}

void AllOriginSizesReported(
    std::unique_ptr<std::vector<CacheStorageUsageInfo>> usages,
    CacheStorageContext::GetUsageInfoCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), *usages));
}

void OneOriginSizeReported(base::OnceClosure callback,
                           CacheStorageUsageInfo* usage,
                           int64_t size) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  DCHECK_NE(size, CacheStorage::kSizeUnknown);
  usage->total_size_bytes = size;
  base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, std::move(callback));
}

}  // namespace

// static
std::unique_ptr<CacheStorageManager> CacheStorageManager::Create(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
    scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy) {
  base::FilePath root_path = path;
  if (!path.empty()) {
    root_path = path.Append(ServiceWorkerContextCore::kServiceWorkerDirectory)
                    .AppendASCII("CacheStorage");
  }

  return base::WrapUnique(new CacheStorageManager(
      root_path, std::move(cache_task_runner), std::move(quota_manager_proxy)));
}

// static
std::unique_ptr<CacheStorageManager> CacheStorageManager::Create(
    CacheStorageManager* old_manager) {
  std::unique_ptr<CacheStorageManager> manager(new CacheStorageManager(
      old_manager->root_path(), old_manager->cache_task_runner(),
      old_manager->quota_manager_proxy_.get()));
  // These values may be NULL, in which case this will be called again later by
  // the dispatcher host per usual.
  manager->SetBlobParametersForCache(old_manager->url_request_context_getter(),
                                     old_manager->blob_storage_context());
  return manager;
}

CacheStorageManager::~CacheStorageManager() = default;

void CacheStorageManager::OpenCache(
    const url::Origin& origin,
    CacheStorageOwner owner,
    const std::string& cache_name,
    CacheStorage::CacheAndErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);

  cache_storage->OpenCache(cache_name, std::move(callback));
}

void CacheStorageManager::HasCache(
    const url::Origin& origin,
    CacheStorageOwner owner,
    const std::string& cache_name,
    CacheStorage::BoolAndErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);
  cache_storage->HasCache(cache_name, std::move(callback));
}

void CacheStorageManager::DeleteCache(const url::Origin& origin,
                                      CacheStorageOwner owner,
                                      const std::string& cache_name,
                                      CacheStorage::ErrorCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);
  cache_storage->DoomCache(cache_name, std::move(callback));
}

void CacheStorageManager::EnumerateCaches(
    const url::Origin& origin,
    CacheStorageOwner owner,
    CacheStorage::IndexCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);

  cache_storage->EnumerateCaches(std::move(callback));
}

void CacheStorageManager::MatchCache(
    const url::Origin& origin,
    CacheStorageOwner owner,
    const std::string& cache_name,
    std::unique_ptr<ServiceWorkerFetchRequest> request,
    const CacheStorageCacheQueryParams& match_params,
    CacheStorageCache::ResponseCallback callback) {
  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);

  cache_storage->MatchCache(cache_name, std::move(request), match_params,
                            std::move(callback));
}

void CacheStorageManager::MatchAllCaches(
    const url::Origin& origin,
    CacheStorageOwner owner,
    std::unique_ptr<ServiceWorkerFetchRequest> request,
    const CacheStorageCacheQueryParams& match_params,
    CacheStorageCache::ResponseCallback callback) {
  CacheStorage* cache_storage = FindOrCreateCacheStorage(origin, owner);

  cache_storage->MatchAllCaches(std::move(request), match_params,
                                std::move(callback));
}

void CacheStorageManager::SetBlobParametersForCache(
    scoped_refptr<net::URLRequestContextGetter> request_context_getter,
    base::WeakPtr<storage::BlobStorageContext> blob_storage_context) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(cache_storage_map_.empty());
  DCHECK(!request_context_getter_ ||
         request_context_getter_.get() == request_context_getter.get());
  DCHECK(!blob_context_ || blob_context_.get() == blob_storage_context.get());
  request_context_getter_ = std::move(request_context_getter);
  blob_context_ = blob_storage_context;
}

void CacheStorageManager::AddObserver(
    CacheStorageContextImpl::Observer* observer) {
  DCHECK(!observers_.HasObserver(observer));
  observers_.AddObserver(observer);
}

void CacheStorageManager::RemoveObserver(
    CacheStorageContextImpl::Observer* observer) {
  observers_.RemoveObserver(observer);
}

void CacheStorageManager::NotifyCacheListChanged(const url::Origin& origin) {
  for (auto& observer : observers_)
    observer.OnCacheListChanged(origin);
}

void CacheStorageManager::NotifyCacheContentChanged(const url::Origin& origin,
                                                    const std::string& name) {
  for (auto& observer : observers_)
    observer.OnCacheContentChanged(origin, name);
}

void CacheStorageManager::GetAllOriginsUsage(
    CacheStorageContext::GetUsageInfoCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  std::unique_ptr<std::vector<CacheStorageUsageInfo>> usages(
      new std::vector<CacheStorageUsageInfo>());

  if (IsMemoryBacked()) {
    for (const auto& origin_details : cache_storage_map_) {
      usages->push_back(CacheStorageUsageInfo(
          origin_details.first.first.GetURL(), 0 /* size */,
          base::Time() /* last modified */));
    }
    GetAllOriginsUsageGetSizes(std::move(usages), std::move(callback));
    return;
  }

  std::vector<CacheStorageUsageInfo>* usages_ptr = usages.get();
  cache_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&ListOriginsAndLastModifiedOnTaskRunner, usages_ptr,
                     root_path_),
      base::BindOnce(&CacheStorageManager::GetAllOriginsUsageGetSizes,
                     weak_ptr_factory_.GetWeakPtr(), std::move(usages),
                     std::move(callback)));
}

void CacheStorageManager::GetAllOriginsUsageGetSizes(
    std::unique_ptr<std::vector<CacheStorageUsageInfo>> usages,
    CacheStorageContext::GetUsageInfoCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(usages);

  // The origin GURL and last modified times are set in |usages| but not the
  // size in bytes. Call each CacheStorage's Size() function to fill that out.
  std::vector<CacheStorageUsageInfo>* usages_ptr = usages.get();

  if (usages->empty()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), *usages));
    return;
  }

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      usages_ptr->size(),
      base::BindOnce(&AllOriginSizesReported, std::move(usages),
                     std::move(callback)));

  for (CacheStorageUsageInfo& usage : *usages_ptr) {
    if (usage.total_size_bytes != CacheStorage::kSizeUnknown) {
      base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, barrier_closure);
      continue;
    }
    CacheStorage* cache_storage = FindOrCreateCacheStorage(
        url::Origin::Create(usage.origin), CacheStorageOwner::kCacheAPI);
    cache_storage->Size(
        base::BindOnce(&OneOriginSizeReported, barrier_closure, &usage));
  }
}

void CacheStorageManager::GetOriginUsage(
    const url::Origin& origin,
    storage::QuotaClient::GetUsageCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  CacheStorage* cache_storage =
      FindOrCreateCacheStorage(origin, CacheStorageOwner::kCacheAPI);

  cache_storage->Size(std::move(callback));
}

void CacheStorageManager::GetOrigins(
    storage::QuotaClient::GetOriginsCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (IsMemoryBacked()) {
    std::set<url::Origin> origins;
    for (const auto& key_value : cache_storage_map_)
      origins.insert(key_value.first.first);

    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), origins));
    return;
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ListOriginsOnTaskRunner, root_path_),
      std::move(callback));
}

void CacheStorageManager::GetOriginsForHost(
    const std::string& host,
    storage::QuotaClient::GetOriginsCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (IsMemoryBacked()) {
    std::set<url::Origin> origins;
    for (const auto& key_value : cache_storage_map_) {
      if (host == net::GetHostOrSpecFromURL(key_value.first.first.GetURL()))
        origins.insert(key_value.first.first);
    }
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), origins));
    return;
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ListOriginsOnTaskRunner, root_path_),
      base::BindOnce(&GetOriginsForHostDidListOrigins, host,
                     std::move(callback)));
}

void CacheStorageManager::DeleteOriginData(
    const url::Origin& origin,
    storage::QuotaClient::DeletionCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  base::RepeatingClosure barrier_closure = base::BarrierClosure(
      static_cast<int>(CacheStorageOwner::kMaxValue) + 1,
      base::BindOnce(&CacheStorageManager::DeleteOriginDidClose,
                     weak_ptr_factory_.GetWeakPtr(), origin,
                     std::move(callback)));

  for (int i = static_cast<int>(CacheStorageOwner::kMinValue);
       i <= static_cast<int>(CacheStorageOwner::kMaxValue); i++) {
    CacheStorageOwner owner = static_cast<CacheStorageOwner>(i);

    // Create the CacheStorage for the origin if it hasn't been loaded yet.
    FindOrCreateCacheStorage(origin, owner);

    // We need another look-up to release the CacheStorage ptr.
    CacheStorage* cache_storage = cache_storage_map_[{origin, owner}].release();

    cache_storage->ResetManager();
    cache_storage_map_.erase({origin, owner});
    cache_storage->GetSizeThenCloseAllCaches(
        base::BindOnce(&CacheStorageManager::NotifyStorageModified,
                       weak_ptr_factory_.GetWeakPtr(), origin,
                       base::WrapUnique(cache_storage), barrier_closure));
  }
}

void CacheStorageManager::DeleteOriginData(const url::Origin& origin) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DeleteOriginData(origin, base::DoNothing());
}

void CacheStorageManager::NotifyStorageModified(
    const url::Origin& origin,
    std::unique_ptr<CacheStorage> cache_storage,
    base::OnceClosure callback,
    int64_t origin_size) {
  // TODO(jkarlin): Deleting the storage leaves any unfinished operations
  // hanging, resulting in unresolved promises. Fix this by returning early from
  // CacheStorage operations posted after GetSizeThenCloseAllCaches is called.
  cache_storage.reset();

  quota_manager_proxy_->NotifyStorageModified(
      storage::QuotaClient::kServiceWorkerCache, origin,
      blink::mojom::StorageType::kTemporary, -1 * origin_size);
  std::move(callback).Run();
}

void CacheStorageManager::DeleteOriginDidClose(
    const url::Origin& origin,
    storage::QuotaClient::DeletionCallback callback) {
  NotifyCacheListChanged(origin);

  if (IsMemoryBacked()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  blink::mojom::QuotaStatusCode::kOk));
    return;
  }

  std::vector<base::FilePath> delete_paths;
  for (int owner = static_cast<int>(CacheStorageOwner::kMinValue);
       owner <= static_cast<int>(CacheStorageOwner::kMaxValue); owner++) {
    delete_paths.push_back(ConstructOriginPath(
        root_path_, origin, static_cast<CacheStorageOwner>(owner)));
  }

  PostTaskAndReplyWithResult(
      cache_task_runner_.get(), FROM_HERE,
      base::BindOnce(&DeleteDirs, std::move(delete_paths)),
      base::BindOnce(&DeleteOriginDidDeleteDirs, std::move(callback)));
}

CacheStorageManager::CacheStorageManager(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
    scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy)
    : root_path_(path),
      cache_task_runner_(std::move(cache_task_runner)),
      quota_manager_proxy_(std::move(quota_manager_proxy)),
      weak_ptr_factory_(this) {
  if (quota_manager_proxy_.get()) {
    quota_manager_proxy_->RegisterClient(
        new CacheStorageQuotaClient(weak_ptr_factory_.GetWeakPtr()));
  }
}

CacheStorage* CacheStorageManager::FindOrCreateCacheStorage(
    const url::Origin& origin,
    CacheStorageOwner owner) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(request_context_getter_);
  CacheStorageMap::const_iterator it = cache_storage_map_.find({origin, owner});
  if (it == cache_storage_map_.end()) {
    CacheStorage* cache_storage = new CacheStorage(
        ConstructOriginPath(root_path_, origin, owner), IsMemoryBacked(),
        cache_task_runner_.get(), request_context_getter_, quota_manager_proxy_,
        blob_context_, this, origin);
    cache_storage_map_[{origin, owner}] = base::WrapUnique(cache_storage);
    return cache_storage;
  }
  return it->second.get();
}

// static
base::FilePath CacheStorageManager::ConstructOriginPath(
    const base::FilePath& root_path,
    const url::Origin& origin,
    CacheStorageOwner owner) {
  std::string identifier = storage::GetIdentifierFromOrigin(origin);
  if (owner != CacheStorageOwner::kCacheAPI) {
    identifier += "-" + std::to_string(static_cast<int>(owner));
  }
  const std::string origin_hash = base::SHA1HashString(identifier);
  const std::string origin_hash_hex = base::ToLowerASCII(
      base::HexEncode(origin_hash.c_str(), origin_hash.length()));
  return root_path.AppendASCII(origin_hash_hex);
}

}  // namespace content
