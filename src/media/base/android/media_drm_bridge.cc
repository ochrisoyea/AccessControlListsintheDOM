// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/android/media_drm_bridge.h"

#include <stddef.h>
#include <algorithm>
#include <memory>
#include <utility>

#include "base/android/build_info.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/containers/hash_tables.h"
#include "base/feature_list.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/sys_byteorder.h"
#include "base/sys_info.h"
#include "base/threading/thread_task_runner_handle.h"
#include "jni/MediaDrmBridge_jni.h"
#include "media/base/android/android_util.h"
#include "media/base/android/media_codec_util.h"
#include "media/base/android/media_drm_bridge_client.h"
#include "media/base/android/media_drm_bridge_delegate.h"
#include "media/base/cdm_key_information.h"
#include "media/base/media_switches.h"
#include "media/base/provision_fetcher.h"

// In SHARED_INTERMEDIATE_DIR
#include "widevine_cdm_version.h"  // NOLINT(build/include)

using base::android::AttachCurrentThread;
using base::android::ConvertUTF8ToJavaString;
using base::android::ConvertJavaStringToUTF8;
using base::android::JavaByteArrayToByteVector;
using base::android::JavaParamRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;

namespace media {

namespace {

using CreateMediaDrmBridgeCB = base::OnceCallback<scoped_refptr<MediaDrmBridge>(
    const std::string& /* origin_id */)>;

// These must be in sync with Android MediaDrm REQUEST_TYPE_XXX constants!
// https://developer.android.com/reference/android/media/MediaDrm.KeyRequest.html
enum class RequestType : uint32_t {
  REQUEST_TYPE_INITIAL = 0,
  REQUEST_TYPE_RENEWAL = 1,
  REQUEST_TYPE_RELEASE = 2,
};

// These must be in sync with Android MediaDrm KEY_STATUS_XXX constants:
// https://developer.android.com/reference/android/media/MediaDrm.KeyStatus.html
enum class KeyStatus : uint32_t {
  KEY_STATUS_USABLE = 0,
  KEY_STATUS_EXPIRED = 1,
  KEY_STATUS_OUTPUT_NOT_ALLOWED = 2,
  KEY_STATUS_PENDING = 3,
  KEY_STATUS_INTERNAL_ERROR = 4,
};

// These must be in sync with Android MediaDrm KEY_TYPE_XXX constants:
// https://developer.android.com/reference/android/media/MediaDrm.html#KEY_TYPE_OFFLINE
// KEY_TYPE_RELEASE is handled internally in Java.
enum class KeyType : uint32_t {
  KEY_TYPE_STREAMING = 1,
  KEY_TYPE_OFFLINE = 2,
};

const uint8_t kWidevineUuid[16] = {
    0xED, 0xEF, 0x8B, 0xA9, 0x79, 0xD6, 0x4A, 0xCE,  //
    0xA3, 0xC8, 0x27, 0xDC, 0xD5, 0x1D, 0x21, 0xED};

// Convert |init_data_type| to a string supported by MediaDRM.
// "audio"/"video" does not matter, so use "video".
std::string ConvertInitDataType(media::EmeInitDataType init_data_type) {
  // TODO(jrummell/xhwang): EME init data types like "webm" and "cenc" are
  // supported in API level >=21 for Widevine key system. Switch to use those
  // strings when they are officially supported in Android for all key systems.
  switch (init_data_type) {
    case media::EmeInitDataType::WEBM:
      return "video/webm";
    case media::EmeInitDataType::CENC:
      return "video/mp4";
    case media::EmeInitDataType::KEYIDS:
      return "keyids";
    default:
      NOTREACHED();
      return "unknown";
  }
}

// Convert CdmSessionType to KeyType supported by MediaDrm.
KeyType ConvertCdmSessionType(CdmSessionType session_type) {
  switch (session_type) {
    case CdmSessionType::TEMPORARY_SESSION:
      return KeyType::KEY_TYPE_STREAMING;
    case CdmSessionType::PERSISTENT_LICENSE_SESSION:
      return KeyType::KEY_TYPE_OFFLINE;

    default:
      LOG(WARNING) << "Unsupported session type "
                   << static_cast<int>(session_type);
      return KeyType::KEY_TYPE_STREAMING;
  }
}

CdmMessageType GetMessageType(RequestType request_type) {
  switch (request_type) {
    case RequestType::REQUEST_TYPE_INITIAL:
      return CdmMessageType::LICENSE_REQUEST;
    case RequestType::REQUEST_TYPE_RENEWAL:
      return CdmMessageType::LICENSE_RENEWAL;
    case RequestType::REQUEST_TYPE_RELEASE:
      return CdmMessageType::LICENSE_RELEASE;
  }

  NOTREACHED();
  return CdmMessageType::LICENSE_REQUEST;
}

CdmKeyInformation::KeyStatus ConvertKeyStatus(KeyStatus key_status,
                                              bool is_key_release) {
  switch (key_status) {
    case KeyStatus::KEY_STATUS_USABLE:
      return CdmKeyInformation::USABLE;
    case KeyStatus::KEY_STATUS_EXPIRED:
      return is_key_release ? CdmKeyInformation::RELEASED
                            : CdmKeyInformation::EXPIRED;
    case KeyStatus::KEY_STATUS_OUTPUT_NOT_ALLOWED:
      return CdmKeyInformation::OUTPUT_RESTRICTED;
    case KeyStatus::KEY_STATUS_PENDING:
      // TODO(xhwang): This should probably be renamed to "PENDING".
      return CdmKeyInformation::KEY_STATUS_PENDING;
    case KeyStatus::KEY_STATUS_INTERNAL_ERROR:
      return CdmKeyInformation::INTERNAL_ERROR;
  }

  NOTREACHED();
  return CdmKeyInformation::INTERNAL_ERROR;
}

class KeySystemManager {
 public:
  KeySystemManager();
  UUID GetUUID(const std::string& key_system);
  std::vector<std::string> GetPlatformKeySystemNames();

