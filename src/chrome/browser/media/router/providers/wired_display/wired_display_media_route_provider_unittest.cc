// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/router/providers/wired_display/wired_display_media_route_provider.h"

#include "base/run_loop.h"
#include "chrome/browser/media/router/providers/wired_display/wired_display_presentation_receiver.h"
#include "chrome/browser/media/router/providers/wired_display/wired_display_presentation_receiver_factory.h"
#include "chrome/browser/media/router/test/media_router_mojo_test.h"
#include "chrome/browser/media/router/test/mock_mojo_media_router.h"
#include "chrome/common/media_router/mojo/media_router.mojom.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/display/display.h"
#include "ui/display/test/test_screen.h"
#include "ui/gfx/geometry/rect.h"

using display::Display;
using testing::_;
using testing::Invoke;
using testing::IsEmpty;
using testing::WithArg;

namespace media_router {

namespace {

class MockCallback {
 public:
  MOCK_METHOD3(CreateRoute,
               void(const base::Optional<MediaRoute>& route,
                    const base::Optional<std::string>& error,
                    RouteRequestResult::ResultCode result));
  MOCK_METHOD2(TerminateRoute,
               void(const base::Optional<std::string>& error,
                    RouteRequestResult::ResultCode result));
};

std::string GetSinkId(const Display& display) {
  return WiredDisplayMediaRouteProvider::GetSinkIdForDisplay(display);
}

class MockPresentationReceiver : public WiredDisplayPresentationReceiver {
 public:
  MOCK_METHOD2(Start,
               void(const std::string& presentation_id, const GURL& start_url));
  void Terminate() override { TerminateInternal(); }
  MOCK_METHOD0(TerminateInternal, void());
  MOCK_METHOD0(ExitFullscreen, void());

  void SetTerminationCallback(base::OnceClosure termination_callback) {
    termination_callback_ = std::move(termination_callback);
  }

  void SetTitleChangeCallback(
      base::RepeatingCallback<void(const std::string&)> title_change_callback) {
    title_change_callback_ = std::move(title_change_callback);
  }

  void RunTerminationCallback() { std::move(termination_callback_).Run(); }

  void RunTitleChangeCallback(const std::string& new_title) {
    title_change_callback_.Run(new_title);
  }

 private:
  base::OnceClosure termination_callback_;
  base::RepeatingCallback<void(const std::string&)> title_change_callback_;
};

class MockReceiverCreator {
 public:
  MockReceiverCreator()
      : unique_receiver_(std::make_unique<MockPresentationReceiver>()),
        receiver_(unique_receiver_.get()) {}
  ~MockReceiverCreator() = default;

  // This should be called only once in the lifetime of this object.
  std::unique_ptr<WiredDisplayPresentationReceiver> CreateReceiver(
      Profile* profile,
      const gfx::Rect& bounds,
      base::OnceClosure termination_callback,
      base::RepeatingCallback<void(const std::string&)> title_change_callback) {
    CHECK(unique_receiver_);
    unique_receiver_->SetTerminationCallback(std::move(termination_callback));
    unique_receiver_->SetTitleChangeCallback(std::move(title_change_callback));
    return std::move(unique_receiver_);
  }

  MockPresentationReceiver* receiver() { return receiver_; }

 private:
  // Initialized in the ctor instead of CreateReceiver() so that the receiver()
  // getter is valid even before CreateReceiver() is called.
  // When CreateReceiver() is called, the ownership of |unique_receiver_| gets
  // transferred to the caller.
  std::unique_ptr<MockPresentationReceiver> unique_receiver_;

