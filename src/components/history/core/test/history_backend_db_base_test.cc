// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/history/core/test/history_backend_db_base_test.h"

#include "base/files/file_path.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/stringprintf.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/history/core/browser/download_constants.h"
#include "components/history/core/browser/download_row.h"
#include "components/history/core/browser/history_backend.h"
#include "components/history/core/browser/history_backend_client.h"
#include "components/history/core/browser/history_constants.h"
#include "components/history/core/browser/history_database_params.h"
#include "components/history/core/browser/in_memory_history_backend.h"
#include "components/history/core/test/database_test_utils.h"
#include "components/history/core/test/test_history_database.h"
#include "url/gurl.h"

namespace history {

// Delegate class for when we create a backend without a HistoryService.
class BackendDelegate : public HistoryBackend::Delegate {
 public:
  explicit BackendDelegate(HistoryBackendDBBaseTest* history_test)
      : history_test_(history_test) {}

  // HistoryBackend::Delegate implementation.
  void NotifyProfileError(sql::InitStatus init_status,
                          const std::string& diagnostics) override {
    history_test_->last_profile_error_ = init_status;
  }
  void SetInMemoryBackend(
      std::unique_ptr<InMemoryHistoryBackend> backend) override {
    // Save the in-memory backend to the history test object, this happens
    // synchronously, so we don't have to do anything fancy.
    history_test_->in_mem_backend_.swap(backend);
  }
  void NotifyFaviconsChanged(const std::set<GURL>& page_urls,
                             const GURL& icon_url) override {}
  void NotifyURLVisited(ui::PageTransition transition,
                        const URLRow& row,
                        const RedirectList& redirects,
                        base::Time visit_time) override {}
  void NotifyURLsModified(const URLRows& changed_urls) override {}
  void NotifyURLsDeleted(DeletionInfo deletion_info) override {}
  void NotifyKeywordSearchTermUpdated(const URLRow& row,
                                      KeywordID keyword_id,
                                      const base::string16& term) override {}
  void NotifyKeywordSearchTermDeleted(URLID url_id) override {}
  void DBLoaded() override {}

 private:
  HistoryBackendDBBaseTest* history_test_;
};

HistoryBackendDBBaseTest::HistoryBackendDBBaseTest()
    : scoped_task_environment_(
          base::test::ScopedTaskEnvironment::MainThreadType::UI),
      db_(nullptr),
      last_profile_error_(sql::INIT_OK) {}

HistoryBackendDBBaseTest::~HistoryBackendDBBaseTest() {
}

void HistoryBackendDBBaseTest::SetUp() {
  ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
  history_dir_ = temp_dir_.GetPath().AppendASCII("HistoryBackendDBBaseTest");
  ASSERT_TRUE(base::CreateDirectory(history_dir_));
}

void HistoryBackendDBBaseTest::TearDown() {
  DeleteBackend();

  // Make sure we don't have any event pending that could disrupt the next
  // test.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::RunLoop::QuitCurrentWhenIdleClosureDeprecated());
  base::RunLoop().Run();
}

void HistoryBackendDBBaseTest::CreateBackendAndDatabase() {
  backend_ = new HistoryBackend(new BackendDelegate(this), nullptr,
                                base::ThreadTaskRunnerHandle::Get());
  backend_->Init(false,
                 TestHistoryDatabaseParamsForPath(history_dir_));
  db_ = backend_->db_.get();
  DCHECK(in_mem_backend_) << "Mem backend should have been set by "
      "HistoryBackend::Init";
}

void HistoryBackendDBBaseTest::CreateBackendAndDatabaseAllowFail() {
  backend_ = new HistoryBackend(new BackendDelegate(this), nullptr,
                                base::ThreadTaskRunnerHandle::Get());
  backend_->Init(false,
                 TestHistoryDatabaseParamsForPath(history_dir_));
  db_ = backend_->db_.get();
}

void HistoryBackendDBBaseTest::CreateDBVersion(int version) {
  base::FilePath data_path;
  ASSERT_TRUE(GetTestDataHistoryDir(&data_path));
  data_path =
      data_path.AppendASCII(base::StringPrintf("history.%d.sql", version));
  ASSERT_NO_FATAL_FAILURE(
      ExecuteSQLScript(data_path, history_dir_.Append(kHistoryFilename)));
}

void HistoryBackendDBBaseTest::DeleteBackend() {
  if (backend_.get()) {
    backend_->Closing();
    backend_ = nullptr;
  }
}

bool HistoryBackendDBBaseTest::AddDownload(uint32_t id,
                                           const std::string& guid,
                                           DownloadState state,
                                           base::Time time) {
  DownloadRow download;
  download.current_path = base::FilePath(FILE_PATH_LITERAL("current-path"));
  download.target_path = base::FilePath(FILE_PATH_LITERAL("target-path"));
  download.url_chain.push_back(GURL("foo-url"));
  download.referrer_url = GURL("http://referrer.example.com/");
  download.site_url = GURL("http://site-url.example.com");
  download.tab_url = GURL("http://tab-url.example.com/");
  download.tab_referrer_url = GURL("http://tab-referrer-url.example.com/");
  download.http_method = std::string();
  download.mime_type = "application/vnd.oasis.opendocument.text";
  download.original_mime_type = "application/octet-stream";
  download.start_time = time;
  download.end_time = time;
  download.received_bytes = 0;
  download.total_bytes = 512;
  download.state = state;
  download.danger_type = DownloadDangerType::NOT_DANGEROUS;
  download.interrupt_reason = kTestDownloadInterruptReasonNone;
  download.id = id;
  download.guid = guid;
  download.opened = false;
  download.last_access_time = time;
  download.transient = true;
  download.by_ext_id = "by_ext_id";
  download.by_ext_name = "by_ext_name";
  return db_->CreateDownload(download);
}

}  // namespace history