 private:
  using KeySystemUuidMap = MediaDrmBridgeClient::KeySystemUuidMap;

  KeySystemUuidMap key_system_uuid_map_;

  DISALLOW_COPY_AND_ASSIGN(KeySystemManager);
};

KeySystemManager::KeySystemManager() {
  // Widevine is always supported in Android.
  key_system_uuid_map_[kWidevineKeySystem] =
      UUID(kWidevineUuid, kWidevineUuid + arraysize(kWidevineUuid));
  MediaDrmBridgeClient* client = GetMediaDrmBridgeClient();
  if (client)
    client->AddKeySystemUUIDMappings(&key_system_uuid_map_);
}

UUID KeySystemManager::GetUUID(const std::string& key_system) {
  KeySystemUuidMap::iterator it = key_system_uuid_map_.find(key_system);
  if (it == key_system_uuid_map_.end())
    return UUID();
  return it->second;
}

std::vector<std::string> KeySystemManager::GetPlatformKeySystemNames() {
  std::vector<std::string> key_systems;
  for (KeySystemUuidMap::iterator it = key_system_uuid_map_.begin();
       it != key_system_uuid_map_.end(); ++it) {
    // Rule out the key system handled by Chrome explicitly.
    if (it->first != kWidevineKeySystem)
      key_systems.push_back(it->first);
  }
  return key_systems;
}

KeySystemManager* GetKeySystemManager() {
  static KeySystemManager* ksm = new KeySystemManager();
  return ksm;
}

// Checks whether |key_system| is supported with |container_mime_type|. Only
// checks |key_system| support if |container_mime_type| is empty.
// TODO(xhwang): The |container_mime_type| is not the same as contentType in
// the EME spec. Revisit this once the spec issue with initData type is
// resolved.
bool IsKeySystemSupportedWithTypeImpl(const std::string& key_system,
                                      const std::string& container_mime_type) {
  DCHECK(MediaDrmBridge::IsAvailable());

  if (key_system.empty()) {
    NOTREACHED();
    return false;
  }

  UUID scheme_uuid = GetKeySystemManager()->GetUUID(key_system);
  if (scheme_uuid.empty())
    return false;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_scheme_uuid =
      base::android::ToJavaByteArray(env, &scheme_uuid[0], scheme_uuid.size());
  ScopedJavaLocalRef<jstring> j_container_mime_type =
      ConvertUTF8ToJavaString(env, container_mime_type);
  return Java_MediaDrmBridge_isCryptoSchemeSupported(env, j_scheme_uuid,
                                                     j_container_mime_type);
}

MediaDrmBridge::SecurityLevel GetSecurityLevelFromString(
    const std::string& security_level_str) {
  if (0 == security_level_str.compare("L1"))
    return MediaDrmBridge::SECURITY_LEVEL_1;
  if (0 == security_level_str.compare("L3"))
    return MediaDrmBridge::SECURITY_LEVEL_3;
  DCHECK(security_level_str.empty());
  return MediaDrmBridge::SECURITY_LEVEL_DEFAULT;
}

// Do not change the return values as they are part of Android MediaDrm API for
// Widevine.
std::string GetSecurityLevelString(
    MediaDrmBridge::SecurityLevel security_level) {
  switch (security_level) {
    case MediaDrmBridge::SECURITY_LEVEL_DEFAULT:
      return "";
    case MediaDrmBridge::SECURITY_LEVEL_1:
      return "L1";
    case MediaDrmBridge::SECURITY_LEVEL_3:
      return "L3";
  }
  return "";
}

bool AreMediaDrmApisAvailable() {
  if (base::android::BuildInfo::GetInstance()->sdk_int() <
      base::android::SDK_VERSION_KITKAT)
    return false;

  int32_t os_major_version = 0;
  int32_t os_minor_version = 0;
  int32_t os_bugfix_version = 0;
  base::SysInfo::OperatingSystemVersionNumbers(
      &os_major_version, &os_minor_version, &os_bugfix_version);
  if (os_major_version == 4 && os_minor_version == 4 && os_bugfix_version == 0)
    return false;

  return true;
}

bool IsPersistentLicenseTypeSupportedByMediaDrm() {
  return MediaDrmBridge::IsAvailable() &&
         // In development. See http://crbug.com/493521
         base::FeatureList::IsEnabled(kMediaDrmPersistentLicense) &&
         base::android::BuildInfo::GetInstance()->sdk_int() >=
             base::android::SDK_VERSION_MARSHMALLOW;
}

// Callback for MediaDrmStorageBridge::Initialize.
// |create_media_drm_bridge_cb|, factory method to create MediaDrmBridge.
// |created_cb|, callback to return the MediaDrmBridge to caller of
// MediaDrmBridge::Create.
void OnStorageInitialized(CreateMediaDrmBridgeCB create_media_drm_bridge_cb,
                          MediaDrmBridge::CreatedCB created_cb,
                          MediaDrmStorageBridge* storage) {
  DCHECK(storage);

  // MediaDrmStorageBridge should always return a valid origin ID after
  // initialize. Otherwise the pipe is broken and we should not create
  // MediaDrmBridge here.
  std::move(created_cb)
      .Run(storage->origin_id().empty() ? nullptr
                                        : std::move(create_media_drm_bridge_cb)
                                              .Run(storage->origin_id()));
}

}  // namespace

// MediaDrm is not generally usable without MediaCodec. Thus, both the MediaDrm
// APIs and MediaCodec APIs must be enabled and not blacklisted.
// static
bool MediaDrmBridge::IsAvailable() {
  return AreMediaDrmApisAvailable() && MediaCodecUtil::IsMediaCodecAvailable();
}

// static
bool MediaDrmBridge::IsKeySystemSupported(const std::string& key_system) {
  if (!MediaDrmBridge::IsAvailable())
    return false;

  return IsKeySystemSupportedWithTypeImpl(key_system, "");
}

// static
bool MediaDrmBridge::IsPersistentLicenseTypeSupported(
    const std::string& key_system) {
  // TODO(yucliu): Check |key_system| if persistent license is supported by
  // MediaDrm.
  return IsPersistentLicenseTypeSupportedByMediaDrm();
}

// static
bool MediaDrmBridge::IsKeySystemSupportedWithType(
    const std::string& key_system,
    const std::string& container_mime_type) {
  DCHECK(!container_mime_type.empty()) << "Call IsKeySystemSupported instead";

  if (!MediaDrmBridge::IsAvailable())
    return false;

  return IsKeySystemSupportedWithTypeImpl(key_system, container_mime_type);
}

// static
std::vector<std::string> MediaDrmBridge::GetPlatformKeySystemNames() {
  if (!MediaDrmBridge::IsAvailable())
    return std::vector<std::string>();

  return GetKeySystemManager()->GetPlatformKeySystemNames();
}

// static
scoped_refptr<MediaDrmBridge> MediaDrmBridge::CreateInternal(
    const std::vector<uint8_t>& scheme_uuid,
    SecurityLevel security_level,
    std::unique_ptr<MediaDrmStorageBridge> storage,
    const CreateFetcherCB& create_fetcher_cb,
    const SessionMessageCB& session_message_cb,
    const SessionClosedCB& session_closed_cb,
    const SessionKeysChangeCB& session_keys_change_cb,
    const SessionExpirationUpdateCB& session_expiration_update_cb,
    const std::string& origin_id) {
  // All paths requires the MediaDrmApis.
  DCHECK(AreMediaDrmApisAvailable());
  DCHECK(!scheme_uuid.empty());

  scoped_refptr<MediaDrmBridge> media_drm_bridge(new MediaDrmBridge(
      scheme_uuid, origin_id, security_level, std::move(storage),
      create_fetcher_cb, session_message_cb, session_closed_cb,
      session_keys_change_cb, session_expiration_update_cb));

  if (media_drm_bridge->j_media_drm_.is_null())
    return nullptr;

  return media_drm_bridge;
}

// static
void MediaDrmBridge::Create(
    const std::string& key_system,
    const url::Origin& security_origin,
    SecurityLevel security_level,
    const CreateFetcherCB& create_fetcher_cb,
    const CreateStorageCB& create_storage_cb,
    const SessionMessageCB& session_message_cb,
    const SessionClosedCB& session_closed_cb,
    const SessionKeysChangeCB& session_keys_change_cb,
    const SessionExpirationUpdateCB& session_expiration_update_cb,
    CreatedCB created_cb) {
  DVLOG(1) << __func__;

  if (!IsAvailable()) {
    std::move(created_cb).Run(nullptr);
    return;
  }

  UUID scheme_uuid = GetKeySystemManager()->GetUUID(key_system);
  if (scheme_uuid.empty()) {
    std::move(created_cb).Run(nullptr);
    return;
  }

  // MediaDrmStorage may be lazy created in MediaDrmStorageBridge.
  auto storage = std::make_unique<MediaDrmStorageBridge>();
  MediaDrmStorageBridge* raw_storage = storage.get();

  CreateMediaDrmBridgeCB create_media_drm_bridge_cb = base::BindOnce(
      &MediaDrmBridge::CreateInternal, scheme_uuid, security_level,
      std::move(storage), create_fetcher_cb, session_message_cb,
      session_closed_cb, session_keys_change_cb, session_expiration_update_cb);

  if (IsPersistentLicenseTypeSupported(key_system) &&
      !security_origin.unique() && !create_storage_cb.is_null()) {
    raw_storage->Initialize(
        create_storage_cb, base::BindOnce(&OnStorageInitialized,
                                          std::move(create_media_drm_bridge_cb),
                                          std::move(created_cb), raw_storage));
  } else {
    std::move(created_cb).Run(std::move(create_media_drm_bridge_cb).Run(""));
  }
}

// static
scoped_refptr<MediaDrmBridge> MediaDrmBridge::CreateWithoutSessionSupport(
    const std::string& key_system,
    const std::string& origin_id,
    SecurityLevel security_level,
    const CreateFetcherCB& create_fetcher_cb) {
  DVLOG(1) << __func__;

  // Sessions won't be used so decoding capability is not required.
  if (!AreMediaDrmApisAvailable()) {
    return nullptr;
  }

  UUID scheme_uuid = GetKeySystemManager()->GetUUID(key_system);
  if (scheme_uuid.empty()) {
    return nullptr;
  }

  return CreateInternal(
      scheme_uuid, security_level, std::make_unique<MediaDrmStorageBridge>(),
      create_fetcher_cb, SessionMessageCB(), SessionClosedCB(),
      SessionKeysChangeCB(), SessionExpirationUpdateCB(), origin_id);
}

void MediaDrmBridge::SetServerCertificate(
    const std::vector<uint8_t>& certificate,
    std::unique_ptr<media::SimpleCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__ << "(" << certificate.size() << " bytes)";

  DCHECK(!certificate.empty());

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_certificate = base::android::ToJavaByteArray(
      env, certificate.data(), certificate.size());
  if (Java_MediaDrmBridge_setServerCertificate(env, j_media_drm_,
                                               j_certificate)) {
    promise->resolve();
  } else {
    promise->reject(CdmPromise::Exception::TYPE_ERROR, 0,
                    "Set server certificate failed.");
  }
}

void MediaDrmBridge::CreateSessionAndGenerateRequest(
    CdmSessionType session_type,
    media::EmeInitDataType init_data_type,
    const std::vector<uint8_t>& init_data,
    std::unique_ptr<media::NewSessionCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_init_data;
  ScopedJavaLocalRef<jobjectArray> j_optional_parameters;

  MediaDrmBridgeClient* client = GetMediaDrmBridgeClient();
  if (client) {
    MediaDrmBridgeDelegate* delegate =
        client->GetMediaDrmBridgeDelegate(scheme_uuid_);
    if (delegate) {
      std::vector<uint8_t> init_data_from_delegate;
      std::vector<std::string> optional_parameters_from_delegate;
      if (!delegate->OnCreateSession(init_data_type, init_data,
                                     &init_data_from_delegate,
                                     &optional_parameters_from_delegate)) {
        promise->reject(CdmPromise::Exception::TYPE_ERROR, 0,
                        "Invalid init data.");
        return;
      }
      if (!init_data_from_delegate.empty()) {
        j_init_data =
            base::android::ToJavaByteArray(env, init_data_from_delegate.data(),
                                           init_data_from_delegate.size());
      }
      if (!optional_parameters_from_delegate.empty()) {
        j_optional_parameters = base::android::ToJavaArrayOfStrings(
            env, optional_parameters_from_delegate);
      }
    }
  }

  if (j_init_data.is_null()) {
    j_init_data =
        base::android::ToJavaByteArray(env, init_data.data(), init_data.size());
  }

  ScopedJavaLocalRef<jstring> j_mime =
      ConvertUTF8ToJavaString(env, ConvertInitDataType(init_data_type));
  uint32_t key_type =
      static_cast<uint32_t>(ConvertCdmSessionType(session_type));
  uint32_t promise_id = cdm_promise_adapter_.SavePromise(std::move(promise));
  Java_MediaDrmBridge_createSessionFromNative(
      env, j_media_drm_, j_init_data, j_mime, key_type, j_optional_parameters,
      promise_id);
}

void MediaDrmBridge::LoadSession(
    CdmSessionType session_type,
    const std::string& session_id,
    std::unique_ptr<media::NewSessionCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__;

  DCHECK(IsPersistentLicenseTypeSupportedByMediaDrm());

  if (session_type != CdmSessionType::PERSISTENT_LICENSE_SESSION) {
    promise->reject(
        CdmPromise::Exception::NOT_SUPPORTED_ERROR, 0,
        "LoadSession() is only supported for 'persistent-license'.");
    return;
  }

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_session_id =
      StringToJavaBytes(env, session_id);
  uint32_t promise_id = cdm_promise_adapter_.SavePromise(std::move(promise));
  Java_MediaDrmBridge_loadSession(env, j_media_drm_, j_session_id, promise_id);
}

void MediaDrmBridge::UpdateSession(
    const std::string& session_id,
    const std::vector<uint8_t>& response,
    std::unique_ptr<media::SimpleCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_response =
      base::android::ToJavaByteArray(env, response.data(), response.size());
  ScopedJavaLocalRef<jbyteArray> j_session_id =
      StringToJavaBytes(env, session_id);
  uint32_t promise_id = cdm_promise_adapter_.SavePromise(std::move(promise));
  Java_MediaDrmBridge_updateSession(env, j_media_drm_, j_session_id, j_response,
                                    promise_id);
}

void MediaDrmBridge::CloseSession(
    const std::string& session_id,
    std::unique_ptr<media::SimpleCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_session_id =
      StringToJavaBytes(env, session_id);
  uint32_t promise_id = cdm_promise_adapter_.SavePromise(std::move(promise));
  Java_MediaDrmBridge_closeSession(env, j_media_drm_, j_session_id, promise_id);
}

void MediaDrmBridge::RemoveSession(
    const std::string& session_id,
    std::unique_ptr<media::SimpleCdmPromise> promise) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(2) << __func__;

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jbyteArray> j_session_id =
      StringToJavaBytes(env, session_id);
  uint32_t promise_id = cdm_promise_adapter_.SavePromise(std::move(promise));
  Java_MediaDrmBridge_removeSession(env, j_media_drm_, j_session_id,
                                    promise_id);
}

