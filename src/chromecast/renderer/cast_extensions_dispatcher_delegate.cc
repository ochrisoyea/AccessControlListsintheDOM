// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/renderer/cast_extensions_dispatcher_delegate.h"

#include <memory>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/sha1.h"
#include "base/strings/string_number_conversions.h"
#include "chromecast/renderer/grit/extensions_renderer_resources.h"
#include "chromecast/renderer/extensions/automation_internal_custom_bindings.h"
#include "components/version_info/version_info.h"
#include "content/public/common/content_switches.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_features.h"
#include "extensions/common/features/feature_channel.h"
#include "extensions/common/permissions/manifest_permission_set.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/switches.h"
#include "extensions/renderer/bindings/api_bindings_system.h"
#include "extensions/renderer/css_native_handler.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/i18n_custom_bindings.h"
#include "extensions/renderer/lazy_background_page_native_handler.h"
#include "extensions/renderer/native_extension_bindings_system.h"
#include "extensions/renderer/native_handler.h"
#include "extensions/renderer/resource_bundle_source_map.h"
#include "extensions/renderer/script_context.h"
#include "media/media_buildflags.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_security_policy.h"

using extensions::NativeHandler;

CastExtensionsDispatcherDelegate::CastExtensionsDispatcherDelegate() {
}

CastExtensionsDispatcherDelegate::~CastExtensionsDispatcherDelegate() {
}

void CastExtensionsDispatcherDelegate::InitOriginPermissions(
    const extensions::Extension* extension,
    bool is_extension_active) {
  // TODO(rmrossi): Stub
}

void CastExtensionsDispatcherDelegate::RegisterNativeHandlers(
    extensions::Dispatcher* dispatcher,
    extensions::ModuleSystem* module_system,
    extensions::ExtensionBindingsSystem* bindings_system,
    extensions::ScriptContext* context) {
  module_system->RegisterNativeHandler(
      "automationInternal",
      std::make_unique<extensions::cast::AutomationInternalCustomBindings>(
          context, bindings_system));
}

void CastExtensionsDispatcherDelegate::PopulateSourceMap(
    extensions::ResourceBundleSourceMap* source_map) {
  // Custom bindings.
  source_map->RegisterSource("automation", IDR_AUTOMATION_CUSTOM_BINDINGS_JS);
  source_map->RegisterSource("automationEvent", IDR_AUTOMATION_EVENT_JS);
  source_map->RegisterSource("automationNode", IDR_AUTOMATION_NODE_JS);
}

void CastExtensionsDispatcherDelegate::RequireAdditionalModules(
    extensions::ScriptContext* context) {
  // TODO(rmrossi): Stub
}

void CastExtensionsDispatcherDelegate::OnActiveExtensionsUpdated(
    const std::set<std::string>& extension_ids) {
  // TODO(rmrossi): Stub
}

void CastExtensionsDispatcherDelegate::InitializeBindingsSystem(
    extensions::Dispatcher* dispatcher,
    extensions::NativeExtensionBindingsSystem* bindings_system) {
  DCHECK(
      base::FeatureList::IsEnabled(extensions::features::kNativeCrxBindings));
  // TODO(rmrossi): Stub
}
