// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/net_log/net_export_file_writer.h"

#include <stdint.h>

#include <memory>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_string_value_serializer.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/scoped_task_environment.h"
#include "base/values.h"
#include "build/build_config.h"
#include "net/base/test_completion_callback.h"
#include "net/log/net_log_capture_mode.h"
#include "net/log/net_log_event_type.h"
#include "net/test/embedded_test_server/default_handlers.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

const char kChannelString[] = "SomeChannel";
const size_t kMaxLogSizeBytes = 100 * 1024 * 1024;  // 100MiB

// Keep this in sync with kLogRelativePath defined in net_export_file_writer.cc.
base::FilePath::CharType kLogRelativePath[] =
    FILE_PATH_LITERAL("net-export/chrome-net-export-log.json");

const char kCaptureModeDefaultString[] = "STRIP_PRIVATE_DATA";
const char kCaptureModeIncludeCookiesAndCredentialsString[] = "NORMAL";
const char kCaptureModeIncludeSocketBytesString[] = "LOG_BYTES";

const char kStateUninitializedString[] = "UNINITIALIZED";
const char kStateInitializingString[] = "INITIALIZING";
const char kStateNotLoggingString[] = "NOT_LOGGING";
const char kStateStartingLogString[] = "STARTING_LOG";
const char kStateLoggingString[] = "LOGGING";
const char kStateStoppingLogString[] = "STOPPING_LOG";
}  // namespace

namespace net_log {

// Sets |path| to |path_to_return| and always returns true. This function is
// used to override NetExportFileWriter's usual getter for the default log base
// directory.
bool SetPathToGivenAndReturnTrue(const base::FilePath& path_to_return,
                                 base::FilePath* path) {
  *path = path_to_return;
  return true;
}

// Checks the "state" string of a NetExportFileWriter state.
WARN_UNUSED_RESULT ::testing::AssertionResult VerifyState(
    std::unique_ptr<base::DictionaryValue> state,
    const std::string& expected_state_string) {
  std::string actual_state_string;
  if (!state->GetString("state", &actual_state_string)) {
    return ::testing::AssertionFailure()
           << "State is missing \"state\" string.";
  }
  if (actual_state_string != expected_state_string) {
    return ::testing::AssertionFailure()
           << "\"state\" string of state does not match expected." << std::endl
           << "    Actual: " << actual_state_string << std::endl
           << "  Expected: " << expected_state_string;
  }
  return ::testing::AssertionSuccess();
}

// Checks all fields of a NetExportFileWriter state except possibly the
// "captureMode" string; that field is only checked if
// |expected_log_capture_mode_known| is true.
WARN_UNUSED_RESULT ::testing::AssertionResult VerifyState(
    std::unique_ptr<base::DictionaryValue> state,
    const std::string& expected_state_string,
    bool expected_log_exists,
    bool expected_log_capture_mode_known,
    const std::string& expected_log_capture_mode_string) {
  base::DictionaryValue expected_state;
  expected_state.SetString("state", expected_state_string);
  expected_state.SetBoolean("logExists", expected_log_exists);
  expected_state.SetBoolean("logCaptureModeKnown",
                            expected_log_capture_mode_known);
  if (expected_log_capture_mode_known) {
    expected_state.SetString("captureMode", expected_log_capture_mode_string);
  } else {
    state->Remove("captureMode", nullptr);
  }

  // Remove "file" field which is only added in debug mode.
  state->Remove("file", nullptr);

  std::string expected_state_json_string;
  JSONStringValueSerializer expected_state_serializer(
      &expected_state_json_string);
  expected_state_serializer.Serialize(expected_state);

  std::string actual_state_json_string;
  JSONStringValueSerializer actual_state_serializer(&actual_state_json_string);
  actual_state_serializer.Serialize(*state);

  if (actual_state_json_string != expected_state_json_string) {
    return ::testing::AssertionFailure()
           << "State (possibly excluding \"captureMode\") does not match "
              "expected."
           << std::endl
           << "    Actual: " << actual_state_json_string << std::endl
           << "  Expected: " << expected_state_json_string;
  }
  return ::testing::AssertionSuccess();
}

WARN_UNUSED_RESULT ::testing::AssertionResult ReadCompleteLogFile(
    const base::FilePath& log_path,
    std::unique_ptr<base::DictionaryValue>* root) {
  DCHECK(!log_path.empty());

  if (!base::PathExists(log_path)) {
    return ::testing::AssertionFailure()
           << log_path.value() << " does not exist.";
  }
  // Parse log file contents into a dictionary
  std::string log_string;
  if (!base::ReadFileToString(log_path, &log_string)) {
    return ::testing::AssertionFailure()
           << log_path.value() << " could not be read.";
  }
  *root = base::DictionaryValue::From(base::JSONReader::Read(log_string));
  if (!*root) {
    return ::testing::AssertionFailure()
           << "Contents of " << log_path.value()
           << " do not form valid JSON dictionary.";
  }
  // Make sure the "constants" section exists
  base::DictionaryValue* constants;
  if (!(*root)->GetDictionary("constants", &constants)) {
    root->reset();
    return ::testing::AssertionFailure()
           << log_path.value() << " is missing constants.";
  }
  // Make sure the "events" section exists
  base::ListValue* events;
  if (!(*root)->GetList("events", &events)) {
    root->reset();
    return ::testing::AssertionFailure()
           << log_path.value() << " is missing events list.";
  }
  return ::testing::AssertionSuccess();
}

// An implementation of NetExportFileWriter::StateObserver that allows waiting
// until it's notified of a new state.
class TestStateObserver : public NetExportFileWriter::StateObserver {
 public:
  // NetExportFileWriter::StateObserver implementation
  void OnNewState(const base::DictionaryValue& state) override {
    test_closure_.closure().Run();
    result_state_ = state.CreateDeepCopy();
  }

