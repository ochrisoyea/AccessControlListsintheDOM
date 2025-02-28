// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_path_override.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_item_model.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/download/download_target_info.h"
#include "chrome/browser/safe_browsing/download_protection/download_protection_util.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/mock_download_item.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/test/mock_download_manager.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(FULL_SAFE_BROWSING)
#include "chrome/browser/safe_browsing/download_protection/download_protection_service.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "content/public/browser/plugin_service.h"
#endif

#if defined(OS_ANDROID)
#include "chrome/browser/infobars/infobar_service.h"
#include "components/infobars/core/infobar.h"
#include "components/infobars/core/infobar_delegate.h"
#include "components/infobars/core/infobar_manager.h"
#endif

using ::testing::AnyNumber;
using ::testing::AtMost;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Ref;
using ::testing::Return;
using ::testing::ReturnArg;
using ::testing::ReturnPointee;
using ::testing::ReturnRef;
using ::testing::ReturnRefOfCopy;
using ::testing::SetArgPointee;
using ::testing::WithArg;
using ::testing::_;
using download::DownloadItem;
using safe_browsing::DownloadFileType;

namespace {

class MockWebContentsDelegate : public content::WebContentsDelegate {
 public:
  ~MockWebContentsDelegate() override {}
};

// Google Mock action that posts a task to the current message loop that invokes
// the first argument of the mocked method as a callback. Said argument must be
// a base::Callback<void(ParamType0, ParamType1)>. |result0| and |result1| must
// be of ParamType0 and ParamType1 respectively and will be bound as such.
//
// Example:
//    class FooClass {
//     public:
//      virtual void Foo(base::Callback<void(bool, std::string)> callback);
//    };
//    ...
//    EXPECT_CALL(mock_fooclass_instance, Foo(callback))
//      .WillOnce(ScheduleCallback2(false, "hello"));
//
ACTION_P2(ScheduleCallback2, result0, result1) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(arg0, result0, result1));
}

// Struct for holding the result of calling DetermineDownloadTarget.
struct DetermineDownloadTargetResult {
  DetermineDownloadTargetResult();

  base::FilePath target_path;
  download::DownloadItem::TargetDisposition disposition;
  download::DownloadDangerType danger_type;
  base::FilePath intermediate_path;
  download::DownloadInterruptReason interrupt_reason;
};

DetermineDownloadTargetResult::DetermineDownloadTargetResult()
    : disposition(download::DownloadItem::TARGET_DISPOSITION_OVERWRITE),
      danger_type(download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS),
      interrupt_reason(download::DOWNLOAD_INTERRUPT_REASON_NONE) {}

// Subclass of the ChromeDownloadManagerDelegate that replaces a few interaction
// points for ease of testing.
class TestChromeDownloadManagerDelegate : public ChromeDownloadManagerDelegate {
 public:
  explicit TestChromeDownloadManagerDelegate(Profile* profile)
      : ChromeDownloadManagerDelegate(profile) {
    ON_CALL(*this, MockCheckDownloadUrl(_, _))
        .WillByDefault(Return(download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS));
    ON_CALL(*this, GetDownloadProtectionService())
        .WillByDefault(Return(nullptr));
    ON_CALL(*this, MockReserveVirtualPath(_, _, _, _, _))
        .WillByDefault(DoAll(SetArgPointee<4>(PathValidationResult::SUCCESS),
                             ReturnArg<1>()));
  }

  ~TestChromeDownloadManagerDelegate() override {}

  // The concrete implementation talks to the ExtensionDownloadsEventRouter to
  // dispatch a OnDeterminingFilename event. While we would like to test this as
  // well in this unit test, we are currently going to rely on the extension
  // browser test to provide test coverage here. Instead we are going to mock it
  // out for unit tests.
  void NotifyExtensions(download::DownloadItem* download,
                        const base::FilePath& suggested_virtual_path,
                        const NotifyExtensionsCallback& callback) override {
    callback.Run(base::FilePath(),
                 DownloadPathReservationTracker::UNIQUIFY);
  }

  // DownloadPathReservationTracker talks to the underlying file system. For
  // tests we are going to mock it out so that we can test how
  // ChromeDownloadManagerDelegate reponds to various DownloadTargetDeterminer
  // results.
  void ReserveVirtualPath(
      download::DownloadItem* download,
      const base::FilePath& virtual_path,
      bool create_directory,
      DownloadPathReservationTracker::FilenameConflictAction conflict_action,
      const DownloadPathReservationTracker::ReservedPathCallback& callback)
      override {
    PathValidationResult result = PathValidationResult::SUCCESS;
    base::FilePath path_to_return = MockReserveVirtualPath(
        download, virtual_path, create_directory, conflict_action, &result);
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(callback, result, path_to_return));
  }

  MOCK_METHOD5(
      MockReserveVirtualPath,
      base::FilePath(download::DownloadItem*,
                     const base::FilePath&,
                     bool,
                     DownloadPathReservationTracker::FilenameConflictAction,
                     PathValidationResult*));

  // The concrete implementation invokes SafeBrowsing's
  // DownloadProtectionService. Now that SafeBrowsingService is testable, we
  // should migrate to using TestSafeBrowsingService instead.
  void CheckDownloadUrl(DownloadItem* download,
                        const base::FilePath& virtual_path,
                        const CheckDownloadUrlCallback& callback) override {
    callback.Run(MockCheckDownloadUrl(download, virtual_path));
  }

  MOCK_METHOD2(MockCheckDownloadUrl,
               download::DownloadDangerType(DownloadItem*,
                                            const base::FilePath&));

  MOCK_METHOD0(GetDownloadProtectionService,
               safe_browsing::DownloadProtectionService*());

  // The concrete implementation on desktop just invokes a file picker. Android
  // has a non-trivial implementation. The former is tested via browser tests,
  // and the latter is exercised in this unit test.
  MOCK_METHOD4(
      RequestConfirmation,
      void(DownloadItem*,
           const base::FilePath&,
           DownloadConfirmationReason,
           const DownloadTargetDeterminerDelegate::ConfirmationCallback&));

  // For testing the concrete implementation.
  void RequestConfirmationConcrete(
      DownloadItem* download_item,
      const base::FilePath& path,
      DownloadConfirmationReason reason,
      const DownloadTargetDeterminerDelegate::ConfirmationCallback& callback) {
    ChromeDownloadManagerDelegate::RequestConfirmation(download_item, path,
                                                       reason, callback);
  }
};