CdmContext* MediaDrmBridge::GetCdmContext() {
  DVLOG(2) << __func__;
  return this;
}

void MediaDrmBridge::DeleteOnCorrectThread() const {
  DVLOG(1) << __func__;

  if (!task_runner_->BelongsToCurrentThread()) {
    // When DeleteSoon returns false, |this| will be leaked, which is okay.
    task_runner_->DeleteSoon(FROM_HERE, this);
  } else {
    delete this;
  }
}

MediaCryptoContext* MediaDrmBridge::GetMediaCryptoContext() {
  DVLOG(2) << __func__;
  return &media_crypto_context_;
}

int MediaDrmBridge::RegisterPlayer(const base::Closure& new_key_cb,
                                   const base::Closure& cdm_unset_cb) {
  // |player_tracker_| can be accessed from any thread.
  return player_tracker_.RegisterPlayer(new_key_cb, cdm_unset_cb);
}

void MediaDrmBridge::UnregisterPlayer(int registration_id) {
  // |player_tracker_| can be accessed from any thread.
  player_tracker_.UnregisterPlayer(registration_id);
}

bool MediaDrmBridge::IsSecureCodecRequired() {
  DCHECK(IsAvailable());

  // For Widevine, this depends on the security level.
  // TODO(xhwang): This is specific to Widevine. See http://crbug.com/459400.
  // To fix it, we could call MediaCrypto.requiresSecureDecoderComponent().
  // See http://crbug.com/727918.
  if (std::equal(scheme_uuid_.begin(), scheme_uuid_.end(), kWidevineUuid))
    return SECURITY_LEVEL_1 == GetSecurityLevel();

  // For other key systems, assume true.
  return true;
}

