// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_util_proxy.h"

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/threading/thread.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base {

class FileUtilProxyTest : public testing::Test {
 public:
  FileUtilProxyTest()
      : file_thread_("FileUtilProxyTestFileThread"),
        error_(File::FILE_OK),
        weak_factory_(this) {}

  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    ASSERT_TRUE(file_thread_.Start());
  }

  void DidFinish(File::Error error) {
    error_ = error;
    RunLoop::QuitCurrentWhenIdleDeprecated();
  }

  void DidGetFileInfo(File::Error error,
                      const File::Info& file_info) {
    error_ = error;
    file_info_ = file_info;
    RunLoop::QuitCurrentWhenIdleDeprecated();
  }

 protected:
  TaskRunner* file_task_runner() const {
    return file_thread_.task_runner().get();
  }
  const FilePath TestPath() const { return dir_.GetPath().AppendASCII("test"); }

  MessageLoopForIO message_loop_;
  Thread file_thread_;

  ScopedTempDir dir_;
  File::Error error_;
  FilePath path_;
  File::Info file_info_;
  std::vector<char> buffer_;
  WeakPtrFactory<FileUtilProxyTest> weak_factory_;
};


TEST_F(FileUtilProxyTest, GetFileInfo_File) {
  // Setup.
  ASSERT_EQ(4, WriteFile(TestPath(), "test", 4));
  File::Info expected_info;
  GetFileInfo(TestPath(), &expected_info);

  // Run.
  FileUtilProxy::GetFileInfo(
      file_task_runner(), TestPath(),
      BindOnce(&FileUtilProxyTest::DidGetFileInfo, weak_factory_.GetWeakPtr()));
  RunLoop().Run();

  // Verify.
  EXPECT_EQ(File::FILE_OK, error_);
  EXPECT_EQ(expected_info.size, file_info_.size);
  EXPECT_EQ(expected_info.is_directory, file_info_.is_directory);
  EXPECT_EQ(expected_info.is_symbolic_link, file_info_.is_symbolic_link);
  EXPECT_EQ(expected_info.last_modified, file_info_.last_modified);
  EXPECT_EQ(expected_info.last_accessed, file_info_.last_accessed);
  EXPECT_EQ(expected_info.creation_time, file_info_.creation_time);
}

TEST_F(FileUtilProxyTest, GetFileInfo_Directory) {
  // Setup.
  ASSERT_TRUE(base::CreateDirectory(TestPath()));
  File::Info expected_info;
  GetFileInfo(TestPath(), &expected_info);

  // Run.
  FileUtilProxy::GetFileInfo(
      file_task_runner(), TestPath(),
      BindOnce(&FileUtilProxyTest::DidGetFileInfo, weak_factory_.GetWeakPtr()));
  RunLoop().Run();

  // Verify.
  EXPECT_EQ(File::FILE_OK, error_);
  EXPECT_EQ(expected_info.size, file_info_.size);
  EXPECT_EQ(expected_info.is_directory, file_info_.is_directory);
  EXPECT_EQ(expected_info.is_symbolic_link, file_info_.is_symbolic_link);
  EXPECT_EQ(expected_info.last_modified, file_info_.last_modified);
  EXPECT_EQ(expected_info.last_accessed, file_info_.last_accessed);
  EXPECT_EQ(expected_info.creation_time, file_info_.creation_time);
}

}  // namespace base