class ChromeDownloadManagerDelegateTest
    : public ChromeRenderViewHostTestHarness {
 public:
  // Result of calling DetermineDownloadTarget.
  ChromeDownloadManagerDelegateTest();

  // ::testing::Test
  void SetUp() override;
  void TearDown() override;

  // Verifies and clears test expectations for |delegate_| and
  // |download_manager_|.
  void VerifyAndClearExpectations();

  // Creates MockDownloadItem and sets up default expectations.
  std::unique_ptr<download::MockDownloadItem> CreateActiveDownloadItem(
      int32_t id);

  // Given the relative path |path|, returns the full path under the temporary
  // downloads directory.
  base::FilePath GetPathInDownloadDir(const char* path);

  void DetermineDownloadTarget(DownloadItem* download,
                               DetermineDownloadTargetResult* result);

  // Invokes ChromeDownloadManagerDelegate::CheckForFileExistence and waits for
  // the asynchronous callback. The result passed into
  // content::CheckForFileExistenceCallback is the return value from this
  // method.
  bool CheckForFileExistence(DownloadItem* download);

  base::FilePath GetDefaultDownloadPath() const;
  TestChromeDownloadManagerDelegate* delegate();
  content::MockDownloadManager* download_manager();
  DownloadPrefs* download_prefs();
  PrefService* pref_service();

  const std::vector<uint32_t>& download_ids() const { return download_ids_; }
  void GetNextId(uint32_t next_id) { download_ids_.emplace_back(next_id); }

 private:
  // Resets the global cached DefaultDownloadDirectory instance.
  base::ShadowingAtExitManager at_exit_manager_;
  base::ScopedPathOverride download_dir_override_{
      chrome::DIR_DEFAULT_DOWNLOADS};
  sync_preferences::TestingPrefServiceSyncable* pref_service_;
  std::unique_ptr<content::MockDownloadManager> download_manager_;
  std::unique_ptr<TestChromeDownloadManagerDelegate> delegate_;
  MockWebContentsDelegate web_contents_delegate_;
  std::vector<uint32_t> download_ids_;
};

ChromeDownloadManagerDelegateTest::ChromeDownloadManagerDelegateTest()
    : download_manager_(new ::testing::NiceMock<content::MockDownloadManager>) {
}

void ChromeDownloadManagerDelegateTest::SetUp() {
  ChromeRenderViewHostTestHarness::SetUp();

  CHECK(profile());
  delegate_ =
      std::make_unique<::testing::NiceMock<TestChromeDownloadManagerDelegate>>(
          profile());
  delegate_->SetDownloadManager(download_manager_.get());
  pref_service_ = profile()->GetTestingPrefService();
  web_contents()->SetDelegate(&web_contents_delegate_);
}

void ChromeDownloadManagerDelegateTest::TearDown() {
  base::RunLoop().RunUntilIdle();
  delegate_->Shutdown();
  ChromeRenderViewHostTestHarness::TearDown();
}

void ChromeDownloadManagerDelegateTest::VerifyAndClearExpectations() {
  ::testing::Mock::VerifyAndClearExpectations(delegate_.get());
}

std::unique_ptr<download::MockDownloadItem>
ChromeDownloadManagerDelegateTest::CreateActiveDownloadItem(int32_t id) {
  std::unique_ptr<download::MockDownloadItem> item(
      new ::testing::NiceMock<download::MockDownloadItem>());
  ON_CALL(*item, GetDangerType())
      .WillByDefault(Return(download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS));
  ON_CALL(*item, GetForcedFilePath())
      .WillByDefault(ReturnRefOfCopy(base::FilePath()));
  ON_CALL(*item, GetFullPath())
      .WillByDefault(ReturnRefOfCopy(base::FilePath()));
  ON_CALL(*item, GetHash())
      .WillByDefault(ReturnRefOfCopy(std::string()));
  ON_CALL(*item, GetId())
      .WillByDefault(Return(id));
  ON_CALL(*item, GetLastReason())
      .WillByDefault(Return(download::DOWNLOAD_INTERRUPT_REASON_NONE));
  ON_CALL(*item, GetReferrerUrl())
      .WillByDefault(ReturnRefOfCopy(GURL()));
  ON_CALL(*item, GetState())
      .WillByDefault(Return(DownloadItem::IN_PROGRESS));
  ON_CALL(*item, GetTargetFilePath())
      .WillByDefault(ReturnRefOfCopy(base::FilePath()));
  ON_CALL(*item, GetTransitionType())
      .WillByDefault(Return(ui::PAGE_TRANSITION_LINK));
  ON_CALL(*item, HasUserGesture())
      .WillByDefault(Return(false));
  ON_CALL(*item, IsDangerous())
      .WillByDefault(Return(false));
  ON_CALL(*item, IsTemporary())
      .WillByDefault(Return(false));
  content::DownloadItemUtils::AttachInfo(item.get(), profile(), web_contents());
  EXPECT_CALL(*download_manager_, GetDownload(id))
      .WillRepeatedly(Return(item.get()));
  return item;
}