void MediaDrmBridge::ResetDeviceCredentials(
    const ResetCredentialsCB& callback) {
  DVLOG(1) << __func__;

  DCHECK(reset_credentials_cb_.is_null());
  reset_credentials_cb_ = callback;
  JNIEnv* env = AttachCurrentThread();
  Java_MediaDrmBridge_resetDeviceCredentials(env, j_media_drm_);
}

void MediaDrmBridge::Unprovision() {
  DVLOG(1) << __func__;

  JNIEnv* env = AttachCurrentThread();
  Java_MediaDrmBridge_unprovision(env, j_media_drm_);
}

void MediaDrmBridge::ResolvePromise(uint32_t promise_id) {
  DVLOG(2) << __func__;
  cdm_promise_adapter_.ResolvePromise(promise_id);
}

void MediaDrmBridge::ResolvePromiseWithSession(uint32_t promise_id,
                                               const std::string& session_id) {
  DVLOG(2) << __func__;
  cdm_promise_adapter_.ResolvePromise(promise_id, session_id);
}

void MediaDrmBridge::RejectPromise(uint32_t promise_id,
                                   const std::string& error_message) {
  DVLOG(2) << __func__;
  cdm_promise_adapter_.RejectPromise(
      promise_id, CdmPromise::Exception::NOT_SUPPORTED_ERROR, 0, error_message);
}