  // Retains a reference to |unique_receiver_| even after |this| loses its
  // ownership.
  MockPresentationReceiver* const receiver_;
};

const char kPresentationSource[] = "https://www.example.com/presentation";
const char kNonPresentationSource[] = "not://a.valid.presentation/source";
const MediaRouteProviderId kProviderId = MediaRouteProviderId::WIRED_DISPLAY;

}  // namespace

class TestWiredDisplayMediaRouteProvider
    : public WiredDisplayMediaRouteProvider {
 public:
  TestWiredDisplayMediaRouteProvider(mojom::MediaRouteProviderRequest request,
                                     mojom::MediaRouterPtr media_router,
                                     Profile* profile)
      : WiredDisplayMediaRouteProvider(std::move(request),
                                       std::move(media_router),
                                       profile) {}
  ~TestWiredDisplayMediaRouteProvider() override = default;

  void set_all_displays(std::vector<Display> all_displays) {
    all_displays_ = std::move(all_displays);
  }

  void set_primary_display(Display primary_display) {
    primary_display_ = std::move(primary_display);
  }

  MOCK_CONST_METHOD0(GetPrimaryDisplayInternal, void());

 protected:
  // In the actual class, this method returns display::Screen::GetAllDisplays().
  // TODO(takumif): Expand display::test::TestScreen to support multiple
  // displays, so that these methods don't need to be overridden. Then
  // SystemDisplayApiTest and WindowSizerTestCommon would also be able to use
  // the class.
  std::vector<Display> GetAllDisplays() const override { return all_displays_; }

  // In the actual class, this method returns
  // display::Screen::GetPrimaryDisplay().
  Display GetPrimaryDisplay() const override {
    GetPrimaryDisplayInternal();
    return primary_display_;
  }

 private:
  std::vector<Display> all_displays_;
  Display primary_display_;
};

class WiredDisplayMediaRouteProviderTest : public testing::Test {
 public:
  WiredDisplayMediaRouteProviderTest()
      : primary_display_bounds_(0, 1080, 1920, 1080),
        secondary_display1_bounds_(0, 0, 1920, 1080),  // x, y, width, height.
        secondary_display2_bounds_(1920, 0, 1920, 1080),
        primary_display_(10000003, primary_display_bounds_),
        secondary_display1_(10000001, secondary_display1_bounds_),
        secondary_display2_(10000002, secondary_display2_bounds_),
        mirror_display_(10000004, primary_display_bounds_) {}
  ~WiredDisplayMediaRouteProviderTest() override {}

  void SetUp() override {
    display::Screen::SetScreenInstance(&test_screen_);

    mojom::MediaRouterPtr router_pointer;
    router_binding_ = std::make_unique<mojo::Binding<mojom::MediaRouter>>(
        &router_, mojo::MakeRequest(&router_pointer));
    provider_ = std::make_unique<TestWiredDisplayMediaRouteProvider>(
        mojo::MakeRequest(&provider_pointer_), std::move(router_pointer),
        &profile_);
    provider_->set_primary_display(primary_display_);
    WiredDisplayPresentationReceiverFactory::SetCreateReceiverCallbackForTest(
        base::BindRepeating(&MockReceiverCreator::CreateReceiver,
                            base::Unretained(&receiver_creator_)));
  }

  void TearDown() override {
    provider_.reset();
    display::Screen::SetScreenInstance(nullptr);
  }

 protected:
  // A mojo pointer to |provider_|.
  mojom::MediaRouteProviderPtr provider_pointer_;
  std::unique_ptr<TestWiredDisplayMediaRouteProvider> provider_;
  MockMojoMediaRouter router_;
  std::unique_ptr<mojo::Binding<mojom::MediaRouter>> router_binding_;

  gfx::Rect primary_display_bounds_;
  gfx::Rect secondary_display1_bounds_;
  gfx::Rect secondary_display2_bounds_;

  Display primary_display_;
  Display secondary_display1_;
  Display secondary_display2_;
  Display mirror_display_;  // Has the same bounds as |primary_display_|.

  MockReceiverCreator receiver_creator_;