  std::unique_ptr<base::DictionaryValue> WaitForNewState() {
    test_closure_.WaitForResult();
    DCHECK(result_state_);
    return std::move(result_state_);
  }

 private:
  net::TestClosure test_closure_;
  std::unique_ptr<base::DictionaryValue> result_state_;
};

// A class that wraps around TestClosure. Provides the ability to wait on a
// file path callback and retrieve the result.
class TestFilePathCallback {
 public:
  TestFilePathCallback()
      : callback_(base::Bind(&TestFilePathCallback::SetResultThenNotify,
                             base::Unretained(this))) {}

  const base::Callback<void(const base::FilePath&)>& callback() const {
    return callback_;
  }

  const base::FilePath& WaitForResult() {
    test_closure_.WaitForResult();
    return result_;
  }

 private:
  void SetResultThenNotify(const base::FilePath& result) {
    result_ = result;
    test_closure_.closure().Run();
  }

  net::TestClosure test_closure_;
  base::FilePath result_;
  base::Callback<void(const base::FilePath&)> callback_;
};

class NetExportFileWriterTest : public ::testing::Test {
 public:
  NetExportFileWriterTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::IO),
        network_service_(network::NetworkService::CreateForTesting()) {}

  // ::testing::Test implementation
  void SetUp() override {
    ASSERT_TRUE(log_temp_dir_.CreateUniqueTempDir());
    network::mojom::NetworkContextParamsPtr params =
        network::mojom::NetworkContextParams::New();
    // Use a fixed proxy config, to avoid dependencies on local network
    // configuration.
    params->initial_proxy_config =
        net::ProxyConfigWithAnnotation::CreateDirect();
    network_context_ = std::make_unique<network::NetworkContext>(
        network_service_.get(), mojo::MakeRequest(&network_context_ptr_),
        std::move(params));

    // Override |file_writer_|'s default-log-base-directory-getter to
    // a getter that returns the temp dir created for the test.
    file_writer_.SetDefaultLogBaseDirectoryGetterForTest(
        base::Bind(&SetPathToGivenAndReturnTrue, log_temp_dir_.GetPath()));

    default_log_path_ = log_temp_dir_.GetPath().Append(kLogRelativePath);

    file_writer_.AddObserver(&test_state_observer_);

    ASSERT_TRUE(VerifyState(file_writer_.GetState(), kStateUninitializedString,
                            false, false, ""));
  }