void MediaDrmBridge::SetMediaCryptoReadyCB(
    const MediaCryptoReadyCB& media_crypto_ready_cb) {
  if (!task_runner_->BelongsToCurrentThread()) {
    task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&MediaDrmBridge::SetMediaCryptoReadyCB,
                   weak_factory_.GetWeakPtr(), media_crypto_ready_cb));
    return;
  }

  DVLOG(1) << __func__;

  if (media_crypto_ready_cb.is_null()) {
    media_crypto_ready_cb_.Reset();
    return;
  }

  DCHECK(media_crypto_ready_cb_.is_null());
  media_crypto_ready_cb_ = media_crypto_ready_cb;

  if (!j_media_crypto_)
    return;

  base::ResetAndReturn(&media_crypto_ready_cb_)
      .Run(CreateJavaObjectPtr(j_media_crypto_->obj()),
           IsSecureCodecRequired());
}

//------------------------------------------------------------------------------
// The following OnXxx functions are called from Java. The implementation must
// only do minimal work and then post tasks to avoid reentrancy issues.

void MediaDrmBridge::OnMediaCryptoReady(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jobject>& j_media_crypto) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __func__;

  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&MediaDrmBridge::NotifyMediaCryptoReady,
                 weak_factory_.GetWeakPtr(),
                 base::Passed(CreateJavaObjectPtr(j_media_crypto.obj()))));
}

