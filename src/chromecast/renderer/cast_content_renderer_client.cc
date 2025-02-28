// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/renderer/cast_content_renderer_client.h"

#include <utility>

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "chromecast/base/bitstream_audio_codecs.h"
#include "chromecast/base/chromecast_switches.h"
#include "chromecast/media/base/media_codec_support.h"
#include "chromecast/media/base/supported_codec_profile_levels_memo.h"
#include "chromecast/public/media/media_capabilities_shlib.h"
#include "chromecast/renderer/cast_render_frame_action_deferrer.h"
#include "chromecast/renderer/media/key_systems_cast.h"
#include "chromecast/renderer/media/media_caps_observer_impl.h"
#include "chromecast/renderer/tts_dispatcher.h"
#include "components/network_hints/renderer/prescient_networking_dispatcher.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/service_names.mojom.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "media/base/media.h"
#include "mojo/public/cpp/bindings/interface_request.h"
#include "services/service_manager/public/cpp/connector.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"

#if defined(OS_ANDROID)
#include "media/base/android/media_codec_util.h"
#else
#include "chromecast/renderer/memory_pressure_observer_impl.h"
#endif  // OS_ANDROID

#if !defined(OS_FUCHSIA)
#include "chromecast/crash/cast_crash_keys.h"
#endif  // !defined(OS_FUCHSIA)

#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
#include "chromecast/common/cast_extensions_client.h"
#include "chromecast/renderer/cast_extensions_renderer_client.h"
#include "content/public/common/content_constants.h"
#include "extensions/common/common_manifest_handlers.h"  // nogncheck
#include "extensions/common/extension_urls.h"            // nogncheck
#include "extensions/renderer/dispatcher.h"              // nogncheck
#include "extensions/renderer/extension_frame_helper.h"  // nogncheck
#include "extensions/renderer/guest_view/extensions_guest_view_container.h"  // nogncheck
#include "extensions/renderer/guest_view/extensions_guest_view_container_dispatcher.h"  // nogncheck
#endif