  // ::testing::Test implementation
  void TearDown() override {
    file_writer_.RemoveObserver(&test_state_observer_);
    ASSERT_TRUE(log_temp_dir_.Delete());
  }

  base::FilePath FileWriterGetFilePathToCompletedLog() {
    TestFilePathCallback test_callback;
    file_writer_.GetFilePathToCompletedLog(test_callback.callback());
    return test_callback.WaitForResult();
  }

  WARN_UNUSED_RESULT ::testing::AssertionResult InitializeThenVerifyNewState(
      bool expected_initialize_success,
      bool expected_log_exists) {
    file_writer_.Initialize();
    std::unique_ptr<base::DictionaryValue> state =
        test_state_observer_.WaitForNewState();
    ::testing::AssertionResult result =
        VerifyState(std::move(state), kStateInitializingString);
    if (!result) {
      return ::testing::AssertionFailure()
             << "First state after Initialize() does not match expected:"
             << std::endl
             << result.message();
    }

    state = test_state_observer_.WaitForNewState();
    result =
        VerifyState(std::move(state),
                    expected_initialize_success ? kStateNotLoggingString
                                                : kStateUninitializedString,
                    expected_log_exists, false, "");
    if (!result) {
      return ::testing::AssertionFailure()
             << "Second state after Initialize() does not match expected:"
             << std::endl
             << result.message();
    }

    return ::testing::AssertionSuccess();
  }

  // If |custom_log_path| is empty path, |file_writer_| will use its
  // default log path, which is cached in |default_log_path_|.
  WARN_UNUSED_RESULT::testing::AssertionResult StartThenVerifyNewState(
      const base::FilePath& custom_log_path,
      net::NetLogCaptureMode capture_mode,
      const std::string& expected_capture_mode_string,
      network::mojom::NetworkContext* network_context) {
    file_writer_.StartNetLog(custom_log_path, capture_mode, kMaxLogSizeBytes,
                             base::CommandLine::StringType(), kChannelString,
                             network_context);
    std::unique_ptr<base::DictionaryValue> state =
        test_state_observer_.WaitForNewState();
    ::testing::AssertionResult result =
        VerifyState(std::move(state), kStateStartingLogString);
    if (!result) {
      return ::testing::AssertionFailure()
             << "First state after StartNetLog() does not match expected:"
             << std::endl
             << result.message();
    }

    state = test_state_observer_.WaitForNewState();
    result = VerifyState(std::move(state), kStateLoggingString, true, true,
                         expected_capture_mode_string);
    if (!result) {
      return ::testing::AssertionFailure()
             << "Second state after StartNetLog() does not match expected:"
             << std::endl
             << result.message();
    }

    // Make sure GetFilePath() returns empty path when logging.
    base::FilePath actual_log_path = FileWriterGetFilePathToCompletedLog();
    if (!actual_log_path.empty()) {
      return ::testing::AssertionFailure()
             << "GetFilePath() should return empty path after logging starts."
             << " Actual: " << ::testing::PrintToString(actual_log_path);
    }

    return ::testing::AssertionSuccess();
  }