void MediaDrmBridge::OnStartProvisioning(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jstring>& j_default_url,
    const JavaParamRef<jbyteArray>& j_request_data) {
  DVLOG(1) << __func__;

  task_runner_->PostTask(FROM_HERE,
                         base::Bind(&MediaDrmBridge::SendProvisioningRequest,
                                    weak_factory_.GetWeakPtr(),
                                    ConvertJavaStringToUTF8(env, j_default_url),
                                    JavaBytesToString(env, j_request_data)));
}

void MediaDrmBridge::OnPromiseResolved(JNIEnv* env,
                                       const JavaParamRef<jobject>& j_media_drm,
                                       jint j_promise_id) {
  task_runner_->PostTask(FROM_HERE,
                         base::Bind(&MediaDrmBridge::ResolvePromise,
                                    weak_factory_.GetWeakPtr(), j_promise_id));
}

void MediaDrmBridge::OnPromiseResolvedWithSession(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    jint j_promise_id,
    const JavaParamRef<jbyteArray>& j_session_id) {
  task_runner_->PostTask(FROM_HERE,
                         base::Bind(&MediaDrmBridge::ResolvePromiseWithSession,
                                    weak_factory_.GetWeakPtr(), j_promise_id,
                                    JavaBytesToString(env, j_session_id)));
}

void MediaDrmBridge::OnPromiseRejected(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    jint j_promise_id,
    const JavaParamRef<jstring>& j_error_message) {
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&MediaDrmBridge::RejectPromise, weak_factory_.GetWeakPtr(),
                 j_promise_id, ConvertJavaStringToUTF8(env, j_error_message)));
}

void MediaDrmBridge::OnSessionMessage(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jbyteArray>& j_session_id,
    jint j_message_type,
    const JavaParamRef<jbyteArray>& j_message) {
  DVLOG(2) << __func__;

  std::vector<uint8_t> message;
  JavaByteArrayToByteVector(env, j_message, &message);
  CdmMessageType message_type =
      GetMessageType(static_cast<RequestType>(j_message_type));

  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(session_message_cb_, JavaBytesToString(env, j_session_id),
                 message_type, message));
}