base::FilePath ChromeDownloadManagerDelegateTest::GetPathInDownloadDir(
    const char* relative_path) {
  base::FilePath full_path =
      GetDefaultDownloadPath().AppendASCII(relative_path);
  return full_path.NormalizePathSeparators();
}

void StoreDownloadTargetInfo(
    const base::Closure& closure,
    DetermineDownloadTargetResult* result,
    const base::FilePath& target_path,
    DownloadItem::TargetDisposition target_disposition,
    download::DownloadDangerType danger_type,
    const base::FilePath& intermediate_path,
    download::DownloadInterruptReason interrupt_reason) {
  result->target_path = target_path;
  result->disposition = target_disposition;
  result->danger_type = danger_type;
  result->intermediate_path = intermediate_path;
  result->interrupt_reason = interrupt_reason;
  closure.Run();
}

void ChromeDownloadManagerDelegateTest::DetermineDownloadTarget(
    DownloadItem* download_item,
    DetermineDownloadTargetResult* result) {
  base::RunLoop loop_runner;
  delegate()->DetermineDownloadTarget(
      download_item,
      base::Bind(&StoreDownloadTargetInfo, loop_runner.QuitClosure(), result));
  loop_runner.Run();
}

void StoreBoolAndRunClosure(const base::Closure& closure,
                            bool* result_storage,
                            bool result) {
  *result_storage = result;
  closure.Run();
}

bool ChromeDownloadManagerDelegateTest::CheckForFileExistence(
    DownloadItem* download_item) {
  base::RunLoop loop_runner;
  bool result = false;
  delegate()->CheckForFileExistence(
      download_item, base::BindOnce(&StoreBoolAndRunClosure,
                                    loop_runner.QuitClosure(), &result));
  loop_runner.Run();
  return result;
}

base::FilePath ChromeDownloadManagerDelegateTest::GetDefaultDownloadPath()
    const {
  base::FilePath path;
  CHECK(base::PathService::Get(chrome::DIR_DEFAULT_DOWNLOADS, &path));
  return path;
}

TestChromeDownloadManagerDelegate*
    ChromeDownloadManagerDelegateTest::delegate() {
  return delegate_.get();
}

content::MockDownloadManager*
    ChromeDownloadManagerDelegateTest::download_manager() {
  return download_manager_.get();
}

DownloadPrefs* ChromeDownloadManagerDelegateTest::download_prefs() {
  return delegate_->download_prefs();
}

PrefService* ChromeDownloadManagerDelegateTest::pref_service() {
  return pref_service_;
}

}  // namespace

TEST_F(ChromeDownloadManagerDelegateTest, LastSavePath) {
  GURL download_url("http://example.com/foo.txt");

  std::unique_ptr<download::MockDownloadItem> save_as_download =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*save_as_download, GetURL())
      .Times(AnyNumber())
      .WillRepeatedly(ReturnRef(download_url));
  EXPECT_CALL(*save_as_download, GetTargetDisposition())
      .Times(AnyNumber())
      .WillRepeatedly(Return(DownloadItem::TARGET_DISPOSITION_PROMPT));

  std::unique_ptr<download::MockDownloadItem> automatic_download =
      CreateActiveDownloadItem(1);
  EXPECT_CALL(*automatic_download, GetURL())
      .Times(AnyNumber())
      .WillRepeatedly(ReturnRef(download_url));
  EXPECT_CALL(*automatic_download, GetTargetDisposition())
      .Times(AnyNumber())
      .WillRepeatedly(Return(DownloadItem::TARGET_DISPOSITION_OVERWRITE));

  {
    // When the prompt is displayed for the first download, the user selects a
    // path in a different directory.
    DetermineDownloadTargetResult result;
    base::FilePath expected_prompt_path(GetPathInDownloadDir("foo.txt"));
    base::FilePath user_selected_path(GetPathInDownloadDir("bar/baz.txt"));
    EXPECT_CALL(*delegate(), RequestConfirmation(save_as_download.get(),
                                                 expected_prompt_path, _, _))
        .WillOnce(WithArg<3>(ScheduleCallback2(
            DownloadConfirmationResult::CONFIRMED, user_selected_path)));
    DetermineDownloadTarget(save_as_download.get(), &result);
    EXPECT_EQ(user_selected_path, result.target_path);
    VerifyAndClearExpectations();
  }

  {
    // The prompt path for the second download is the user selected directory
    // from the previous download.
    DetermineDownloadTargetResult result;
    base::FilePath expected_prompt_path(GetPathInDownloadDir("bar/foo.txt"));
    EXPECT_CALL(*delegate(), RequestConfirmation(save_as_download.get(),
                                                 expected_prompt_path, _, _))
        .WillOnce(WithArg<3>(ScheduleCallback2(
            DownloadConfirmationResult::CANCELED, base::FilePath())));
    DetermineDownloadTarget(save_as_download.get(), &result);
    VerifyAndClearExpectations();
  }

  {
    // Start an automatic download. This one should get the default download
    // path since the last download path only affects Save As downloads.
    DetermineDownloadTargetResult result;
    base::FilePath expected_path(GetPathInDownloadDir("foo.txt"));
    DetermineDownloadTarget(automatic_download.get(), &result);
    EXPECT_EQ(expected_path, result.target_path);
    VerifyAndClearExpectations();
  }

  {
    // The prompt path for the next download should be the default.
    download_prefs()->SetSaveFilePath(download_prefs()->DownloadPath());
    DetermineDownloadTargetResult result;
    base::FilePath expected_prompt_path(GetPathInDownloadDir("foo.txt"));
    EXPECT_CALL(*delegate(), RequestConfirmation(save_as_download.get(),
                                                 expected_prompt_path, _, _))
        .WillOnce(WithArg<3>(ScheduleCallback2(
            DownloadConfirmationResult::CANCELED, base::FilePath())));
    DetermineDownloadTarget(save_as_download.get(), &result);
    VerifyAndClearExpectations();
  }
}