  // If |custom_log_path| is empty path, it's assumed the log file with be at
  // |default_log_path_|.
  WARN_UNUSED_RESULT ::testing::AssertionResult StopThenVerifyNewStateAndFile(
      const base::FilePath& custom_log_path,
      std::unique_ptr<base::DictionaryValue> polled_data,
      const std::string& expected_capture_mode_string) {
    file_writer_.StopNetLog(std::move(polled_data));
    std::unique_ptr<base::DictionaryValue> state =
        test_state_observer_.WaitForNewState();
    ::testing::AssertionResult result =
        VerifyState(std::move(state), kStateStoppingLogString);
    if (!result) {
      return ::testing::AssertionFailure()
             << "First state after StopNetLog() does not match expected:"
             << std::endl
             << result.message();
    }

    state = test_state_observer_.WaitForNewState();
    result = VerifyState(std::move(state), kStateNotLoggingString, true, true,
                         expected_capture_mode_string);
    if (!result) {
      return ::testing::AssertionFailure()
             << "Second state after StopNetLog() does not match expected:"
             << std::endl
             << result.message();
    }

    // Make sure GetFilePath() returns the expected path.
    const base::FilePath& expected_log_path =
        custom_log_path.empty() ? default_log_path_ : custom_log_path;
    base::FilePath actual_log_path = FileWriterGetFilePathToCompletedLog();
    if (actual_log_path != expected_log_path) {
      return ::testing::AssertionFailure()
             << "GetFilePath() returned incorrect path after logging stopped."
             << std::endl
             << "    Actual: " << ::testing::PrintToString(actual_log_path)
             << std::endl
             << "  Expected: " << ::testing::PrintToString(expected_log_path);
    }

    // Make sure the generated log file is valid.
    std::unique_ptr<base::DictionaryValue> root;
    result = ReadCompleteLogFile(expected_log_path, &root);
    if (!result) {
      return ::testing::AssertionFailure()
             << "Log file after logging stopped is not valid:" << std::endl
             << result.message();
    }

    return ::testing::AssertionSuccess();
  }

  net::NetLog* net_log() { return network_service_->net_log(); }

  NetExportFileWriter* file_writer() { return &file_writer_; }

  const base::FilePath& GetLogTempDirPath() const {
    return log_temp_dir_.GetPath();
  }

  const base::FilePath& default_log_path() const { return default_log_path_; }

  TestStateObserver* test_state_observer() { return &test_state_observer_; }

  network::mojom::NetworkContext* network_context() {
    return network_context_ptr_.get();
  }

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  std::unique_ptr<network::NetworkService> network_service_;

  network::mojom::NetworkContextPtr network_context_ptr_;
  std::unique_ptr<network::NetworkContext> network_context_;

  // |file_writer_| is initialized after |network_context_ptr_| since
  // |file_writer_| destructor can talk to mojo objects owned by
  // |network_context_|.
  NetExportFileWriter file_writer_;

  base::ScopedTempDir log_temp_dir_;

  // The default log path that |file_writer_| will use is cached here.
  base::FilePath default_log_path_;

  TestStateObserver test_state_observer_;
};

TEST_F(NetExportFileWriterTest, InitFail) {
  // Override file_writer_'s default log base directory getter to always
  // fail.
  file_writer()->SetDefaultLogBaseDirectoryGetterForTest(
      base::Bind([](base::FilePath* path) -> bool { return false; }));

  // Initialization should fail due to the override.
  ASSERT_TRUE(InitializeThenVerifyNewState(false, false));

  // NetExportFileWriter::GetFilePath() should return empty path if
  // uninitialized.
  EXPECT_TRUE(FileWriterGetFilePathToCompletedLog().empty());
}

TEST_F(NetExportFileWriterTest, InitWithoutExistingLog) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  // NetExportFileWriter::GetFilePathToCompletedLog() should return empty path
  // when no log file exists.
  EXPECT_TRUE(FileWriterGetFilePathToCompletedLog().empty());
}

TEST_F(NetExportFileWriterTest, InitWithExistingLog) {
  // Create and close an empty log file to simulate existence of a previous log
  // file.
  ASSERT_TRUE(
      base::CreateDirectoryAndGetError(default_log_path().DirName(), nullptr));
  base::ScopedFILE empty_file(base::OpenFile(default_log_path(), "w"));
  ASSERT_TRUE(empty_file.get());
  empty_file.reset();

  ASSERT_TRUE(InitializeThenVerifyNewState(true, true));

  EXPECT_EQ(default_log_path(), FileWriterGetFilePathToCompletedLog());
}

