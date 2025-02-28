// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/memory/shared_memory.h"
#include "base/process/process_handle.h"
#include "build/build_config.h"
#include "mojo/edk/test/mojo_test_base.h"
#include "mojo/public/c/system/platform_handle.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

#if defined(OS_WIN)
#define SIMPLE_PLATFORM_HANDLE_TYPE MOJO_PLATFORM_HANDLE_TYPE_WINDOWS_HANDLE
#elif defined(OS_POSIX) || defined(OS_FUCHSIA)
#define SIMPLE_PLATFORM_HANDLE_TYPE MOJO_PLATFORM_HANDLE_TYPE_FILE_DESCRIPTOR
#endif

#if defined(OS_FUCHSIA)
#define SHARED_BUFFER_PLATFORM_HANDLE_TYPE \
  MOJO_PLATFORM_HANDLE_TYPE_FUCHSIA_HANDLE
#elif defined(OS_MACOSX) && !defined(OS_IOS)
#define SHARED_BUFFER_PLATFORM_HANDLE_TYPE MOJO_PLATFORM_HANDLE_TYPE_MACH_PORT
#elif defined(OS_WIN) || defined(OS_POSIX)
#define SHARED_BUFFER_PLATFORM_HANDLE_TYPE SIMPLE_PLATFORM_HANDLE_TYPE
#endif

uint64_t PlatformHandleValueFromPlatformFile(base::PlatformFile file) {
#if defined(OS_WIN)
  return reinterpret_cast<uint64_t>(file);
#elif defined(OS_POSIX) || defined(OS_FUCHSIA)
  return static_cast<uint64_t>(file);
#endif
}

base::PlatformFile PlatformFileFromPlatformHandleValue(uint64_t value) {
#if defined(OS_WIN)
  return reinterpret_cast<base::PlatformFile>(value);
#elif defined(OS_POSIX) || defined(OS_FUCHSIA)
  return static_cast<base::PlatformFile>(value);
#endif
}

namespace mojo {
namespace edk {
namespace {

using PlatformWrapperTest = test::MojoTestBase;

TEST_F(PlatformWrapperTest, WrapPlatformHandle) {
  // Create a temporary file and write a message to it.
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFile(&temp_file_path));
  const std::string kMessage = "Hello, world!";
  EXPECT_EQ(base::WriteFile(temp_file_path, kMessage.data(),
                            static_cast<int>(kMessage.size())),
            static_cast<int>(kMessage.size()));

  RunTestClient("ReadPlatformFile", [&](MojoHandle h) {
    // Open the temporary file for reading, wrap its handle, and send it to
    // the child along with the expected message to be read.
    base::File file(temp_file_path,
                    base::File::FLAG_OPEN | base::File::FLAG_READ);
    EXPECT_TRUE(file.IsValid());

    MojoHandle wrapped_handle;
    MojoPlatformHandle os_file;
    os_file.struct_size = sizeof(MojoPlatformHandle);
    os_file.type = SIMPLE_PLATFORM_HANDLE_TYPE;
    os_file.value =
        PlatformHandleValueFromPlatformFile(file.TakePlatformFile());
    EXPECT_EQ(MOJO_RESULT_OK,
              MojoWrapPlatformHandle(&os_file, &wrapped_handle));

    WriteMessageWithHandles(h, kMessage, &wrapped_handle, 1);
  });

  base::DeleteFile(temp_file_path, false);
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(ReadPlatformFile, PlatformWrapperTest, h) {
  // Read a message and a wrapped file handle; unwrap the handle.
  MojoHandle wrapped_handle;
  std::string message = ReadMessageWithHandles(h, &wrapped_handle, 1);

  MojoPlatformHandle platform_handle;
  platform_handle.struct_size = sizeof(MojoPlatformHandle);
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoUnwrapPlatformHandle(wrapped_handle, &platform_handle));
  EXPECT_EQ(SIMPLE_PLATFORM_HANDLE_TYPE, platform_handle.type);
  base::File file(PlatformFileFromPlatformHandleValue(platform_handle.value));

