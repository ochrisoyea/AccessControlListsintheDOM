// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MEDIA_ROUTER_PROVIDERS_DIAL_DIAL_MEDIA_ROUTE_PROVIDER_H_
#define CHROME_BROWSER_MEDIA_ROUTER_PROVIDERS_DIAL_DIAL_MEDIA_ROUTE_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/sequence_checker.h"
#include "chrome/browser/media/router/discovery/dial/dial_media_sink_service_impl.h"
#include "chrome/common/media_router/mojo/media_router.mojom.h"
#include "mojo/public/cpp/bindings/binding.h"

namespace url {
class Origin;
}

namespace media_router {

// MediaRouteProvider for DIAL sinks.
// This class is created on the UI thread. All other methods must be invoked
// on the |task_runner| provided to the constructor.
class DialMediaRouteProvider : public mojom::MediaRouteProvider {
 public:
  DialMediaRouteProvider(
      mojom::MediaRouteProviderRequest request,
      mojom::MediaRouterPtrInfo media_router,
      DialMediaSinkServiceImpl* media_sink_service,
      const scoped_refptr<base::SequencedTaskRunner>& task_runner);
  ~DialMediaRouteProvider() override;

  // mojom::MediaRouteProvider:
  void CreateRoute(const std::string& media_source,
                   const std::string& sink_id,
                   const std::string& presentation_id,
                   const url::Origin& origin,
                   int32_t tab_id,
                   base::TimeDelta timeout,
                   bool incognito,
                   CreateRouteCallback callback) override;
  void JoinRoute(const std::string& media_source,
                 const std::string& presentation_id,
                 const url::Origin& origin,
                 int32_t tab_id,
                 base::TimeDelta timeout,
                 bool incognito,
                 JoinRouteCallback callback) override;
  void ConnectRouteByRouteId(const std::string& media_source,
                             const std::string& route_id,
                             const std::string& presentation_id,
                             const url::Origin& origin,
                             int32_t tab_id,
                             base::TimeDelta timeout,
                             bool incognito,
                             ConnectRouteByRouteIdCallback callback) override;
  void TerminateRoute(const std::string& route_id,
                      TerminateRouteCallback callback) override;
  void SendRouteMessage(const std::string& media_route_id,
                        const std::string& message,
                        SendRouteMessageCallback callback) override;
  void SendRouteBinaryMessage(const std::string& media_route_id,
                              const std::vector<uint8_t>& data,
                              SendRouteBinaryMessageCallback callback) override;
  void StartObservingMediaSinks(const std::string& media_source) override;
  void StopObservingMediaSinks(const std::string& media_source) override;
  void StartObservingMediaRoutes(const std::string& media_source) override;
  void StopObservingMediaRoutes(const std::string& media_source) override;
  void StartListeningForRouteMessages(const std::string& route_id) override;
  void StopListeningForRouteMessages(const std::string& route_id) override;
  void DetachRoute(const std::string& route_id) override;
  void EnableMdnsDiscovery() override;
  void UpdateMediaSinks(const std::string& media_source) override;
  void SearchSinks(const std::string& sink_id,
                   const std::string& media_source,
                   mojom::SinkSearchCriteriaPtr search_criteria,
                   SearchSinksCallback callback) override;
  void ProvideSinks(
      const std::string& provider_name,
      const std::vector<media_router::MediaSinkInternal>& sinks) override;
  void CreateMediaRouteController(
      const std::string& route_id,
      mojom::MediaControllerRequest media_controller,
      mojom::MediaStatusObserverPtr observer,
      CreateMediaRouteControllerCallback callback) override;

 private:
  FRIEND_TEST_ALL_PREFIXES(DialMediaRouteProviderTest, AddRemoveSinkQuery);
  FRIEND_TEST_ALL_PREFIXES(DialMediaRouteProviderTest,
                           AddSinkQuerySameMediaSource);
  FRIEND_TEST_ALL_PREFIXES(DialMediaRouteProviderTest,
                           AddSinkQuerySameAppDifferentMediaSources);
  FRIEND_TEST_ALL_PREFIXES(DialMediaRouteProviderTest,
                           AddSinkQueryDifferentApps);

  struct MediaSinkQuery {
    MediaSinkQuery();
    ~MediaSinkQuery();

    // Set of registered media sources for current sink query.
    base::flat_set<MediaSource> media_sources;
    DialMediaSinkServiceImpl::SinkQueryByAppSubscription subscription;

    DISALLOW_COPY_AND_ASSIGN(MediaSinkQuery);
  };

  // Binds the message pipes |request| and |media_router| to |this|.
  void Init(mojom::MediaRouteProviderRequest request,
            mojom::MediaRouterPtrInfo media_router);

  void OnAvailableSinksUpdated(const std::string& app_name);

  void NotifyOnSinksReceived(const MediaSource::Id& source_id,
                             const std::vector<MediaSinkInternal>& sinks,
                             const std::vector<url::Origin>& origins);

  // Returns a list of valid origins for |app_name|. Returns an empty list if
  // all origins are valid.
  std::vector<url::Origin> GetOrigins(const std::string& app_name);

  // Binds |this| to the Mojo request passed into the ctor.
  mojo::Binding<mojom::MediaRouteProvider> binding_;

  // Mojo pointer to the Media Router.
  mojom::MediaRouterPtr media_router_;

  // Non-owned pointer to the DialMediaSinkServiceImpl instance.
  DialMediaSinkServiceImpl* const media_sink_service_;

  // Map of media sink queries, keyed by app name.
  base::flat_map<std::string, std::unique_ptr<MediaSinkQuery>>
      media_sink_queries_;

  SEQUENCE_CHECKER(sequence_checker_);
  DISALLOW_COPY_AND_ASSIGN(DialMediaRouteProvider);
};

}  // namespace media_router

#endif  // CHROME_BROWSER_MEDIA_ROUTER_PROVIDERS_DIAL_DIAL_MEDIA_ROUTE_PROVIDER_H_