TEST_F(ChromeDownloadManagerDelegateTest, ConflictAction) {
  const GURL kUrl("http://example.com/foo");
  const std::string kTargetDisposition("attachment; filename=\"foo.txt\"");

  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(kUrl));
  EXPECT_CALL(*download_item, GetContentDisposition())
      .WillRepeatedly(Return(kTargetDisposition));

  base::FilePath kExpectedPath = GetPathInDownloadDir("bar.txt");

  DetermineDownloadTargetResult result;

  EXPECT_CALL(*delegate(), MockReserveVirtualPath(_, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<4>(PathValidationResult::CONFLICT),
                      ReturnArg<1>()));
  EXPECT_CALL(
      *delegate(),
      RequestConfirmation(_, _, DownloadConfirmationReason::TARGET_CONFLICT, _))
      .WillOnce(WithArg<3>(ScheduleCallback2(
          DownloadConfirmationResult::CONFIRMED, kExpectedPath)));
  DetermineDownloadTarget(download_item.get(), &result);
  EXPECT_EQ(download::DownloadItem::TARGET_DISPOSITION_PROMPT,
            result.disposition);
  EXPECT_EQ(kExpectedPath, result.target_path);

  VerifyAndClearExpectations();
}

TEST_F(ChromeDownloadManagerDelegateTest, MaybeDangerousContent) {
#if BUILDFLAG(ENABLE_PLUGINS)
  content::PluginService::GetInstance()->Init();
#endif

  GURL url("http://example.com/foo");

  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(url));
  EXPECT_CALL(*download_item, GetTargetDisposition())
      .WillRepeatedly(Return(DownloadItem::TARGET_DISPOSITION_OVERWRITE));
  EXPECT_CALL(*delegate(), MockCheckDownloadUrl(_, _))
      .WillRepeatedly(
          Return(download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT));

  {
    const std::string kDangerousContentDisposition(
        "attachment; filename=\"foo.swf\"");
    EXPECT_CALL(*download_item, GetContentDisposition())
        .WillRepeatedly(Return(kDangerousContentDisposition));
    DetermineDownloadTargetResult result;
    DetermineDownloadTarget(download_item.get(), &result);

    EXPECT_EQ(DownloadFileType::DANGEROUS,
              DownloadItemModel(download_item.get()).GetDangerLevel());
    EXPECT_EQ(download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
              result.danger_type);
  }

  {
    const std::string kSafeContentDisposition(
        "attachment; filename=\"foo.txt\"");
    EXPECT_CALL(*download_item, GetContentDisposition())
        .WillRepeatedly(Return(kSafeContentDisposition));
    DetermineDownloadTargetResult result;
    DetermineDownloadTarget(download_item.get(), &result);
    EXPECT_EQ(DownloadFileType::NOT_DANGEROUS,
              DownloadItemModel(download_item.get()).GetDangerLevel());
    EXPECT_EQ(download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
              result.danger_type);
  }

  {
    const std::string kModerateContentDisposition(
        "attachment; filename=\"foo.crx\"");
    EXPECT_CALL(*download_item, GetContentDisposition())
        .WillRepeatedly(Return(kModerateContentDisposition));
    DetermineDownloadTargetResult result;
    DetermineDownloadTarget(download_item.get(), &result);
    EXPECT_EQ(DownloadFileType::ALLOW_ON_USER_GESTURE,
              DownloadItemModel(download_item.get()).GetDangerLevel());
    EXPECT_EQ(download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
              result.danger_type);
  }
}

TEST_F(ChromeDownloadManagerDelegateTest, CheckForFileExistence) {
  const char kData[] = "helloworld";
  const size_t kDataLength = sizeof(kData) - 1;
  base::FilePath existing_path = GetDefaultDownloadPath().AppendASCII("foo");
  base::FilePath non_existent_path =
      GetDefaultDownloadPath().AppendASCII("bar");
  base::WriteFile(existing_path, kData, kDataLength);

  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(1);
  EXPECT_CALL(*download_item, GetTargetFilePath())
      .WillRepeatedly(ReturnRef(existing_path));
  EXPECT_TRUE(CheckForFileExistence(download_item.get()));

  download_item = CreateActiveDownloadItem(1);
  EXPECT_CALL(*download_item, GetTargetFilePath())
      .WillRepeatedly(ReturnRef(non_existent_path));
  EXPECT_FALSE(CheckForFileExistence(download_item.get()));
}