 private:
  content::TestBrowserThreadBundle test_thread_bundle_;
  TestingProfile profile_;
  display::test::TestScreen test_screen_;
};

TEST_F(WiredDisplayMediaRouteProviderTest, GetDisplaysAsSinks) {
  provider_->set_all_displays(
      {secondary_display1_, primary_display_, secondary_display2_});

  const std::string primary_id = GetSinkId(primary_display_);
  const std::string secondary_id1 = GetSinkId(secondary_display1_);
  const std::string secondary_id2 = GetSinkId(secondary_display2_);
  EXPECT_CALL(router_,
              OnSinksReceived(kProviderId, kPresentationSource, _, IsEmpty()))
      .WillOnce(
          WithArg<2>(Invoke([&primary_id, &secondary_id1, &secondary_id2](
                                const std::vector<MediaSinkInternal>& sinks) {
            EXPECT_EQ(sinks.size(), 3u);
            EXPECT_EQ(sinks[0].sink().id(), primary_id);
            EXPECT_EQ(sinks[1].sink().id(), secondary_id1);
            EXPECT_EQ(sinks[2].sink().id(), secondary_id2);
          })));
  provider_pointer_->StartObservingMediaSinks(kPresentationSource);
  base::RunLoop().RunUntilIdle();
}

TEST_F(WiredDisplayMediaRouteProviderTest, NotifyOnDisplayChange) {
  const std::string primary_id = GetSinkId(primary_display_);
  const std::string secondary_id1 = GetSinkId(secondary_display1_);
  provider_pointer_->StartObservingMediaSinks(kPresentationSource);
  base::RunLoop().RunUntilIdle();

  // Add an external display. MediaRouter should be notified of the sink and the
  // sink availability change.
  provider_->set_all_displays({primary_display_, secondary_display1_});
  EXPECT_CALL(router_, OnSinkAvailabilityUpdated(
                           MediaRouteProviderId::WIRED_DISPLAY,
                           mojom::MediaRouter::SinkAvailability::PER_SOURCE));
  EXPECT_CALL(router_,
              OnSinksReceived(MediaRouteProviderId::WIRED_DISPLAY, _, _, _))
      .WillOnce(
          WithArg<2>(Invoke([&primary_id, &secondary_id1](
                                const std::vector<MediaSinkInternal>& sinks) {
            EXPECT_EQ(sinks.size(), 2u);
            EXPECT_EQ(sinks[0].sink().id(), primary_id);
            EXPECT_EQ(sinks[1].sink().id(), secondary_id1);
          })));
  provider_->OnDisplayAdded(secondary_display1_);
  base::RunLoop().RunUntilIdle();

  // Remove the external display. MediaRouter should be notified of the lack of
  // sinks.
  provider_->set_all_displays({primary_display_});
  EXPECT_CALL(router_, OnSinkAvailabilityUpdated(
                           MediaRouteProviderId::WIRED_DISPLAY,
                           mojom::MediaRouter::SinkAvailability::UNAVAILABLE));
  EXPECT_CALL(router_, OnSinksReceived(MediaRouteProviderId::WIRED_DISPLAY, _,
                                       IsEmpty(), _));
  provider_->OnDisplayRemoved(secondary_display1_);
  base::RunLoop().RunUntilIdle();

  // Add a display that mirrors the primary display. The sink list should still
  // be empty.
  provider_->set_all_displays({primary_display_, mirror_display_});
  EXPECT_CALL(router_, OnSinkAvailabilityUpdated(
                           MediaRouteProviderId::WIRED_DISPLAY,
                           mojom::MediaRouter::SinkAvailability::UNAVAILABLE));
  EXPECT_CALL(router_, OnSinksReceived(MediaRouteProviderId::WIRED_DISPLAY, _,
                                       IsEmpty(), _));
  provider_->OnDisplayAdded(mirror_display_);
  base::RunLoop().RunUntilIdle();
}

TEST_F(WiredDisplayMediaRouteProviderTest, NoDisplay) {
  provider_->StartObservingMediaSinks(kPresentationSource);
  provider_->set_all_displays({primary_display_});
  provider_->OnDisplayAdded(primary_display_);
  base::RunLoop().RunUntilIdle();

  // When the display list is empty, |provider_| should not try to get the
  // primary display, as it would be invalid.
  EXPECT_CALL(*provider_, GetPrimaryDisplayInternal()).Times(0);
  provider_->set_all_displays({});
  provider_->OnDisplayRemoved(primary_display_);
  base::RunLoop().RunUntilIdle();
}

TEST_F(WiredDisplayMediaRouteProviderTest, NoSinksForNonPresentationSource) {
  EXPECT_CALL(router_,
              OnSinksReceived(kProviderId, kNonPresentationSource, _, _))
      .Times(0);
  provider_pointer_->StartObservingMediaSinks(kNonPresentationSource);
  base::RunLoop().RunUntilIdle();
}

TEST_F(WiredDisplayMediaRouteProviderTest, CreateAndTerminateRoute) {
  const std::string presentation_id = "presentationId";
  MockCallback callback;

  provider_->set_all_displays({secondary_display1_, primary_display_});
  provider_pointer_->StartObservingMediaRoutes(kPresentationSource);
  base::RunLoop().RunUntilIdle();

  // Create a route for |presentation_id|.
  EXPECT_CALL(callback, CreateRoute(_, base::Optional<std::string>(),
                                    RouteRequestResult::OK))
      .WillOnce(WithArg<0>(
          Invoke([&presentation_id](const base::Optional<MediaRoute>& route) {
            EXPECT_TRUE(route.has_value());
            EXPECT_EQ(route->media_route_id(), presentation_id);
            EXPECT_EQ(route->description(), "Presenting (www.example.com)");
          })));
  EXPECT_CALL(router_,
              OnRoutesUpdated(kProviderId, _, kPresentationSource, IsEmpty()))
      .WillOnce(WithArg<1>(
          Invoke([&presentation_id](const std::vector<MediaRoute>& routes) {
            EXPECT_EQ(routes.size(), 1u);
            EXPECT_EQ(routes[0].media_route_id(), presentation_id);
            EXPECT_EQ(routes[0].description(), "Presenting (www.example.com)");
          })));
  EXPECT_CALL(*receiver_creator_.receiver(),
              Start(presentation_id, GURL(kPresentationSource)));
  provider_pointer_->CreateRoute(
      kPresentationSource, GetSinkId(secondary_display1_), presentation_id,
      url::Origin::Create(GURL(kPresentationSource)), 0,
      base::TimeDelta::FromSeconds(100), false,
      base::BindOnce(&MockCallback::CreateRoute, base::Unretained(&callback)));
  base::RunLoop().RunUntilIdle();

  // Terminate the route.
  EXPECT_CALL(callback, TerminateRoute(base::Optional<std::string>(),
                                       RouteRequestResult::OK));
  EXPECT_CALL(*receiver_creator_.receiver(), TerminateInternal());
  EXPECT_CALL(router_,
              OnPresentationConnectionStateChanged(
                  presentation_id,
                  mojom::MediaRouter::PresentationConnectionState::TERMINATED));
  provider_pointer_->TerminateRoute(
      presentation_id, base::BindOnce(&MockCallback::TerminateRoute,
                                      base::Unretained(&callback)));
  base::RunLoop().RunUntilIdle();

  // The presentation should not be removed until the receiver's termination
  // callback is called.
  EXPECT_CALL(router_, OnRoutesUpdated(kProviderId, IsEmpty(),
                                       kPresentationSource, IsEmpty()));
  receiver_creator_.receiver()->RunTerminationCallback();
}

TEST_F(WiredDisplayMediaRouteProviderTest, SendMediaStatusUpdate) {
  const std::string presentation_id = "presentationId";
  const std::string page_title = "Presentation Page Title";
  MockCallback callback;

  provider_->set_all_displays({secondary_display1_, primary_display_});
  provider_pointer_->StartObservingMediaRoutes(kPresentationSource);
  base::RunLoop().RunUntilIdle();

  // Create a route for |presentation_id|.
  provider_pointer_->CreateRoute(
      kPresentationSource, GetSinkId(secondary_display1_), presentation_id,
      url::Origin::Create(GURL(kPresentationSource)), 0,
      base::TimeDelta::FromSeconds(100), false,
      base::BindOnce(&MockCallback::CreateRoute, base::Unretained(&callback)));
  base::RunLoop().RunUntilIdle();

  mojom::MediaControllerPtr media_controller_ptr;
  mojom::MediaStatusObserverPtr status_observer_ptr;
  MockMediaStatusObserver status_observer(
      mojo::MakeRequest(&status_observer_ptr));
  provider_pointer_->CreateMediaRouteController(
      presentation_id, mojo::MakeRequest(&media_controller_ptr),
      std::move(status_observer_ptr), base::BindOnce([](bool success) {}));

  EXPECT_CALL(status_observer, OnMediaStatusUpdated(_))
      .WillOnce(Invoke([&page_title](const MediaStatus& status) {
        EXPECT_EQ(status.title, page_title);
      }));
  receiver_creator_.receiver()->RunTitleChangeCallback(page_title);
  base::RunLoop().RunUntilIdle();
}

TEST_F(WiredDisplayMediaRouteProviderTest, ExitFullscreenOnDisplayRemoved) {
  MockCallback callback;
  provider_->set_all_displays({secondary_display1_, primary_display_});
  provider_pointer_->StartObservingMediaRoutes(kPresentationSource);
  base::RunLoop().RunUntilIdle();

  provider_pointer_->CreateRoute(
      kPresentationSource, GetSinkId(secondary_display1_), "presentationId",
      url::Origin::Create(GURL(kPresentationSource)), 0,
      base::TimeDelta::FromSeconds(100), false,
      base::BindOnce(&MockCallback::CreateRoute, base::Unretained(&callback)));
  base::RunLoop().RunUntilIdle();

  EXPECT_CALL(*receiver_creator_.receiver(), ExitFullscreen());
  provider_->OnDisplayRemoved(secondary_display1_);
}

}  // namespace media_router