TEST_F(NetExportFileWriterTest, StartAndStopWithAllCaptureModes) {
  const net::NetLogCaptureMode capture_modes[3] = {
      net::NetLogCaptureMode::Default(),
      net::NetLogCaptureMode::IncludeCookiesAndCredentials(),
      net::NetLogCaptureMode::IncludeSocketBytes()};

  const std::string capture_mode_strings[3] = {
      kCaptureModeDefaultString, kCaptureModeIncludeCookiesAndCredentialsString,
      kCaptureModeIncludeSocketBytesString};

  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  // For each capture mode, start and stop |file_writer_| in that mode.
  for (int i = 0; i < 3; ++i) {
    // StartNetLog(), should result in state change.
    ASSERT_TRUE(StartThenVerifyNewState(base::FilePath(), capture_modes[i],
                                        capture_mode_strings[i],
                                        network_context()));

    // Calling StartNetLog() again should be a no-op. Try doing StartNetLog()
    // with various capture modes; they should all be ignored and result in no
    // state change.
    file_writer()->StartNetLog(
        base::FilePath(), capture_modes[i], kMaxLogSizeBytes,
        base::CommandLine::StringType(), kChannelString, network_context());
    file_writer()->StartNetLog(
        base::FilePath(), capture_modes[(i + 1) % 3], kMaxLogSizeBytes,
        base::CommandLine::StringType(), kChannelString, network_context());
    file_writer()->StartNetLog(
        base::FilePath(), capture_modes[(i + 2) % 3], kMaxLogSizeBytes,
        base::CommandLine::StringType(), kChannelString, network_context());

    // StopNetLog(), should result in state change. The capture mode should
    // match that of the first StartNetLog() call (called by
    // StartThenVerifyNewState()).
    ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                              capture_mode_strings[i]));

    // Stopping a second time should be a no-op.
    file_writer()->StopNetLog(nullptr);
  }

  // Start and stop one more time just to make sure the last StopNetLog() call
  // was properly ignored and left |file_writer_| in a valid state.
  ASSERT_TRUE(StartThenVerifyNewState(base::FilePath(), capture_modes[0],
                                      capture_mode_strings[0],
                                      network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            capture_mode_strings[0]));
}

// Verify the file sizes after two consecutive starts/stops are the same (even
// if some junk data is added in between).
TEST_F(NetExportFileWriterTest, StartClearsFile) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            kCaptureModeDefaultString));

  int64_t stop_file_size;
  EXPECT_TRUE(base::GetFileSize(default_log_path(), &stop_file_size));

  // Add some junk at the end of the file.
  std::string junk_data("Hello");
  EXPECT_TRUE(base::AppendToFile(default_log_path(), junk_data.c_str(),
                                 junk_data.size()));

  int64_t junk_file_size;
  EXPECT_TRUE(base::GetFileSize(default_log_path(), &junk_file_size));
  EXPECT_GT(junk_file_size, stop_file_size);

  // Start and stop again and make sure the file is back to the size it was
  // before adding the junk data.
  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            kCaptureModeDefaultString));

  int64_t new_stop_file_size;
  EXPECT_TRUE(base::GetFileSize(default_log_path(), &new_stop_file_size));

  EXPECT_EQ(stop_file_size, new_stop_file_size);
}

// Adds an event to the log file, then checks that the file is larger than
// the file created without that event.
TEST_F(NetExportFileWriterTest, AddEvent) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            kCaptureModeDefaultString));

  // Get file size without the event.
  int64_t stop_file_size;
  EXPECT_TRUE(base::GetFileSize(default_log_path(), &stop_file_size));

  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  net_log()->AddGlobalEntry(net::NetLogEventType::CANCELLED);

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            kCaptureModeDefaultString));

  // Get file size after adding the event and make sure it's larger than before.
  int64_t new_stop_file_size;
  EXPECT_TRUE(base::GetFileSize(default_log_path(), &new_stop_file_size));
  EXPECT_GE(new_stop_file_size, stop_file_size);
}