TEST_F(ChromeDownloadManagerDelegateTest, BlockedByPolicy) {
  const GURL kUrl("http://example.com/foo");
  const std::string kTargetDisposition("attachment; filename=\"foo.txt\"");

  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(kUrl));
  EXPECT_CALL(*download_item, GetContentDisposition())
      .WillRepeatedly(Return(kTargetDisposition));

  base::FilePath kExpectedPath = GetPathInDownloadDir("bar.txt");

  DetermineDownloadTargetResult result;

  EXPECT_CALL(*delegate(), MockReserveVirtualPath(_, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<4>(PathValidationResult::CONFLICT),
                      ReturnArg<1>()));
  EXPECT_CALL(
      *delegate(),
      RequestConfirmation(_, _, DownloadConfirmationReason::TARGET_CONFLICT, _))
      .WillOnce(WithArg<3>(ScheduleCallback2(
          DownloadConfirmationResult::CONFIRMED, kExpectedPath)));

  pref_service()->SetInteger(
      prefs::kDownloadRestrictions,
      static_cast<int>(DownloadPrefs::DownloadRestriction::ALL_FILES));

  DetermineDownloadTarget(download_item.get(), &result);
  EXPECT_EQ(download::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED,
            result.interrupt_reason);

  VerifyAndClearExpectations();
}

TEST_F(ChromeDownloadManagerDelegateTest, WithoutHistoryDbNextId) {
  content::DownloadIdCallback id_callback = base::Bind(
      &ChromeDownloadManagerDelegateTest::GetNextId, base::Unretained(this));
  delegate()->GetNextId(id_callback);
  delegate()->GetNextId(id_callback);
  // When download database fails to initialize, id will be set to
  // |download::DownloadItem::kInvalidId|.
  delegate()->GetDownloadIdReceiverCallback().Run(
      download::DownloadItem::kInvalidId);
  std::vector<uint32_t> expected_ids = std::vector<uint32_t>{1u, 2u};
  EXPECT_EQ(expected_ids, download_ids());
}

TEST_F(ChromeDownloadManagerDelegateTest, WithHistoryDbNextId) {
  content::DownloadIdCallback id_callback = base::Bind(
      &ChromeDownloadManagerDelegateTest::GetNextId, base::Unretained(this));
  delegate()->GetNextId(id_callback);
  delegate()->GetNextId(id_callback);
  // Simulates a valid download database with no records.
  delegate()->GetDownloadIdReceiverCallback().Run(1u);
  std::vector<uint32_t> expected_ids = std::vector<uint32_t>{1u, 2u};
  EXPECT_EQ(expected_ids, download_ids());
}

#if defined(FULL_SAFE_BROWSING)
namespace {

struct SafeBrowsingTestParameters {
  download::DownloadDangerType initial_danger_type;
  DownloadFileType::DangerLevel initial_danger_level;
  safe_browsing::DownloadCheckResult verdict;
  DownloadPrefs::DownloadRestriction download_restriction;

  download::DownloadDangerType expected_danger_type;
  bool blocked;
};

class TestDownloadProtectionService
    : public safe_browsing::DownloadProtectionService {
 public:
  TestDownloadProtectionService() : DownloadProtectionService(nullptr) {}

  void CheckClientDownload(
      DownloadItem* download_item,
      const safe_browsing::CheckDownloadCallback& callback) override {
    callback.Run(MockCheckClientDownload());
  }
  MOCK_METHOD0(MockCheckClientDownload, safe_browsing::DownloadCheckResult());
};

class ChromeDownloadManagerDelegateTestWithSafeBrowsing
    : public ChromeDownloadManagerDelegateTest,
      public ::testing::WithParamInterface<SafeBrowsingTestParameters> {
 public:
  void SetUp() override;
  void TearDown() override;
  TestDownloadProtectionService* download_protection_service() {
    return test_download_protection_service_.get();
  }

 private:
  std::unique_ptr<TestDownloadProtectionService>
      test_download_protection_service_;
};

void ChromeDownloadManagerDelegateTestWithSafeBrowsing::SetUp() {
  ChromeDownloadManagerDelegateTest::SetUp();
  test_download_protection_service_.reset(
      new ::testing::StrictMock<TestDownloadProtectionService>);
  ON_CALL(*delegate(), GetDownloadProtectionService())
      .WillByDefault(Return(test_download_protection_service_.get()));
}

void ChromeDownloadManagerDelegateTestWithSafeBrowsing::TearDown() {
  test_download_protection_service_.reset();
  ChromeDownloadManagerDelegateTest::TearDown();
}

const SafeBrowsingTestParameters kSafeBrowsingTestCases[] = {
    // SAFE verdict for a safe file.
    {download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     DownloadFileType::NOT_DANGEROUS, safe_browsing::DownloadCheckResult::SAFE,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     /*blocked=*/false},

    // UNKNOWN verdict for a safe file.
    {download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     DownloadFileType::NOT_DANGEROUS,
     safe_browsing::DownloadCheckResult::UNKNOWN,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     /*blocked=*/false},

    // DANGEROUS verdict for a safe file.
    {download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     DownloadFileType::NOT_DANGEROUS,
     safe_browsing::DownloadCheckResult::DANGEROUS,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT,
     /*blocked=*/false},

    // UNCOMMON verdict for a safe file.
    {download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     DownloadFileType::NOT_DANGEROUS,
     safe_browsing::DownloadCheckResult::UNCOMMON,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT,
     /*blocked=*/false},

    // POTENTIALLY_UNWANTED verdict for a safe file.
    {download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     DownloadFileType::NOT_DANGEROUS,
     safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED,
     /*blocked=*/false},

    // SAFE verdict for a potentially dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::SAFE,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
     /*blocked=*/false},

