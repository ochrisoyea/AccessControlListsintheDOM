// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/embedder/entrypoints.h"

#include <stdint.h>

#include "mojo/edk/system/core.h"
#include "mojo/public/c/system/buffer.h"
#include "mojo/public/c/system/data_pipe.h"
#include "mojo/public/c/system/functions.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/c/system/platform_handle.h"

namespace {

mojo::edk::Core* g_core;

extern "C" {

MojoResult MojoInitializeImpl(const struct MojoInitializeOptions* options) {
  NOTREACHED() << "Do not call MojoInitialize() as an EDK embedder!";
  return MOJO_RESULT_OK;
}

MojoTimeTicks MojoGetTimeTicksNowImpl() {
  return g_core->GetTimeTicksNow();
}

MojoResult MojoCloseImpl(MojoHandle handle) {
  return g_core->Close(handle);
}

MojoResult MojoQueryHandleSignalsStateImpl(
    MojoHandle handle,
    MojoHandleSignalsState* signals_state) {
  return g_core->QueryHandleSignalsState(handle, signals_state);
}

MojoResult MojoCreateMessagePipeImpl(
    const MojoCreateMessagePipeOptions* options,
    MojoHandle* message_pipe_handle0,
    MojoHandle* message_pipe_handle1) {
  return g_core->CreateMessagePipe(options, message_pipe_handle0,
                                   message_pipe_handle1);
}

MojoResult MojoWriteMessageImpl(MojoHandle message_pipe_handle,
                                MojoMessageHandle message,
                                const MojoWriteMessageOptions* options) {
  return g_core->WriteMessage(message_pipe_handle, message, options);
}

MojoResult MojoReadMessageImpl(MojoHandle message_pipe_handle,
                               const MojoReadMessageOptions* options,
                               MojoMessageHandle* message) {
  return g_core->ReadMessage(message_pipe_handle, options, message);
}

MojoResult MojoFuseMessagePipesImpl(
    MojoHandle handle0,
    MojoHandle handle1,
    const MojoFuseMessagePipesOptions* options) {
  return g_core->FuseMessagePipes(handle0, handle1, options);
}

MojoResult MojoCreateMessageImpl(const MojoCreateMessageOptions* options,
                                 MojoMessageHandle* message) {
  return g_core->CreateMessage(options, message);
}

MojoResult MojoDestroyMessageImpl(MojoMessageHandle message) {
  return g_core->DestroyMessage(message);
}

MojoResult MojoSerializeMessageImpl(
    MojoMessageHandle message,
    const MojoSerializeMessageOptions* options) {
  return g_core->SerializeMessage(message, options);
}

MojoResult MojoAppendMessageDataImpl(
    MojoMessageHandle message,
    uint32_t additional_payload_size,
    const MojoHandle* handles,
    uint32_t num_handles,
    const MojoAppendMessageDataOptions* options,
    void** buffer,
    uint32_t* buffer_size) {
  return g_core->AppendMessageData(message, additional_payload_size, handles,
                                   num_handles, options, buffer, buffer_size);
}

MojoResult MojoGetMessageDataImpl(MojoMessageHandle message,
                                  const MojoGetMessageDataOptions* options,
                                  void** buffer,
                                  uint32_t* num_bytes,
                                  MojoHandle* handles,
                                  uint32_t* num_handles) {
  return g_core->GetMessageData(message, options, buffer, num_bytes, handles,
                                num_handles);
}

MojoResult MojoSetMessageContextImpl(
    MojoMessageHandle message,
    uintptr_t context,
    MojoMessageContextSerializer serializer,
    MojoMessageContextDestructor destructor,
    const MojoSetMessageContextOptions* options) {
  return g_core->SetMessageContext(message, context, serializer, destructor,
                                   options);
}

MojoResult MojoGetMessageContextImpl(
    MojoMessageHandle message,
    const MojoGetMessageContextOptions* options,
    uintptr_t* context) {
  return g_core->GetMessageContext(message, options, context);
}

MojoResult MojoNotifyBadMessageImpl(
    MojoMessageHandle message,
    const char* error,
    size_t error_num_bytes,
    const MojoNotifyBadMessageOptions* options) {
  return g_core->NotifyBadMessage(message, error, error_num_bytes, options);
}

MojoResult MojoCreateDataPipeImpl(const MojoCreateDataPipeOptions* options,
                                  MojoHandle* data_pipe_producer_handle,
                                  MojoHandle* data_pipe_consumer_handle) {
  return g_core->CreateDataPipe(options, data_pipe_producer_handle,
                                data_pipe_consumer_handle);
}

MojoResult MojoWriteDataImpl(MojoHandle data_pipe_producer_handle,
                             const void* elements,
                             uint32_t* num_elements,
                             const MojoWriteDataOptions* options) {
  return g_core->WriteData(data_pipe_producer_handle, elements, num_elements,
                           options);
}

MojoResult MojoBeginWriteDataImpl(MojoHandle data_pipe_producer_handle,
                                  const MojoBeginWriteDataOptions* options,
                                  void** buffer,
                                  uint32_t* buffer_num_elements) {
  return g_core->BeginWriteData(data_pipe_producer_handle, options, buffer,
                                buffer_num_elements);
}

MojoResult MojoEndWriteDataImpl(MojoHandle data_pipe_producer_handle,
                                uint32_t num_elements_written,
                                const MojoEndWriteDataOptions* options) {
  return g_core->EndWriteData(data_pipe_producer_handle, num_elements_written,
                              options);
}

MojoResult MojoReadDataImpl(MojoHandle data_pipe_consumer_handle,
                            const MojoReadDataOptions* options,
                            void* elements,
                            uint32_t* num_elements) {
  return g_core->ReadData(data_pipe_consumer_handle, options, elements,
                          num_elements);
}

MojoResult MojoBeginReadDataImpl(MojoHandle data_pipe_consumer_handle,
                                 const MojoBeginReadDataOptions* options,
                                 const void** buffer,
                                 uint32_t* buffer_num_elements) {
  return g_core->BeginReadData(data_pipe_consumer_handle, options, buffer,
                               buffer_num_elements);
}

MojoResult MojoEndReadDataImpl(MojoHandle data_pipe_consumer_handle,
                               uint32_t num_elements_read,
                               const MojoEndReadDataOptions* options) {
  return g_core->EndReadData(data_pipe_consumer_handle, num_elements_read,
                             options);
}

MojoResult MojoCreateSharedBufferImpl(
    uint64_t num_bytes,
    const MojoCreateSharedBufferOptions* options,
    MojoHandle* shared_buffer_handle) {
  return g_core->CreateSharedBuffer(num_bytes, options, shared_buffer_handle);
}

MojoResult MojoDuplicateBufferHandleImpl(
    MojoHandle buffer_handle,
    const MojoDuplicateBufferHandleOptions* options,
    MojoHandle* new_buffer_handle) {
  return g_core->DuplicateBufferHandle(buffer_handle, options,
                                       new_buffer_handle);
}

MojoResult MojoMapBufferImpl(MojoHandle buffer_handle,
                             uint64_t offset,
                             uint64_t num_bytes,
                             const MojoMapBufferOptions* options,
                             void** buffer) {
  return g_core->MapBuffer(buffer_handle, offset, num_bytes, options, buffer);
}

MojoResult MojoUnmapBufferImpl(void* buffer) {
  return g_core->UnmapBuffer(buffer);
}

MojoResult MojoGetBufferInfoImpl(MojoHandle buffer_handle,
                                 const MojoGetBufferInfoOptions* options,
                                 MojoSharedBufferInfo* info) {
  return g_core->GetBufferInfo(buffer_handle, options, info);
}

MojoResult MojoCreateTrapImpl(MojoTrapEventHandler handler,
                              const MojoCreateTrapOptions* options,
                              MojoHandle* trap_handle) {
  return g_core->CreateTrap(handler, options, trap_handle);
}

MojoResult MojoAddTriggerImpl(MojoHandle trap_handle,
                              MojoHandle handle,
                              MojoHandleSignals signals,
                              MojoTriggerCondition condition,
                              uintptr_t context,
                              const MojoAddTriggerOptions* options) {
  return g_core->AddTrigger(trap_handle, handle, signals, condition, context,
                            options);
}

MojoResult MojoRemoveTriggerImpl(MojoHandle trap_handle,
                                 uintptr_t context,
                                 const MojoRemoveTriggerOptions* options) {
  return g_core->RemoveTrigger(trap_handle, context, options);
}

MojoResult MojoArmTrapImpl(MojoHandle trap_handle,
                           const MojoArmTrapOptions* options,
                           uint32_t* num_ready_triggers,
                           uintptr_t* ready_triggers,
                           MojoResult* ready_results,
                           MojoHandleSignalsState* ready_signals_states) {
  return g_core->ArmTrap(trap_handle, options, num_ready_triggers,
                         ready_triggers, ready_results, ready_signals_states);
}

MojoResult MojoWrapPlatformHandleImpl(const MojoPlatformHandle* platform_handle,
                                      MojoHandle* mojo_handle) {
  return g_core->WrapPlatformHandle(platform_handle, mojo_handle);
}

MojoResult MojoUnwrapPlatformHandleImpl(MojoHandle mojo_handle,
                                        MojoPlatformHandle* platform_handle) {
  return g_core->UnwrapPlatformHandle(mojo_handle, platform_handle);
}

MojoResult MojoWrapPlatformSharedBufferHandleImpl(
    const MojoPlatformHandle* platform_handle,
    size_t num_bytes,
    const MojoSharedBufferGuid* guid,
    MojoPlatformSharedBufferHandleFlags flags,
    MojoHandle* mojo_handle) {
  return g_core->WrapPlatformSharedBufferHandle(platform_handle, num_bytes,
                                                guid, flags, mojo_handle);
}

MojoResult MojoUnwrapPlatformSharedBufferHandleImpl(
    MojoHandle mojo_handle,
    MojoPlatformHandle* platform_handle,
    size_t* num_bytes,
    MojoSharedBufferGuid* guid,
    MojoPlatformSharedBufferHandleFlags* flags) {
  return g_core->UnwrapPlatformSharedBufferHandle(mojo_handle, platform_handle,
                                                  num_bytes, guid, flags);
}

}  // extern "C"

MojoSystemThunks g_thunks = {sizeof(MojoSystemThunks),
                             MojoInitializeImpl,
                             MojoGetTimeTicksNowImpl,
                             MojoCloseImpl,
                             MojoQueryHandleSignalsStateImpl,
                             MojoCreateMessagePipeImpl,
                             MojoWriteMessageImpl,
                             MojoReadMessageImpl,
                             MojoFuseMessagePipesImpl,
                             MojoCreateMessageImpl,
                             MojoDestroyMessageImpl,
                             MojoSerializeMessageImpl,
                             MojoAppendMessageDataImpl,
                             MojoGetMessageDataImpl,
                             MojoSetMessageContextImpl,
                             MojoGetMessageContextImpl,
                             MojoNotifyBadMessageImpl,
                             MojoCreateDataPipeImpl,
                             MojoWriteDataImpl,
                             MojoBeginWriteDataImpl,
                             MojoEndWriteDataImpl,
                             MojoReadDataImpl,
                             MojoBeginReadDataImpl,
                             MojoEndReadDataImpl,
                             MojoCreateSharedBufferImpl,
                             MojoDuplicateBufferHandleImpl,
                             MojoMapBufferImpl,
                             MojoUnmapBufferImpl,
                             MojoGetBufferInfoImpl,
                             MojoCreateTrapImpl,
                             MojoAddTriggerImpl,
                             MojoRemoveTriggerImpl,
                             MojoArmTrapImpl,
                             MojoWrapPlatformHandleImpl,
                             MojoUnwrapPlatformHandleImpl,
                             MojoWrapPlatformSharedBufferHandleImpl,
                             MojoUnwrapPlatformSharedBufferHandleImpl};

}  // namespace

namespace mojo {
namespace edk {

// static
Core* Core::Get() {
  return g_core;
}

void InitializeCore() {
  g_core = new Core;
}

const MojoSystemThunks& GetSystemThunks() {
  return g_thunks;
}

}  // namespace edk
}  // namespace mojo