// Using a custom path to make sure logging can still occur when the path has
// changed.
TEST_F(NetExportFileWriterTest, AddEventCustomPath) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  base::FilePath::CharType kCustomRelativePath[] =
      FILE_PATH_LITERAL("custom/custom/chrome-net-export-log.json");
  base::FilePath custom_log_path =
      GetLogTempDirPath().Append(kCustomRelativePath);
  EXPECT_TRUE(
      base::CreateDirectoryAndGetError(custom_log_path.DirName(), nullptr));

  ASSERT_TRUE(StartThenVerifyNewState(
      custom_log_path, net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(custom_log_path, nullptr,
                                            kCaptureModeDefaultString));

  // Get file size without the event.
  int64_t stop_file_size;
  EXPECT_TRUE(base::GetFileSize(custom_log_path, &stop_file_size));

  ASSERT_TRUE(StartThenVerifyNewState(
      custom_log_path, net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  net_log()->AddGlobalEntry(net::NetLogEventType::CANCELLED);

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(custom_log_path, nullptr,
                                            kCaptureModeDefaultString));

  // Get file size after adding the event and make sure it's larger than before.
  int64_t new_stop_file_size;
  EXPECT_TRUE(base::GetFileSize(custom_log_path, &new_stop_file_size));
  EXPECT_GE(new_stop_file_size, stop_file_size);
}

TEST_F(NetExportFileWriterTest, StopWithPolledData) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  // Create dummy polled data
  const char kDummyPolledDataPath[] = "dummy_path";
  const char kDummyPolledDataString[] = "dummy_info";
  std::unique_ptr<base::DictionaryValue> dummy_polled_data =
      std::make_unique<base::DictionaryValue>();
  dummy_polled_data->SetString(kDummyPolledDataPath, kDummyPolledDataString);

  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(),
                                            std::move(dummy_polled_data),
                                            kCaptureModeDefaultString));

  // Read polledData from log file.
  std::unique_ptr<base::DictionaryValue> root;
  ASSERT_TRUE(ReadCompleteLogFile(default_log_path(), &root));
  base::DictionaryValue* polled_data;
  ASSERT_TRUE(root->GetDictionary("polledData", &polled_data));

  // Check that it contains the field from the polled data that was passed in.
  std::string dummy_string;
  ASSERT_TRUE(polled_data->GetString(kDummyPolledDataPath, &dummy_string));
  EXPECT_EQ(kDummyPolledDataString, dummy_string);

  // Check that it also contains something from net::GetNetInfo.
  base::DictionaryValue* http_cache_info;
  ASSERT_TRUE(polled_data->GetDictionary("httpCacheInfo", &http_cache_info));
}

// Test with requests in flight. This is done by going through a sequence of a
// redirect --- at which point the log is started --- and then a fetch of
// destination that's blocked on an event in EmbeddedTestServer.
TEST_F(NetExportFileWriterTest, StartWithNetworkContextActive) {
  net::EmbeddedTestServer test_server;
  net::test_server::RegisterDefaultHandlers(&test_server);

  base::WaitableEvent block_fetch(
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED);

  test_server.RegisterRequestHandler(base::BindRepeating(
      [](base::WaitableEvent* block_fetch,
         const net::test_server::HttpRequest& request)
          -> std::unique_ptr<net::test_server::HttpResponse> {
        if (request.relative_url == "/block")
          block_fetch->Wait();
        return nullptr;
      },
      &block_fetch));

  ASSERT_TRUE(test_server.Start());

  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  network::mojom::URLLoaderFactoryPtr url_loader_factory;
  network_context()->CreateURLLoaderFactory(
      mojo::MakeRequest(&url_loader_factory), 0);

  const char kRedirectURL[] = "/server-redirect?/block";
  std::unique_ptr<network::ResourceRequest> request =
      std::make_unique<network::ResourceRequest>();
  request->url = test_server.GetURL(kRedirectURL);

  std::unique_ptr<network::SimpleURLLoader> simple_loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);
  base::RunLoop run_loop, run_loop2;
  simple_loader->SetOnRedirectCallback(base::BindRepeating(
      [](base::RepeatingClosure notify_log,
         const net::RedirectInfo& redirect_info,
         const network::ResourceResponseHead& response_head) {
        notify_log.Run();
      },
      run_loop.QuitClosure()));
  simple_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      url_loader_factory.get(),
      base::BindOnce(
          [](base::OnceClosure quit_closure,
             std::unique_ptr<std::string> response_body) {
            std::move(quit_closure).Run();
          },
          run_loop2.QuitClosure()));

  // Wait for fetch to get some bytes accross. It will not be the entire
  // thing since the post-redirect URL will get blocked by the custom handler.
  run_loop.Run();
  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::Default(),
      kCaptureModeDefaultString, network_context()));

  ASSERT_TRUE(StopThenVerifyNewStateAndFile(base::FilePath(), nullptr,
                                            kCaptureModeDefaultString));
  // Read events from log file.
  std::unique_ptr<base::DictionaryValue> root;
  ASSERT_TRUE(ReadCompleteLogFile(default_log_path(), &root));
  base::ListValue* events;
  ASSERT_TRUE(root->GetList("events", &events));

  // Check there is at least one event as a result of the ongoing request.
  ASSERT_GE(events->GetSize(), 1u);

  // Check the URL in the params of the first event.
  base::DictionaryValue* event;
  EXPECT_TRUE(events->GetDictionary(0, &event));
  base::DictionaryValue* event_params;
  EXPECT_TRUE(event->GetDictionary("params", &event_params));
  std::string event_url;
  EXPECT_TRUE(event_params->GetString("url", &event_url));
  EXPECT_EQ(test_server.GetURL(kRedirectURL), event_url);

  block_fetch.Signal();
  run_loop2.Run();
}