    // UNKNOWN verdict for a potentially dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::UNKNOWN,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE,
     /*blocked=*/false},

    // UNKNOWN verdict for a potentially dangerous file blocked by policy.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::UNKNOWN,
     DownloadPrefs::DownloadRestriction::DANGEROUS_FILES,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE,
     /*blocked=*/true},

    // DANGEROUS verdict for a potentially dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::DANGEROUS,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT,
     /*blocked=*/false},

    // UNCOMMON verdict for a potentially dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::UNCOMMON,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT,
     /*blocked=*/false},

    // POTENTIALLY_UNWANTED verdict for a potentially dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED,
     /*blocked=*/false},

    // POTENTIALLY_UNWANTED verdict for a potentially dangerous file, blocked by
    // policy.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED,
     DownloadPrefs::DownloadRestriction::POTENTIALLY_DANGEROUS_FILES,

     download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED,
     /*blocked=*/true},

    // POTENTIALLY_UNWANTED verdict for a potentially dangerous file, not
    // blocked by policy.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::ALLOW_ON_USER_GESTURE,
     safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED,
     DownloadPrefs::DownloadRestriction::DANGEROUS_FILES,

     download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED,
     /*blocked=*/false},

    // SAFE verdict for a dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::DANGEROUS, safe_browsing::DownloadCheckResult::SAFE,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE,
     /*blocked=*/false},

    // UNKNOWN verdict for a dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::DANGEROUS, safe_browsing::DownloadCheckResult::UNKNOWN,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE,
     /*blocked=*/false},

    // DANGEROUS verdict for a dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::DANGEROUS, safe_browsing::DownloadCheckResult::DANGEROUS,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT,
     /*blocked=*/false},

    // UNCOMMON verdict for a dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::DANGEROUS, safe_browsing::DownloadCheckResult::UNCOMMON,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT,
     /*blocked=*/false},

    // POTENTIALLY_UNWANTED verdict for a dangerous file.
    {download::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT,
     DownloadFileType::DANGEROUS,
     safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED,
     DownloadPrefs::DownloadRestriction::NONE,

     download::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED,
     /*blocked=*/false},
};

INSTANTIATE_TEST_CASE_P(_,
                        ChromeDownloadManagerDelegateTestWithSafeBrowsing,
                        ::testing::ValuesIn(kSafeBrowsingTestCases));

}  // namespace

TEST_P(ChromeDownloadManagerDelegateTestWithSafeBrowsing, CheckClientDownload) {
  const SafeBrowsingTestParameters& kParameters = GetParam();

  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*delegate(), GetDownloadProtectionService());
  EXPECT_CALL(*download_protection_service(), MockCheckClientDownload())
      .WillOnce(Return(kParameters.verdict));
  EXPECT_CALL(*download_item, GetDangerType())
      .WillRepeatedly(Return(kParameters.initial_danger_type));

  if (kParameters.initial_danger_level != DownloadFileType::NOT_DANGEROUS) {
    DownloadItemModel(download_item.get())
        .SetDangerLevel(kParameters.initial_danger_level);
  }

  if (kParameters.expected_danger_type !=
      download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS) {
    if (kParameters.blocked) {
      EXPECT_CALL(*download_item,
                  OnContentCheckCompleted(
                      // Specifying a dangerous type here would take precendence
                      // over the blocking of the file.
                      download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS,
                      download::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED));
    } else {
      EXPECT_CALL(
          *download_item,
          OnContentCheckCompleted(kParameters.expected_danger_type,
                                  download::DOWNLOAD_INTERRUPT_REASON_NONE));
    }
  } else {
    EXPECT_CALL(*download_item, OnContentCheckCompleted(_, _)).Times(0);
  }

  pref_service()->SetInteger(
      prefs::kDownloadRestrictions,
      static_cast<int>(kParameters.download_restriction));

  base::RunLoop run_loop;
  ASSERT_FALSE(delegate()->ShouldCompleteDownload(download_item.get(),
                                                  run_loop.QuitClosure()));
  run_loop.Run();
}

TEST_F(ChromeDownloadManagerDelegateTestWithSafeBrowsing,
       TrustedSourcesPolicyNotTrusted) {
  GURL download_url("http://untrusted.com/best-download-ever.exe");
  pref_service()->SetBoolean(prefs::kSafeBrowsingForTrustedSourcesEnabled,
                             false);
  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(download_url));

  EXPECT_CALL(*delegate(), GetDownloadProtectionService());
  EXPECT_CALL(*download_protection_service(), MockCheckClientDownload())
      .WillOnce(Return(safe_browsing::DownloadCheckResult::SAFE));
  EXPECT_CALL(*download_item, GetDangerType())
      .WillRepeatedly(Return(download::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS));

  base::RunLoop run_loop;
  ASSERT_FALSE(delegate()->ShouldCompleteDownload(download_item.get(),
                                                  run_loop.QuitClosure()));
  run_loop.Run();
}

#if !defined(OS_WIN)
// TODO(crbug.com/739204) Add a Windows version of this test.
TEST_F(ChromeDownloadManagerDelegateTestWithSafeBrowsing,
       TrustedSourcesPolicyTrusted) {
  base::CommandLine* command_line(base::CommandLine::ForCurrentProcess());
  DCHECK(command_line);
  command_line->AppendSwitchASCII(switches::kTrustedDownloadSources,
                                  "trusted.com");
  GURL download_url("http://trusted.com/best-download-ever.exe");
  pref_service()->SetBoolean(prefs::kSafeBrowsingForTrustedSourcesEnabled,
                             false);
  std::unique_ptr<download::MockDownloadItem> download_item =
      CreateActiveDownloadItem(0);
  EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(download_url));
  EXPECT_CALL(*delegate(), GetDownloadProtectionService()).Times(0);
  EXPECT_TRUE(
      delegate()->ShouldCompleteDownload(download_item.get(), base::Closure()));
}
#endif  // OS_WIN
#endif  // FULL_SAFE_BROWSING

#if defined(OS_ANDROID)