namespace chromecast {
namespace shell {

CastContentRendererClient::CastContentRendererClient()
    : supported_profiles_(new media::SupportedCodecProfileLevelsMemo()),
      app_media_capabilities_observer_binding_(this),
      allow_hidden_media_playback_(
          base::CommandLine::ForCurrentProcess()->HasSwitch(
              switches::kAllowHiddenMediaPlayback)),
      supported_bitstream_audio_codecs_(kBitstreamAudioCodecNone) {
#if defined(OS_ANDROID)
  DCHECK(::media::MediaCodecUtil::IsMediaCodecAvailable())
      << "MediaCodec is not available!";
  // Platform decoder support must be enabled before we set the
  // IsCodecSupportedCB because the latter instantiates the lazy MimeUtil
  // instance, which caches the platform decoder supported state when it is
  // constructed.
  ::media::EnablePlatformDecoderSupport();
#endif  // OS_ANDROID
}

CastContentRendererClient::~CastContentRendererClient() = default;

void CastContentRendererClient::RenderThreadStarted() {
  // Register as observer for media capabilities
  content::RenderThread* thread = content::RenderThread::Get();
  media::mojom::MediaCapsPtr media_caps;
  thread->GetConnector()->BindInterface(content::mojom::kBrowserServiceName,
                                        &media_caps);
  media::mojom::MediaCapsObserverPtr proxy;
  media_caps_observer_.reset(
      new media::MediaCapsObserverImpl(&proxy, supported_profiles_.get()));
  media_caps->AddObserver(std::move(proxy));

#if !defined(OS_ANDROID)
  // Register to observe memory pressure changes
  chromecast::mojom::MemoryPressureControllerPtr memory_pressure_controller;
  thread->GetConnector()->BindInterface(content::mojom::kBrowserServiceName,
                                        &memory_pressure_controller);
  chromecast::mojom::MemoryPressureObserverPtr memory_pressure_proxy;
  memory_pressure_observer_.reset(
      new MemoryPressureObserverImpl(&memory_pressure_proxy));
  memory_pressure_controller->AddObserver(std::move(memory_pressure_proxy));
#endif

  prescient_networking_dispatcher_.reset(
      new network_hints::PrescientNetworkingDispatcher());

#if !defined(OS_FUCHSIA)
  // TODO(crbug.com/753619): Enable crash reporting on Fuchsia.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  std::string last_launched_app =
      command_line->GetSwitchValueNative(switches::kLastLaunchedApp);
  if (!last_launched_app.empty())
    crash_keys::last_app.Set(last_launched_app);

  std::string previous_app =
      command_line->GetSwitchValueNative(switches::kPreviousApp);
  if (!previous_app.empty())
    crash_keys::previous_app.Set(previous_app);
#endif  // !defined(OS_FUCHSIA)

#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
  extensions_client_ = std::make_unique<extensions::CastExtensionsClient>();
  extensions::ExtensionsClient::Set(extensions_client_.get());

  extensions_renderer_client_ =
      std::make_unique<extensions::CastExtensionsRendererClient>();
  extensions::ExtensionsRendererClient::Set(extensions_renderer_client_.get());

  thread->AddObserver(extensions_renderer_client_->GetDispatcher());

  guest_view_container_dispatcher_ =
      std::make_unique<extensions::ExtensionsGuestViewContainerDispatcher>();
  thread->AddObserver(guest_view_container_dispatcher_.get());
#endif
}

void CastContentRendererClient::RenderViewCreated(
    content::RenderView* render_view) {
  blink::WebView* webview = render_view->GetWebView();
  if (webview) {
    if (auto* web_frame_widget = render_view->GetWebFrameWidget())
      web_frame_widget->SetBaseBackgroundColor(chromecast::GetSwitchValueColor(
          switches::kCastAppBackgroundColor, SK_ColorBLACK));

    // Disable application cache as Chromecast doesn't support off-line
    // application running.
    webview->GetSettings()->SetOfflineWebApplicationCacheEnabled(false);
  }
}

void CastContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  DCHECK(render_frame);
  if (!app_media_capabilities_observer_binding_.is_bound()) {
    mojom::ApplicationMediaCapabilitiesObserverPtr observer;
    app_media_capabilities_observer_binding_.Bind(mojo::MakeRequest(&observer));
    mojom::ApplicationMediaCapabilitiesPtr app_media_capabilities;
    render_frame->GetRemoteInterfaces()->GetInterface(
        mojo::MakeRequest(&app_media_capabilities));
    app_media_capabilities->AddObserver(std::move(observer));
  }

#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
  extensions::Dispatcher* dispatcher =
      extensions_renderer_client_->GetDispatcher();
  // ExtensionFrameHelper destroys itself when the RenderFrame is destroyed.
  new extensions::ExtensionFrameHelper(render_frame, dispatcher);

  dispatcher->OnRenderFrameCreated(render_frame);
#endif
}

content::BrowserPluginDelegate*
CastContentRendererClient::CreateBrowserPluginDelegate(
    content::RenderFrame* render_frame,
    const content::WebPluginInfo& info,
    const std::string& mime_type,
    const GURL& original_url) {
#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
  if (mime_type == content::kBrowserPluginMimeType) {
    return new extensions::ExtensionsGuestViewContainer(render_frame);
  }
#endif
  return nullptr;
}

void CastContentRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
  extensions_renderer_client_->GetDispatcher()->RunScriptsAtDocumentStart(
      render_frame);
#endif
}

void CastContentRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
#if BUILDFLAG(ENABLE_CHROMECAST_EXTENSIONS)
  extensions_renderer_client_->GetDispatcher()->RunScriptsAtDocumentEnd(
      render_frame);
#endif
}

void CastContentRendererClient::AddSupportedKeySystems(
    std::vector<std::unique_ptr<::media::KeySystemProperties>>*
        key_systems_properties) {
  media::AddChromecastKeySystems(key_systems_properties,
                                 false /* enable_persistent_license_support */,
                                 false /* force_software_crypto */);
}