TEST_F(NetExportFileWriterTest, ReceiveStartWhileInitializing) {
  // Trigger initialization of |file_writer_|.
  file_writer()->Initialize();

  // Before running the main message loop, tell |file_writer_| to start
  // logging. Not running the main message loop prevents the initialization
  // process from completing, so this ensures that StartNetLog() is received
  // before |file_writer_| finishes initialization, which means this
  // should be a no-op.
  file_writer()->StartNetLog(
      base::FilePath(), net::NetLogCaptureMode::Default(), kMaxLogSizeBytes,
      base::CommandLine::StringType(), kChannelString, network_context());

  // Now run the main message loop. Make sure StartNetLog() was ignored by
  // checking that the next two states are "initializing" followed by
  // "not-logging".
  std::unique_ptr<base::DictionaryValue> state =
      test_state_observer()->WaitForNewState();
  ASSERT_TRUE(VerifyState(std::move(state), kStateInitializingString));
  state = test_state_observer()->WaitForNewState();
  ASSERT_TRUE(
      VerifyState(std::move(state), kStateNotLoggingString, false, false, ""));
}

TEST_F(NetExportFileWriterTest, ReceiveStartWhileStoppingLog) {
  ASSERT_TRUE(InitializeThenVerifyNewState(true, false));

  // Call StartNetLog() on |file_writer_| and wait for the state change.
  ASSERT_TRUE(StartThenVerifyNewState(
      base::FilePath(), net::NetLogCaptureMode::IncludeSocketBytes(),
      kCaptureModeIncludeSocketBytesString, network_context()));

  // Tell |file_writer_| to stop logging.
  file_writer()->StopNetLog(nullptr);

  // Before running the main message loop, tell |file_writer_| to start
  // logging. Not running the main message loop prevents the stopping process
  // from completing, so this ensures StartNetLog() is received before
  // |file_writer_| finishes stopping, which means this should be a
  // no-op.
  file_writer()->StartNetLog(
      base::FilePath(), net::NetLogCaptureMode::Default(), kMaxLogSizeBytes,
      base::CommandLine::StringType(), kChannelString, network_context());

  // Now run the main message loop. Make sure the last StartNetLog() was
  // ignored by checking that the next two states are "stopping-log" followed by
  // "not-logging". Also make sure the capture mode matches that of the first
  // StartNetLog() call (called by StartThenVerifyState()).
  std::unique_ptr<base::DictionaryValue> state =
      test_state_observer()->WaitForNewState();
  ASSERT_TRUE(VerifyState(std::move(state), kStateStoppingLogString));
  state = test_state_observer()->WaitForNewState();
  ASSERT_TRUE(VerifyState(std::move(state), kStateNotLoggingString, true, true,
                          kCaptureModeIncludeSocketBytesString));
}

}  // namespace net_log