namespace {

class AndroidDownloadInfobarCounter
    : public infobars::InfoBarManager::Observer {
 public:
  explicit AndroidDownloadInfobarCounter(content::WebContents* web_contents)
      : infobar_service_(InfoBarService::FromWebContents(web_contents)) {
    infobar_service_->AddObserver(this);
  }

  ~AndroidDownloadInfobarCounter() override {
    infobar_service_->RemoveObserver(this);
  }

  int CheckAndResetInfobarCount() {
    int count = infobar_count_;
    infobar_count_ = 0;
    return count;
  }

 private:
  void OnInfoBarAdded(infobars::InfoBar* infobar) override {
    if (infobar->delegate()->GetIdentifier() ==
        infobars::InfoBarDelegate::DUPLICATE_DOWNLOAD_INFOBAR_DELEGATE_ANDROID)
      ++infobar_count_;
    infobar->delegate()->InfoBarDismissed();
    infobar->RemoveSelf();
  }

  InfoBarService* infobar_service_;
  int infobar_count_ = 0;
};

class TestDownloadLocationDialogBridge : public DownloadLocationDialogBridge {
 public:
  TestDownloadLocationDialogBridge() {}

  // DownloadLocationDialogBridge implementation.
  void ShowDialog(gfx::NativeWindow native_window,
                  DownloadLocationDialogType dialog_type,
                  const base::FilePath& suggested_path,
                  const DownloadTargetDeterminerDelegate::ConfirmationCallback&
                      callback) override {
    dialog_shown_count_++;
    dialog_type_ = dialog_type;
    callback.Run(DownloadConfirmationResult::CANCELED, base::FilePath());
  }

  void OnComplete(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj,
      const base::android::JavaParamRef<jstring>& returned_path) override {}

  void OnCanceled(JNIEnv* env,
                  const base::android::JavaParamRef<jobject>& obj) override {}

  // Returns the number of times ShowDialog has been called.
  int GetDialogShownCount() { return dialog_shown_count_; }

  // Returns the type of the last dialog that was called to be shown.
  DownloadLocationDialogType GetDialogType() { return dialog_type_; }

  // Resets the stored information.
  void ResetStoredVariables() {
    dialog_shown_count_ = 0;
    dialog_type_ = DownloadLocationDialogType::NO_DIALOG;
  }

 private:
  int dialog_shown_count_;
  DownloadLocationDialogType dialog_type_;
  DownloadTargetDeterminerDelegate::ConfirmationCallback
      dialog_complete_callback_;

  DISALLOW_COPY_AND_ASSIGN(TestDownloadLocationDialogBridge);
};

}  // namespace

TEST_F(ChromeDownloadManagerDelegateTest, RequestConfirmation_Android) {
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(features::kDownloadsLocationChange));

  enum class WebContents { AVAILABLE, NONE };
  enum class ExpectPath { FULL, EMPTY };
  enum class ExpectInfoBar { YES, NO };
  struct {
    DownloadConfirmationReason confirmation_reason;
    DownloadConfirmationResult expected_result;
    WebContents web_contents;
    ExpectInfoBar info_bar;
    ExpectPath path;
  } kTestCases[] = {
      {DownloadConfirmationReason::TARGET_PATH_NOT_WRITEABLE,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       ExpectInfoBar::NO, ExpectPath::EMPTY},

      {DownloadConfirmationReason::NAME_TOO_LONG,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::AVAILABLE, ExpectInfoBar::NO, ExpectPath::FULL},

      {DownloadConfirmationReason::TARGET_NO_SPACE,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::AVAILABLE, ExpectInfoBar::NO, ExpectPath::FULL},

      {DownloadConfirmationReason::SAVE_AS,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::AVAILABLE, ExpectInfoBar::NO, ExpectPath::FULL},

      // This case results in an infobar. The logic above dismisses the infobar
      // and counts it for testing. The functionality of the infobar is not
      // tested here other than that dimssing the infobar is treated as a user
      // initiated cancellation.
      {DownloadConfirmationReason::TARGET_CONFLICT,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       ExpectInfoBar::YES, ExpectPath::EMPTY},

      {DownloadConfirmationReason::TARGET_CONFLICT,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       ExpectInfoBar::NO, ExpectPath::EMPTY},

      {DownloadConfirmationReason::UNEXPECTED,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       ExpectInfoBar::NO, ExpectPath::EMPTY},
  };

  EXPECT_CALL(*delegate(), RequestConfirmation(_, _, _, _))
      .WillRepeatedly(Invoke(
          delegate(),
          &TestChromeDownloadManagerDelegate::RequestConfirmationConcrete));
  InfoBarService::CreateForWebContents(web_contents());
  base::FilePath fake_path = GetPathInDownloadDir(FILE_PATH_LITERAL("foo.txt"));
  GURL url("http://example.com");
  AndroidDownloadInfobarCounter infobar_counter(web_contents());

  for (const auto& test_case : kTestCases) {
    std::unique_ptr<download::MockDownloadItem> download_item =
        CreateActiveDownloadItem(1);
    content::DownloadItemUtils::AttachInfo(
        download_item.get(), profile(),
        test_case.web_contents == WebContents::AVAILABLE ? web_contents()
                                                         : nullptr);
    EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(url));
    infobar_counter.CheckAndResetInfobarCount();

    base::RunLoop loop;
    const auto callback = base::Bind(
        [](const base::Closure& quit_closure,
           DownloadConfirmationResult expected_result,
           const base::FilePath& expected_path,
           DownloadConfirmationResult actual_result,
           const base::FilePath& actual_path) {
          EXPECT_EQ(expected_result, actual_result);
          EXPECT_EQ(expected_path, actual_path);
          quit_closure.Run();
        },
        loop.QuitClosure(), test_case.expected_result,
        test_case.path == ExpectPath::FULL ? fake_path : base::FilePath());
    delegate()->RequestConfirmation(download_item.get(), fake_path,
                                    test_case.confirmation_reason, callback);
    loop.Run();

    EXPECT_EQ(test_case.info_bar == ExpectInfoBar::YES ? 1 : 0,
              infobar_counter.CheckAndResetInfobarCount());
  }
}

