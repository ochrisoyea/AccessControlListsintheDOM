// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Note: This header should be compilable as C.

#ifndef MOJO_PUBLIC_C_SYSTEM_THUNKS_H_
#define MOJO_PUBLIC_C_SYSTEM_THUNKS_H_

#include <stddef.h>
#include <stdint.h>

#include "mojo/public/c/system/core.h"
#include "mojo/public/c/system/system_export.h"

// Structure used to bind the basic Mojo Core functions to an embedder
// implementation. This is intended to eventually be used as a stable ABI
// between a Mojo embedder and some loaded application code, but for now it is
// still effectively safe to rearrange entries as needed.
#pragma pack(push, 8)
struct MojoSystemThunks {
  size_t size;  // Should be set to sizeof(MojoSystemThunks).

  MojoResult (*Initialize)(const struct MojoInitializeOptions* options);

  MojoTimeTicks (*GetTimeTicksNow)();

  // Generic handle API.
  MojoResult (*Close)(MojoHandle handle);
  MojoResult (*QueryHandleSignalsState)(
      MojoHandle handle,
      struct MojoHandleSignalsState* signals_state);

  // Message pipe API.
  MojoResult (*CreateMessagePipe)(
      const struct MojoCreateMessagePipeOptions* options,
      MojoHandle* message_pipe_handle0,
      MojoHandle* message_pipe_handle1);
  MojoResult (*WriteMessage)(MojoHandle message_pipe_handle,
                             MojoMessageHandle message_handle,
                             const struct MojoWriteMessageOptions* options);
  MojoResult (*ReadMessage)(MojoHandle message_pipe_handle,
                            const struct MojoReadMessageOptions* options,
                            MojoMessageHandle* message_handle);
  MojoResult (*FuseMessagePipes)(
      MojoHandle handle0,
      MojoHandle handle1,
      const struct MojoFuseMessagePipesOptions* options);

  // Message object API.
  MojoResult (*CreateMessage)(const struct MojoCreateMessageOptions* options,
                              MojoMessageHandle* message);
  MojoResult (*DestroyMessage)(MojoMessageHandle message);
  MojoResult (*SerializeMessage)(
      MojoMessageHandle message,
      const struct MojoSerializeMessageOptions* options);
  MojoResult (*AppendMessageData)(
      MojoMessageHandle message,
      uint32_t additional_payload_size,
      const MojoHandle* handles,
      uint32_t num_handles,
      const struct MojoAppendMessageDataOptions* options,
      void** buffer,
      uint32_t* buffer_size);
  MojoResult (*GetMessageData)(MojoMessageHandle message,
                               const struct MojoGetMessageDataOptions* options,
                               void** buffer,
                               uint32_t* num_bytes,
                               MojoHandle* handles,
                               uint32_t* num_handles);
  MojoResult (*SetMessageContext)(
      MojoMessageHandle message,
      uintptr_t context,
      MojoMessageContextSerializer serializer,
      MojoMessageContextDestructor destructor,
      const struct MojoSetMessageContextOptions* options);
  MojoResult (*GetMessageContext)(
      MojoMessageHandle message,
      const struct MojoGetMessageContextOptions* options,
      uintptr_t* context);
  MojoResult (*NotifyBadMessage)(
      MojoMessageHandle message,
      const char* error,
      size_t error_num_bytes,
      const struct MojoNotifyBadMessageOptions* options);

