// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/shared_buffer_dispatcher.h"

#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "build/build_config.h"
#include "mojo/edk/embedder/platform_handle_utils.h"
#include "mojo/edk/system/configuration.h"
#include "mojo/edk/system/node_controller.h"
#include "mojo/edk/system/options_validation.h"
#include "mojo/edk/system/platform_shared_memory_mapping.h"

namespace mojo {
namespace edk {

namespace {

#pragma pack(push, 1)

struct SerializedState {
  uint64_t num_bytes;
  uint32_t flags;
  uint64_t guid_high;
  uint64_t guid_low;
  uint32_t padding;
};

const uint32_t kSerializedStateFlagsReadOnly = 1 << 0;

#pragma pack(pop)

static_assert(sizeof(SerializedState) % 8 == 0,
              "Invalid SerializedState size.");

}  // namespace

// static
const MojoCreateSharedBufferOptions
    SharedBufferDispatcher::kDefaultCreateOptions = {
        static_cast<uint32_t>(sizeof(MojoCreateSharedBufferOptions)),
        MOJO_CREATE_SHARED_BUFFER_FLAG_NONE};

// static
MojoResult SharedBufferDispatcher::ValidateCreateOptions(
    const MojoCreateSharedBufferOptions* in_options,
    MojoCreateSharedBufferOptions* out_options) {
  const MojoCreateSharedBufferFlags kKnownFlags =
      MOJO_CREATE_SHARED_BUFFER_FLAG_NONE;

  *out_options = kDefaultCreateOptions;
  if (!in_options)
    return MOJO_RESULT_OK;

  UserOptionsReader<MojoCreateSharedBufferOptions> reader(in_options);
  if (!reader.is_valid())
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (!OPTIONS_STRUCT_HAS_MEMBER(MojoCreateSharedBufferOptions, flags, reader))
    return MOJO_RESULT_OK;
  if ((reader.options().flags & ~kKnownFlags))
    return MOJO_RESULT_UNIMPLEMENTED;
  out_options->flags = reader.options().flags;

  // Checks for fields beyond |flags|:

  // (Nothing here yet.)

  return MOJO_RESULT_OK;
}

// static
MojoResult SharedBufferDispatcher::Create(
    const MojoCreateSharedBufferOptions& /*validated_options*/,
    NodeController* node_controller,
    uint64_t num_bytes,
    scoped_refptr<SharedBufferDispatcher>* result) {
  if (!num_bytes)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (num_bytes > GetConfiguration().max_shared_memory_num_bytes)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  base::WritableSharedMemoryRegion writable_region;
  if (node_controller) {
    writable_region =
        node_controller->CreateSharedBuffer(static_cast<size_t>(num_bytes));
  } else {
    writable_region = base::WritableSharedMemoryRegion::Create(
        static_cast<size_t>(num_bytes));
  }
  if (!writable_region.IsValid())
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  *result = CreateInternal(
      base::WritableSharedMemoryRegion::TakeHandleForSerialization(
          std::move(writable_region)));
  return MOJO_RESULT_OK;
}

// static
MojoResult SharedBufferDispatcher::CreateFromPlatformSharedMemoryRegion(
    base::subtle::PlatformSharedMemoryRegion region,
    scoped_refptr<SharedBufferDispatcher>* result) {
  if (!region.IsValid())
    return MOJO_RESULT_INVALID_ARGUMENT;

  *result = CreateInternal(std::move(region));
  return MOJO_RESULT_OK;
}

// static
scoped_refptr<SharedBufferDispatcher> SharedBufferDispatcher::Deserialize(
    const void* bytes,
    size_t num_bytes,
    const ports::PortName* ports,
    size_t num_ports,
    ScopedPlatformHandle* platform_handles,
    size_t num_platform_handles) {
  if (num_bytes != sizeof(SerializedState)) {
    LOG(ERROR) << "Invalid serialized shared buffer dispatcher (bad size)";
    return nullptr;
  }

  const SerializedState* serialized_state =
      static_cast<const SerializedState*>(bytes);
  if (!serialized_state->num_bytes) {
    LOG(ERROR)
        << "Invalid serialized shared buffer dispatcher (invalid num_bytes)";
    return nullptr;
  }

  if (num_platform_handles != 1 || num_ports) {
    LOG(ERROR)
        << "Invalid serialized shared buffer dispatcher (missing handles)";
    return nullptr;
  }

  base::UnguessableToken guid = base::UnguessableToken::Deserialize(
      serialized_state->guid_high, serialized_state->guid_low);

  // TODO(https://crbug.com/826213): Support proper serialization and
  // deserialization of writable regions as well.
  base::subtle::PlatformSharedMemoryRegion::Mode mode =
      serialized_state->flags & kSerializedStateFlagsReadOnly
          ? base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly
          : base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe;
  auto region = base::subtle::PlatformSharedMemoryRegion::Take(
      CreateSharedMemoryRegionHandleFromPlatformHandles(
          std::move(platform_handles[0]), ScopedPlatformHandle()),
      mode, static_cast<size_t>(serialized_state->num_bytes), guid);
  if (!region.IsValid()) {
    LOG(ERROR)
        << "Invalid serialized shared buffer dispatcher (invalid num_bytes?)";
    return nullptr;
  }

  return CreateInternal(std::move(region));
}

base::subtle::PlatformSharedMemoryRegion
SharedBufferDispatcher::PassPlatformSharedMemoryRegion() {
  base::AutoLock lock(lock_);
  if (!region_.IsValid() || in_transit_)
    return base::subtle::PlatformSharedMemoryRegion();

  return std::move(region_);
}

Dispatcher::Type SharedBufferDispatcher::GetType() const {
  return Type::SHARED_BUFFER;
}

MojoResult SharedBufferDispatcher::Close() {
  base::AutoLock lock(lock_);
  if (in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;

  region_ = base::subtle::PlatformSharedMemoryRegion();
  return MOJO_RESULT_OK;
}

MojoResult SharedBufferDispatcher::DuplicateBufferHandle(
    const MojoDuplicateBufferHandleOptions* options,
    scoped_refptr<Dispatcher>* new_dispatcher) {
  MojoDuplicateBufferHandleOptions validated_options;
  MojoResult result = ValidateDuplicateOptions(options, &validated_options);
  if (result != MOJO_RESULT_OK)
    return result;

  base::AutoLock lock(lock_);
  if (in_transit_)
    return MOJO_RESULT_INVALID_ARGUMENT;

  if ((validated_options.flags & MOJO_DUPLICATE_BUFFER_HANDLE_FLAG_READ_ONLY)) {
    // If a read-only duplicate is requested and this handle is not already
    // read-only, we need to make it read-only before duplicating. If it's
    // unsafe it can't be made read-only, and we must fail instead.
    if (region_.GetMode() ==
        base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe) {
      return MOJO_RESULT_FAILED_PRECONDITION;
    } else if (region_.GetMode() ==
               base::subtle::PlatformSharedMemoryRegion::Mode::kWritable) {
      region_ = base::ReadOnlySharedMemoryRegion::TakeHandleForSerialization(
          base::WritableSharedMemoryRegion::ConvertToReadOnly(
              base::WritableSharedMemoryRegion::Deserialize(
                  std::move(region_))));
    }

    DCHECK_EQ(region_.GetMode(),
              base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly);
  } else {
    // A writable duplicate was requested. If this is already a read-only handle
    // we have to reject. Otherwise we have to convert to unsafe to ensure that
    // no future read-only duplication requests can succeed.
    if (region_.GetMode() ==
        base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly) {
      return MOJO_RESULT_FAILED_PRECONDITION;
    } else if (region_.GetMode() ==
               base::subtle::PlatformSharedMemoryRegion::Mode::kWritable) {
      auto handle = region_.PassPlatformHandle();
#if defined(OS_POSIX) && !defined(OS_ANDROID) && !defined(OS_FUCHSIA) && \
    (!defined(OS_MACOSX) || defined(OS_IOS))
      // On POSIX systems excluding Android, Fuchsia, and OSX, we explicitly
      // wipe out the secondary (read-only) FD from the platform handle to
      // repurpose it for exclusive unsafe usage.
      handle.readonly_fd.reset();
#endif
      region_ = base::subtle::PlatformSharedMemoryRegion::Take(
          std::move(handle),
          base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe,
          region_.GetSize(), region_.GetGUID());
    }
  }

  *new_dispatcher = CreateInternal(region_.Duplicate());
  return MOJO_RESULT_OK;
}

MojoResult SharedBufferDispatcher::MapBuffer(
    uint64_t offset,
    uint64_t num_bytes,
    std::unique_ptr<PlatformSharedMemoryMapping>* mapping) {
  if (offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (num_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return MOJO_RESULT_INVALID_ARGUMENT;

  base::AutoLock lock(lock_);
  DCHECK(region_.IsValid());
  if (in_transit_ || num_bytes == 0 ||
      static_cast<size_t>(offset + num_bytes) > region_.GetSize()) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  DCHECK(mapping);
  *mapping = std::make_unique<PlatformSharedMemoryMapping>(
      &region_, static_cast<size_t>(offset), static_cast<size_t>(num_bytes));
  if (!(*mapping)->IsValid()) {
    LOG(ERROR) << "Failed to map shared memory region.";
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult SharedBufferDispatcher::GetBufferInfo(MojoSharedBufferInfo* info) {
  if (!info)
    return MOJO_RESULT_INVALID_ARGUMENT;

  base::AutoLock lock(lock_);
  info->struct_size = sizeof(*info);
  info->size = region_.GetSize();
  return MOJO_RESULT_OK;
}

void SharedBufferDispatcher::StartSerialize(uint32_t* num_bytes,
                                            uint32_t* num_ports,
                                            uint32_t* num_platform_handles) {
  *num_bytes = sizeof(SerializedState);
  *num_ports = 0;
  *num_platform_handles = 1;
}

bool SharedBufferDispatcher::EndSerialize(void* destination,
                                          ports::PortName* ports,
                                          ScopedPlatformHandle* handles) {
  SerializedState* serialized_state =
      static_cast<SerializedState*>(destination);
  base::AutoLock lock(lock_);
  serialized_state->num_bytes = region_.GetSize();
  serialized_state->flags =
      region_.GetMode() ==
              base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly
          ? kSerializedStateFlagsReadOnly
          : 0;
  const base::UnguessableToken& guid = region_.GetGUID();
  serialized_state->guid_high = guid.GetHighForSerialization();
  serialized_state->guid_low = guid.GetLowForSerialization();
  serialized_state->padding = 0;

  ScopedPlatformHandle ignored_handle;
  ExtractPlatformHandlesFromSharedMemoryRegionHandle(
      region_.PassPlatformHandle(), &handles[0], &ignored_handle);
  region_ = base::subtle::PlatformSharedMemoryRegion();
  return true;
}

bool SharedBufferDispatcher::BeginTransit() {
  base::AutoLock lock(lock_);
  if (in_transit_)
    return false;
  in_transit_ = region_.IsValid();
  return in_transit_;
}

void SharedBufferDispatcher::CompleteTransitAndClose() {
  base::AutoLock lock(lock_);
  in_transit_ = false;
  region_ = base::subtle::PlatformSharedMemoryRegion();
}

void SharedBufferDispatcher::CancelTransit() {
  base::AutoLock lock(lock_);
  in_transit_ = false;
}

SharedBufferDispatcher::SharedBufferDispatcher(
    base::subtle::PlatformSharedMemoryRegion region)
    : region_(std::move(region)) {
  DCHECK(region_.IsValid());
}

SharedBufferDispatcher::~SharedBufferDispatcher() {
  DCHECK(!region_.IsValid() && !in_transit_);
}

// static
scoped_refptr<SharedBufferDispatcher> SharedBufferDispatcher::CreateInternal(
    base::subtle::PlatformSharedMemoryRegion region) {
  return base::WrapRefCounted(new SharedBufferDispatcher(std::move(region)));
}

// static
MojoResult SharedBufferDispatcher::ValidateDuplicateOptions(
    const MojoDuplicateBufferHandleOptions* in_options,
    MojoDuplicateBufferHandleOptions* out_options) {
  const MojoDuplicateBufferHandleFlags kKnownFlags =
      MOJO_DUPLICATE_BUFFER_HANDLE_FLAG_READ_ONLY;
  static const MojoDuplicateBufferHandleOptions kDefaultOptions = {
      static_cast<uint32_t>(sizeof(MojoDuplicateBufferHandleOptions)),
      MOJO_DUPLICATE_BUFFER_HANDLE_FLAG_NONE};

  *out_options = kDefaultOptions;
  if (!in_options)
    return MOJO_RESULT_OK;

  UserOptionsReader<MojoDuplicateBufferHandleOptions> reader(in_options);
  if (!reader.is_valid())
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (!OPTIONS_STRUCT_HAS_MEMBER(MojoDuplicateBufferHandleOptions, flags,
                                 reader))
    return MOJO_RESULT_OK;
  if ((reader.options().flags & ~kKnownFlags))
    return MOJO_RESULT_UNIMPLEMENTED;
  out_options->flags = reader.options().flags;

  // Checks for fields beyond |flags|:

  // (Nothing here yet.)

  return MOJO_RESULT_OK;
}

}  // namespace edk
}  // namespace mojo