bool CastContentRendererClient::IsSupportedAudioConfig(
    const ::media::AudioConfig& config) {
#if defined(OS_ANDROID)
  // No ATV device we know of has (E)AC3 decoder, so it relies on the audio sink
  // device.
  if (config.codec == ::media::kCodecEAC3)
    return kBitstreamAudioCodecEac3 & supported_bitstream_audio_codecs_;
  if (config.codec == ::media::kCodecAC3)
    return kBitstreamAudioCodecAc3 & supported_bitstream_audio_codecs_;
  if (config.codec == ::media::kCodecMpegHAudio)
    return kBitstreamAudioCodecMpegHAudio & supported_bitstream_audio_codecs_;

  // TODO(sanfin): Implement this for Android.
  return true;
#else
  // If the HDMI sink supports bitstreaming the codec, then the vendor backend
  // does not need to support it.
  if (IsSupportedBitstreamAudioCodec(config.codec)) {
    return true;
  }

  media::AudioCodec codec = media::ToCastAudioCodec(config.codec);
  // Cast platform implements software decoding of Opus and FLAC, so only PCM
  // support is necessary in order to support Opus and FLAC.
  if (codec == media::kCodecOpus || codec == media::kCodecFLAC)
    codec = media::kCodecPCM;

  media::AudioConfig cast_audio_config;
  cast_audio_config.codec = codec;
  return media::MediaCapabilitiesShlib::IsSupportedAudioConfig(
      cast_audio_config);
#endif
}

bool CastContentRendererClient::IsSupportedVideoConfig(
    const ::media::VideoConfig& config) {
// TODO(servolk): make use of eotf.
#if defined(OS_ANDROID)
  return supported_profiles_->IsSupportedVideoConfig(
      media::ToCastVideoCodec(config.codec, config.profile),
      media::ToCastVideoProfile(config.profile), config.level);
#else
  return media::MediaCapabilitiesShlib::IsSupportedVideoConfig(
      media::ToCastVideoCodec(config.codec, config.profile),
      media::ToCastVideoProfile(config.profile), config.level);
#endif
}

bool CastContentRendererClient::IsSupportedBitstreamAudioCodec(
    ::media::AudioCodec codec) {
  return (codec == ::media::kCodecAC3 &&
          (kBitstreamAudioCodecAc3 & supported_bitstream_audio_codecs_)) ||
         (codec == ::media::kCodecEAC3 &&
          (kBitstreamAudioCodecEac3 & supported_bitstream_audio_codecs_)) ||
         (codec == ::media::kCodecMpegHAudio &&
          (kBitstreamAudioCodecMpegHAudio & supported_bitstream_audio_codecs_));
}

blink::WebPrescientNetworking*
CastContentRendererClient::GetPrescientNetworking() {
  return prescient_networking_dispatcher_.get();
}

void CastContentRendererClient::DeferMediaLoad(
    content::RenderFrame* render_frame,
    bool render_frame_has_played_media_before,
    const base::Closure& closure) {
  if (allow_hidden_media_playback_) {
    closure.Run();
    return;
  }

  RunWhenInForeground(render_frame, closure);
}

void CastContentRendererClient::RunWhenInForeground(
    content::RenderFrame* render_frame,
    const base::Closure& closure) {
  if (!render_frame->IsHidden()) {
    closure.Run();
    return;
  }

  // Lifetime is tied to |render_frame| via content::RenderFrameObserver.
  new CastRenderFrameActionDeferrer(render_frame, closure);
}

bool CastContentRendererClient::AllowIdleMediaSuspend() {
  return false;
}

void CastContentRendererClient::
    SetRuntimeFeaturesDefaultsBeforeBlinkInitialization() {
  // Settings for ATV (Android defaults are not what we want).
  blink::WebRuntimeFeatures::EnableMediaControlsOverlayPlayButton(false);
}

void CastContentRendererClient::OnSupportedBitstreamAudioCodecsChanged(
    int codecs) {
  supported_bitstream_audio_codecs_ = codecs;
}

std::unique_ptr<blink::WebSpeechSynthesizer>
CastContentRendererClient::OverrideSpeechSynthesizer(
    blink::WebSpeechSynthesizerClient* client) {
  return std::make_unique<TtsDispatcher>(client);
}

}  // namespace shell
}  // namespace chromecast