TEST_F(ChromeDownloadManagerDelegateTest,
       RequestConfirmation_Android_WithLocationChangeEnabled) {
  DeleteContents();
  SetContents(CreateTestWebContents());

  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kDownloadsLocationChange);
  EXPECT_TRUE(base::FeatureList::IsEnabled(features::kDownloadsLocationChange));

  enum class WebContents { AVAILABLE, NONE };
  enum class ExpectPath { FULL, EMPTY };
  struct {
    DownloadConfirmationReason confirmation_reason;
    DownloadConfirmationResult expected_result;
    WebContents web_contents;
    DownloadLocationDialogType dialog_type;
    ExpectPath path;
  } kTestCases[] = {
      // SAVE_AS
      {DownloadConfirmationReason::SAVE_AS,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::AVAILABLE, DownloadLocationDialogType::NO_DIALOG,
       ExpectPath::FULL},
      {DownloadConfirmationReason::SAVE_AS,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::NONE, DownloadLocationDialogType::NO_DIALOG,
       ExpectPath::FULL},

      // !web_contents
      {DownloadConfirmationReason::PREFERENCE,
       DownloadConfirmationResult::CONTINUE_WITHOUT_CONFIRMATION,
       WebContents::NONE, DownloadLocationDialogType::NO_DIALOG,
       ExpectPath::FULL},
      {DownloadConfirmationReason::TARGET_CONFLICT,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},
      {DownloadConfirmationReason::TARGET_NO_SPACE,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},
      {DownloadConfirmationReason::TARGET_PATH_NOT_WRITEABLE,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},
      {DownloadConfirmationReason::NAME_TOO_LONG,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},

      // UNEXPECTED
      {DownloadConfirmationReason::UNEXPECTED,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},
      {DownloadConfirmationReason::UNEXPECTED,
       DownloadConfirmationResult::CANCELED, WebContents::NONE,
       DownloadLocationDialogType::NO_DIALOG, ExpectPath::EMPTY},

      // TARGET_CONFLICT
      {DownloadConfirmationReason::TARGET_CONFLICT,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       DownloadLocationDialogType::NAME_CONFLICT, ExpectPath::EMPTY},

      // Other error dialogs
      {DownloadConfirmationReason::TARGET_NO_SPACE,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       DownloadLocationDialogType::LOCATION_FULL, ExpectPath::EMPTY},
      {DownloadConfirmationReason::TARGET_PATH_NOT_WRITEABLE,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       DownloadLocationDialogType::LOCATION_NOT_FOUND, ExpectPath::EMPTY},
      {DownloadConfirmationReason::NAME_TOO_LONG,
       DownloadConfirmationResult::CANCELED, WebContents::AVAILABLE,
       DownloadLocationDialogType::NAME_TOO_LONG, ExpectPath::EMPTY},
  };

  EXPECT_CALL(*delegate(), RequestConfirmation(_, _, _, _))
      .WillRepeatedly(Invoke(
          delegate(),
          &TestChromeDownloadManagerDelegate::RequestConfirmationConcrete));
  base::FilePath fake_path = GetPathInDownloadDir(FILE_PATH_LITERAL("foo.txt"));
  GURL url("http://example.com");
  TestDownloadLocationDialogBridge* location_dialog_bridge =
      new TestDownloadLocationDialogBridge();
  delegate()->SetDownloadLocationDialogBridgeForTesting(
      static_cast<DownloadLocationDialogBridge*>(location_dialog_bridge));

  for (const auto& test_case : kTestCases) {
    std::unique_ptr<download::MockDownloadItem> download_item =
        CreateActiveDownloadItem(1);
    content::DownloadItemUtils::AttachInfo(
        download_item.get(), profile(),
        test_case.web_contents == WebContents::AVAILABLE ? web_contents()
                                                         : nullptr);
    EXPECT_CALL(*download_item, GetURL()).WillRepeatedly(ReturnRef(url));
    location_dialog_bridge->ResetStoredVariables();

    base::RunLoop loop;
    const auto callback = base::BindRepeating(
        [](const base::RepeatingClosure& quit_closure,
           DownloadConfirmationResult expected_result,
           const base::FilePath& expected_path,
           DownloadConfirmationResult actual_result,
           const base::FilePath& actual_path) {
          EXPECT_EQ(expected_result, actual_result);
          EXPECT_EQ(expected_path, actual_path);
          quit_closure.Run();
        },
        loop.QuitClosure(), test_case.expected_result,
        test_case.path == ExpectPath::FULL ? fake_path : base::FilePath());
    delegate()->RequestConfirmation(download_item.get(), fake_path,
                                    test_case.confirmation_reason, callback);
    loop.Run();

    EXPECT_EQ(
        test_case.dialog_type != DownloadLocationDialogType::NO_DIALOG ? 1 : 0,
        location_dialog_bridge->GetDialogShownCount());
    EXPECT_EQ(test_case.dialog_type, location_dialog_bridge->GetDialogType());

    EXPECT_CALL(*download_item, GetState())
        .WillRepeatedly(Return(DownloadItem::COMPLETE));
    download_item->NotifyObserversDownloadUpdated();
  }
}
#endif  // OS_ANDROID