void MediaDrmBridge::OnSessionClosed(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jbyteArray>& j_session_id) {
  DVLOG(2) << __func__;
  std::string session_id = JavaBytesToString(env, j_session_id);
  task_runner_->PostTask(FROM_HERE, base::Bind(session_closed_cb_, session_id));
}

void MediaDrmBridge::OnSessionKeysChange(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jbyteArray>& j_session_id,
    const JavaParamRef<jobjectArray>& j_keys_info,
    bool has_additional_usable_key,
    bool is_key_release) {
  DVLOG(2) << __func__;

  CdmKeysInfo cdm_keys_info;

  size_t size = env->GetArrayLength(j_keys_info);
  DCHECK_GT(size, 0u);

  for (size_t i = 0; i < size; ++i) {
    ScopedJavaLocalRef<jobject> j_key_status(
        env, env->GetObjectArrayElement(j_keys_info, i));

    ScopedJavaLocalRef<jbyteArray> j_key_id =
        Java_KeyStatus_getKeyId(env, j_key_status);
    std::vector<uint8_t> key_id;
    JavaByteArrayToByteVector(env, j_key_id.obj(), &key_id);
    DCHECK(!key_id.empty());

    jint j_status_code = Java_KeyStatus_getStatusCode(env, j_key_status);
    CdmKeyInformation::KeyStatus key_status =
        ConvertKeyStatus(static_cast<KeyStatus>(j_status_code), is_key_release);

    DVLOG(2) << __func__ << "Key status change: "
             << base::HexEncode(&key_id[0], key_id.size()) << ", "
             << key_status;

    cdm_keys_info.push_back(
        std::make_unique<CdmKeyInformation>(key_id, key_status, 0));
  }

  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(session_keys_change_cb_, JavaBytesToString(env, j_session_id),
                 has_additional_usable_key, base::Passed(&cdm_keys_info)));

  if (has_additional_usable_key) {
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&MediaDrmBridge::OnHasAdditionalUsableKey,
                                      weak_factory_.GetWeakPtr()));
  }
}

// According to MeidaDrm documentation [1], zero |expiry_time_ms| means the keys
// will never expire. This will be translated into a NULL base::Time() [2],
// which will then be mapped to a zero Java time [3]. The zero Java time is
// passed to Blink which will then be translated to NaN [4], which is what the
// spec uses to indicate that the license will never expire [5].
// [1]
// http://developer.android.com/reference/android/media/MediaDrm.OnExpirationUpdateListener.html
// [2] See base::Time::FromDoubleT()
// [3] See base::Time::ToJavaTime()
// [4] See MediaKeySession::expirationChanged()
// [5] https://github.com/w3c/encrypted-media/issues/58
void MediaDrmBridge::OnSessionExpirationUpdate(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_media_drm,
    const JavaParamRef<jbyteArray>& j_session_id,
    jlong expiry_time_ms) {
  DVLOG(2) << __func__ << ": " << expiry_time_ms << " ms";
  task_runner_->PostTask(
      FROM_HERE, base::Bind(session_expiration_update_cb_,
                            JavaBytesToString(env, j_session_id),
                            base::Time::FromDoubleT(expiry_time_ms / 1000.0)));
}

void MediaDrmBridge::OnResetDeviceCredentialsCompleted(
    JNIEnv* env,
    const JavaParamRef<jobject>&,
    bool success) {
  DVLOG(2) << __func__ << ": success:" << success;
  DCHECK(!reset_credentials_cb_.is_null());
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(base::ResetAndReturn(&reset_credentials_cb_), success));
}

//------------------------------------------------------------------------------
// The following are private methods.