  // Expect to read the same message from the file.
  std::vector<char> data(message.size());
  EXPECT_EQ(file.ReadAtCurrentPos(data.data(), static_cast<int>(data.size())),
            static_cast<int>(data.size()));
  EXPECT_TRUE(std::equal(message.begin(), message.end(), data.begin()));
}

TEST_F(PlatformWrapperTest, WrapPlatformSharedBufferHandle) {
  // Allocate a new platform shared buffer and write a message into it.
  const std::string kMessage = "Hello, world!";
  base::SharedMemory buffer;
  buffer.CreateAndMapAnonymous(kMessage.size());
  CHECK(buffer.memory());
  memcpy(buffer.memory(), kMessage.data(), kMessage.size());

  RunTestClient("ReadPlatformSharedBuffer", [&](MojoHandle h) {
    // Wrap the shared memory handle and send it to the child along with the
    // expected message.
    base::SharedMemoryHandle memory_handle =
        base::SharedMemory::DuplicateHandle(buffer.handle());
    MojoPlatformHandle os_buffer;
    os_buffer.struct_size = sizeof(MojoPlatformHandle);
    os_buffer.type = SHARED_BUFFER_PLATFORM_HANDLE_TYPE;
#if defined(OS_WIN)
    os_buffer.value = reinterpret_cast<uint64_t>(memory_handle.GetHandle());
#elif defined(OS_MACOSX) && !defined(OS_IOS)
    os_buffer.value = static_cast<uint64_t>(memory_handle.GetMemoryObject());
#elif defined(OS_POSIX) || defined(OS_FUCHSIA)
    os_buffer.value = static_cast<uint64_t>(memory_handle.GetHandle());
#else
#error Unsupported platform
#endif

    MojoSharedBufferGuid mojo_guid;
    base::UnguessableToken guid = memory_handle.GetGUID();
    mojo_guid.high = guid.GetHighForSerialization();
    mojo_guid.low = guid.GetLowForSerialization();

    MojoHandle wrapped_handle;
    ASSERT_EQ(MOJO_RESULT_OK, MojoWrapPlatformSharedBufferHandle(
                                  &os_buffer, kMessage.size(), &mojo_guid,
                                  MOJO_PLATFORM_SHARED_BUFFER_HANDLE_FLAG_NONE,
                                  &wrapped_handle));
    WriteMessageWithHandles(h, kMessage, &wrapped_handle, 1);

    // As a sanity check, send the GUID explicitly in a second message. We'll
    // verify that the deserialized buffer handle holds the same GUID on the
    // receiving end.
    WriteMessageRaw(MessagePipeHandle(h), &mojo_guid, sizeof(mojo_guid),
                    nullptr, 0, MOJO_WRITE_MESSAGE_FLAG_NONE);
  });
}

DEFINE_TEST_CLIENT_TEST_WITH_PIPE(ReadPlatformSharedBuffer,
                                  PlatformWrapperTest,
                                  h) {
  // Read a message and a wrapped shared buffer handle.
  MojoHandle wrapped_handle;
  std::string message = ReadMessageWithHandles(h, &wrapped_handle, 1);

  // Check the message in the buffer
  ExpectBufferContents(wrapped_handle, 0, message);

  // Now unwrap the buffer and verify that the base::SharedMemoryHandle also
  // works as expected.
  MojoPlatformHandle os_buffer;
  os_buffer.struct_size = sizeof(MojoPlatformHandle);
  size_t size;
  MojoSharedBufferGuid mojo_guid;
  MojoPlatformSharedBufferHandleFlags flags;
  ASSERT_EQ(MOJO_RESULT_OK,
            MojoUnwrapPlatformSharedBufferHandle(wrapped_handle, &os_buffer,
                                                 &size, &mojo_guid, &flags));
  bool read_only = flags & MOJO_PLATFORM_SHARED_BUFFER_HANDLE_FLAG_NONE;
  EXPECT_FALSE(read_only);

  base::UnguessableToken guid =
      base::UnguessableToken::Deserialize(mojo_guid.high, mojo_guid.low);
#if defined(OS_WIN)
  ASSERT_EQ(MOJO_PLATFORM_HANDLE_TYPE_WINDOWS_HANDLE, os_buffer.type);
  base::SharedMemoryHandle memory_handle(
      reinterpret_cast<HANDLE>(os_buffer.value), size, guid);
#elif defined(OS_FUCHSIA)
  ASSERT_EQ(MOJO_PLATFORM_HANDLE_TYPE_FUCHSIA_HANDLE, os_buffer.type);
  base::SharedMemoryHandle memory_handle(
      static_cast<zx_handle_t>(os_buffer.value), size, guid);
#elif defined(OS_MACOSX) && !defined(OS_IOS)
  ASSERT_EQ(MOJO_PLATFORM_HANDLE_TYPE_MACH_PORT, os_buffer.type);
  base::SharedMemoryHandle memory_handle(
      static_cast<mach_port_t>(os_buffer.value), size, guid);
#elif defined(OS_POSIX)
  ASSERT_EQ(MOJO_PLATFORM_HANDLE_TYPE_FILE_DESCRIPTOR, os_buffer.type);
  base::SharedMemoryHandle memory_handle(
      base::FileDescriptor(static_cast<int>(os_buffer.value), false), size,
      guid);
#endif

  base::SharedMemory memory(memory_handle, read_only);
  memory.Map(message.size());
  ASSERT_TRUE(memory.memory());

  EXPECT_TRUE(std::equal(message.begin(), message.end(),
                         static_cast<const char*>(memory.memory())));

  // Verify that the received buffer's internal GUID was preserved in transit.
  EXPECT_EQ(MOJO_RESULT_OK, WaitForSignals(h, MOJO_HANDLE_SIGNAL_READABLE));
  std::vector<uint8_t> guid_bytes;
  EXPECT_EQ(MOJO_RESULT_OK,
            ReadMessageRaw(MessagePipeHandle(h), &guid_bytes, nullptr,
                           MOJO_READ_MESSAGE_FLAG_NONE));
  EXPECT_EQ(sizeof(MojoSharedBufferGuid), guid_bytes.size());
  auto* expected_guid =
      reinterpret_cast<MojoSharedBufferGuid*>(guid_bytes.data());
  EXPECT_EQ(expected_guid->high, mojo_guid.high);
  EXPECT_EQ(expected_guid->low, mojo_guid.low);
}

TEST_F(PlatformWrapperTest, InvalidHandle) {
  // Wrap an invalid platform handle and expect to unwrap the same.

  MojoHandle wrapped_handle;
  MojoPlatformHandle invalid_handle;
  invalid_handle.struct_size = sizeof(MojoPlatformHandle);
  invalid_handle.type = MOJO_PLATFORM_HANDLE_TYPE_INVALID;
  EXPECT_EQ(MOJO_RESULT_OK,
            MojoWrapPlatformHandle(&invalid_handle, &wrapped_handle));
  EXPECT_EQ(MOJO_RESULT_OK,
            MojoUnwrapPlatformHandle(wrapped_handle, &invalid_handle));
  EXPECT_EQ(MOJO_PLATFORM_HANDLE_TYPE_INVALID, invalid_handle.type);
}

TEST_F(PlatformWrapperTest, InvalidArgument) {
  // Try to wrap an invalid MojoPlatformHandle struct and expect an error.
  MojoHandle wrapped_handle;
  MojoPlatformHandle platform_handle;
  platform_handle.struct_size = 0;
  EXPECT_EQ(MOJO_RESULT_INVALID_ARGUMENT,
            MojoWrapPlatformHandle(&platform_handle, &wrapped_handle));
}

}  // namespace
}  // namespace edk
}  // namespace mojo
