// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/updater/update_data_provider.h"

#include <utility>

#include "base/base64.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "base/task_scheduler/post_task.h"
#include "components/update_client/update_client.h"
#include "content/public/browser/browser_thread.h"
#include "crypto/sha2.h"
#include "extensions/browser/content_verifier.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/updater/manifest_fetch_data.h"
#include "extensions/common/extension.h"

namespace extensions {

namespace {

using UpdateClientCallback = UpdateDataProvider::UpdateClientCallback;

void InstallUpdateCallback(content::BrowserContext* context,
                           const std::string& extension_id,
                           const std::string& public_key,
                           const base::FilePath& unpacked_dir,
                           UpdateClientCallback update_client_callback) {
  using InstallError = update_client::InstallError;
  using Result = update_client::CrxInstaller::Result;

  ExtensionSystem::Get(context)->InstallUpdate(
      extension_id, public_key, unpacked_dir,
      base::BindOnce(
          [](UpdateClientCallback update_client_callback, bool success) {
            DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
            std::move(update_client_callback)
                .Run(Result(success ? InstallError::NONE
                                    : InstallError::GENERIC_ERROR));
          },
          std::move(update_client_callback)));
}

}  // namespace

UpdateDataProvider::UpdateDataProvider(content::BrowserContext* browser_context)
    : browser_context_(browser_context) {}

UpdateDataProvider::~UpdateDataProvider() {}

void UpdateDataProvider::Shutdown() {
  browser_context_ = nullptr;
}

std::vector<std::unique_ptr<update_client::CrxComponent>>
UpdateDataProvider::GetData(const ExtensionUpdateDataMap& update_crx_component,
                            const std::vector<std::string>& ids) {
  std::vector<std::unique_ptr<update_client::CrxComponent>> data;
  if (!browser_context_)
    return data;
  const ExtensionRegistry* registry = ExtensionRegistry::Get(browser_context_);
  const ExtensionPrefs* extension_prefs = ExtensionPrefs::Get(browser_context_);
  for (const auto& id : ids) {
    const Extension* extension = registry->GetInstalledExtension(id);
    data.push_back(extension ? std::make_unique<update_client::CrxComponent>()
                             : nullptr);
    if (!extension)
      continue;
    DCHECK_NE(0u, update_crx_component.count(id));
    const ExtensionUpdateData& extension_data = update_crx_component.at(id);
    update_client::CrxComponent* crx_component = data.back().get();
    std::string pubkey_bytes;
    base::Base64Decode(extension->public_key(), &pubkey_bytes);
    crx_component->pk_hash.resize(crypto::kSHA256Length, 0);
    crypto::SHA256HashString(pubkey_bytes, crx_component->pk_hash.data(),
                             crx_component->pk_hash.size());
    crx_component->version = extension_data.is_corrupt_reinstall
                                 ? base::Version("0.0.0.0")
                                 : extension->version();
    crx_component->allows_background_download = false;
    crx_component->requires_network_encryption = true;
    crx_component->installer = base::MakeRefCounted<ExtensionInstaller>(
        id, extension->path(),
        base::BindOnce(&UpdateDataProvider::RunInstallCallback, this));
    if (!ExtensionsBrowserClient::Get()->IsExtensionEnabled(id,
                                                            browser_context_)) {
      int disabled_reasons = extension_prefs->GetDisableReasons(id);
      if (disabled_reasons == extensions::disable_reason::DISABLE_NONE ||
          disabled_reasons >= extensions::disable_reason::DISABLE_REASON_LAST) {
        crx_component->disabled_reasons.push_back(0);
      }
      for (int enum_value = 1;
           enum_value < extensions::disable_reason::DISABLE_REASON_LAST;
           enum_value <<= 1) {
        if (disabled_reasons & enum_value)
          crx_component->disabled_reasons.push_back(enum_value);
      }
    }
    crx_component->install_source = extension_data.install_source;
    crx_component->install_location =
        ManifestFetchData::GetSimpleLocationString(extension->location());
  }
  return data;
}

void UpdateDataProvider::RunInstallCallback(
    const std::string& extension_id,
    const std::string& public_key,
    const base::FilePath& unpacked_dir,
    UpdateClientCallback update_client_callback) {
  VLOG(3) << "UpdateDataProvider::RunInstallCallback " << extension_id << " "
          << public_key;

  if (!browser_context_) {
    base::PostTaskWithTraits(
        FROM_HERE, {base::TaskPriority::BACKGROUND, base::MayBlock()},
        base::BindOnce(base::IgnoreResult(&base::DeleteFile), unpacked_dir,
                       true));
    return;
  }

  content::BrowserThread::GetTaskRunnerForThread(content::BrowserThread::UI)
      ->PostTask(FROM_HERE,
                 base::BindOnce(InstallUpdateCallback, browser_context_,
                                extension_id, public_key, unpacked_dir,
                                std::move(update_client_callback)));
}

}  // namespace extensions