MediaDrmBridge::MediaDrmBridge(
    const std::vector<uint8_t>& scheme_uuid,
    const std::string& origin_id,
    SecurityLevel security_level,
    std::unique_ptr<MediaDrmStorageBridge> storage,
    const CreateFetcherCB& create_fetcher_cb,
    const SessionMessageCB& session_message_cb,
    const SessionClosedCB& session_closed_cb,
    const SessionKeysChangeCB& session_keys_change_cb,
    const SessionExpirationUpdateCB& session_expiration_update_cb)
    : scheme_uuid_(scheme_uuid),
      storage_(std::move(storage)),
      create_fetcher_cb_(create_fetcher_cb),
      session_message_cb_(session_message_cb),
      session_closed_cb_(session_closed_cb),
      session_keys_change_cb_(session_keys_change_cb),
      session_expiration_update_cb_(session_expiration_update_cb),
      task_runner_(base::ThreadTaskRunnerHandle::Get()),
      media_crypto_context_(this),
      weak_factory_(this) {
  DVLOG(1) << __func__;

  DCHECK(storage_);

  JNIEnv* env = AttachCurrentThread();
  CHECK(env);

  ScopedJavaLocalRef<jbyteArray> j_scheme_uuid =
      base::android::ToJavaByteArray(env, &scheme_uuid[0], scheme_uuid.size());

  std::string security_level_str = GetSecurityLevelString(security_level);
  ScopedJavaLocalRef<jstring> j_security_level =
      ConvertUTF8ToJavaString(env, security_level_str);

  bool use_origin_isolated_storage =
      // TODO(yucliu): Remove the check once persistent storage is fully
      // supported and check if origin is valid.
      base::FeatureList::IsEnabled(kMediaDrmPersistentLicense) &&
      // MediaDrm implements origin isolated storage on Marshmallow.
      base::android::BuildInfo::GetInstance()->sdk_int() >=
          base::android::SDK_VERSION_MARSHMALLOW &&
      // origin id can be empty when MediaDrmBridge is created by
      // CreateWithoutSessionSupport, which is used to reset credentials.
      !origin_id.empty();

  ScopedJavaLocalRef<jstring> j_security_origin = ConvertUTF8ToJavaString(
      env, use_origin_isolated_storage ? origin_id : "");

  // Note: OnMediaCryptoReady() could be called in this call.
  j_media_drm_.Reset(Java_MediaDrmBridge_create(
      env, j_scheme_uuid, j_security_origin, j_security_level,
      reinterpret_cast<intptr_t>(this),
      reinterpret_cast<intptr_t>(storage_.get())));
}

MediaDrmBridge::~MediaDrmBridge() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __func__;

  JNIEnv* env = AttachCurrentThread();

  // After the call to Java_MediaDrmBridge_destroy() Java won't call native
  // methods anymore, this is ensured by MediaDrmBridge.java.
  if (!j_media_drm_.is_null())
    Java_MediaDrmBridge_destroy(env, j_media_drm_);

  player_tracker_.NotifyCdmUnset();

  if (!media_crypto_ready_cb_.is_null()) {
    base::ResetAndReturn(&media_crypto_ready_cb_)
        .Run(CreateJavaObjectPtr(nullptr), false);
  }

  // Rejects all pending promises.
  cdm_promise_adapter_.Clear();
}

MediaDrmBridge::SecurityLevel MediaDrmBridge::GetSecurityLevel() {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jstring> j_security_level =
      Java_MediaDrmBridge_getSecurityLevel(env, j_media_drm_);
  std::string security_level_str =
      ConvertJavaStringToUTF8(env, j_security_level.obj());
  return GetSecurityLevelFromString(security_level_str);
}

void MediaDrmBridge::NotifyMediaCryptoReady(JavaObjectPtr j_media_crypto) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(j_media_crypto);
  DCHECK(!j_media_crypto_);

  j_media_crypto_ = std::move(j_media_crypto);

  UMA_HISTOGRAM_BOOLEAN("Media.EME.MediaCryptoAvailable",
                        !j_media_crypto_->is_null());

  if (media_crypto_ready_cb_.is_null())
    return;

  // We have to use scoped_ptr to pass ScopedJavaGlobalRef with a callback.
  base::ResetAndReturn(&media_crypto_ready_cb_)
      .Run(CreateJavaObjectPtr(j_media_crypto_->obj()),
           IsSecureCodecRequired());
}

void MediaDrmBridge::SendProvisioningRequest(const std::string& default_url,
                                             const std::string& request_data) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __func__;

  DCHECK(!provision_fetcher_) << "At most one provision request at any time.";
  DCHECK(create_fetcher_cb_);
  provision_fetcher_ = create_fetcher_cb_.Run();

  provision_fetcher_->Retrieve(
      default_url, request_data,
      base::Bind(&MediaDrmBridge::ProcessProvisionResponse,
                 weak_factory_.GetWeakPtr()));
}

void MediaDrmBridge::ProcessProvisionResponse(bool success,
                                              const std::string& response) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __func__;

  DCHECK(provision_fetcher_) << "No provision request pending.";
  provision_fetcher_.reset();

  if (!success)
    VLOG(1) << "Device provision failure: can't get server response";

  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jbyteArray> j_response = StringToJavaBytes(env, response);

  Java_MediaDrmBridge_processProvisionResponse(env, j_media_drm_, success,
                                               j_response);
}

void MediaDrmBridge::OnHasAdditionalUsableKey() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __func__;

  player_tracker_.NotifyNewKey();
}

}  // namespace media