  // Data pipe API.
  MojoResult (*CreateDataPipe)(const struct MojoCreateDataPipeOptions* options,
                               MojoHandle* data_pipe_producer_handle,
                               MojoHandle* data_pipe_consumer_handle);
  MojoResult (*WriteData)(MojoHandle data_pipe_producer_handle,
                          const void* elements,
                          uint32_t* num_elements,
                          const struct MojoWriteDataOptions* options);
  MojoResult (*BeginWriteData)(MojoHandle data_pipe_producer_handle,
                               const struct MojoBeginWriteDataOptions* options,
                               void** buffer,
                               uint32_t* buffer_num_elements);
  MojoResult (*EndWriteData)(MojoHandle data_pipe_producer_handle,
                             uint32_t num_elements_written,
                             const struct MojoEndWriteDataOptions* options);
  MojoResult (*ReadData)(MojoHandle data_pipe_consumer_handle,
                         const struct MojoReadDataOptions* options,
                         void* elements,
                         uint32_t* num_elements);
  MojoResult (*BeginReadData)(MojoHandle data_pipe_consumer_handle,
                              const struct MojoBeginReadDataOptions* options,
                              const void** buffer,
                              uint32_t* buffer_num_elements);
  MojoResult (*EndReadData)(MojoHandle data_pipe_consumer_handle,
                            uint32_t num_elements_read,
                            const struct MojoEndReadDataOptions* options);

  // Shared buffer API.
  MojoResult (*CreateSharedBuffer)(
      uint64_t num_bytes,
      const struct MojoCreateSharedBufferOptions* options,
      MojoHandle* shared_buffer_handle);
  MojoResult (*DuplicateBufferHandle)(
      MojoHandle buffer_handle,
      const struct MojoDuplicateBufferHandleOptions* options,
      MojoHandle* new_buffer_handle);
  MojoResult (*MapBuffer)(MojoHandle buffer_handle,
                          uint64_t offset,
                          uint64_t num_bytes,
                          const struct MojoMapBufferOptions* options,
                          void** buffer);
  MojoResult (*UnmapBuffer)(void* buffer);
  MojoResult (*GetBufferInfo)(MojoHandle buffer_handle,
                              const struct MojoGetBufferInfoOptions* options,
                              struct MojoSharedBufferInfo* info);

  // Traps API.
  MojoResult (*CreateTrap)(MojoTrapEventHandler handler,
                           const struct MojoCreateTrapOptions* options,
                           MojoHandle* trap_handle);
  MojoResult (*AddTrigger)(MojoHandle trap_handle,
                           MojoHandle handle,
                           MojoHandleSignals signals,
                           MojoTriggerCondition condition,
                           uintptr_t context,
                           const struct MojoAddTriggerOptions* options);
  MojoResult (*RemoveTrigger)(MojoHandle trap_handle,
                              uintptr_t context,
                              const struct MojoRemoveTriggerOptions* options);
  MojoResult (*ArmTrap)(MojoHandle trap_handle,
                        const struct MojoArmTrapOptions* options,
                        uint32_t* num_ready_triggers,
                        uintptr_t* ready_triggers,
                        MojoResult* ready_results,
                        MojoHandleSignalsState* ready_signals_states);

  // Platform handle API.
  MojoResult (*WrapPlatformHandle)(
      const struct MojoPlatformHandle* platform_handle,
      MojoHandle* mojo_handle);
  MojoResult (*UnwrapPlatformHandle)(
      MojoHandle mojo_handle,
      struct MojoPlatformHandle* platform_handle);
  MojoResult (*WrapPlatformSharedBufferHandle)(
      const struct MojoPlatformHandle* platform_handle,
      size_t num_bytes,
      const struct MojoSharedBufferGuid* guid,
      MojoPlatformSharedBufferHandleFlags flags,
      MojoHandle* mojo_handle);
  MojoResult (*UnwrapPlatformSharedBufferHandle)(
      MojoHandle mojo_handle,
      struct MojoPlatformHandle* platform_handle,
      size_t* num_bytes,
      struct MojoSharedBufferGuid* guid,
      MojoPlatformSharedBufferHandleFlags* flags);
};
#pragma pack(pop)

// A function for setting up the embedder's own system thunks. This should only
// be called by Mojo embedder code.
MOJO_SYSTEM_EXPORT void MojoEmbedderSetSystemThunks(
    const struct MojoSystemThunks* system_thunks);

#endif  // MOJO_PUBLIC_C_SYSTEM_THUNKS_H_
