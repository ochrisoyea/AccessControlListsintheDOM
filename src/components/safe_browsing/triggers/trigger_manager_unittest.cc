// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/safe_browsing/triggers/trigger_manager.h"

#include "base/stl_util.h"
#include "base/test/scoped_feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/safe_browsing/browser/threat_details.h"
#include "components/safe_browsing/features.h"
#include "components/safe_browsing/triggers/trigger_throttler.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_web_contents_factory.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::Key;
using testing::Return;
using testing::UnorderedElementsAre;

namespace safe_browsing {

// Mock ThreatDetails class that makes FinishCollection a no-op.
class MockThreatDetails : public ThreatDetails {
 public:
  MockThreatDetails() {}
  MOCK_METHOD2(FinishCollection, void(bool did_proceed, int num_visits));

 private:
  ~MockThreatDetails() override {}
  DISALLOW_COPY_AND_ASSIGN(MockThreatDetails);
};

class MockThreatDetailsFactory : public ThreatDetailsFactory {
 public:
  ~MockThreatDetailsFactory() override {}

  ThreatDetails* CreateThreatDetails(
      BaseUIManager* ui_manager,
      content::WebContents* web_contents,
      const security_interstitials::UnsafeResource& unsafe_resource,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      history::HistoryService* history_service,
      ReferrerChainProvider* referrer_chain_provider,
      bool trim_to_ad_tags,
      ThreatDetailsDoneCallback done_callback) override {
    MockThreatDetails* threat_details = new MockThreatDetails();
    return threat_details;
  }
};

class MockTriggerThrottler : public TriggerThrottler {
 public:
  MOCK_CONST_METHOD1(TriggerCanFire, bool(TriggerType trigger_type));
};

class TriggerManagerTest : public ::testing::Test {
 public:
  TriggerManagerTest()
      : trigger_manager_(/*ui_manager=*/nullptr,
                         /*referrer_chain_provider=*/nullptr) {}
  ~TriggerManagerTest() override {}

  void SetUp() override {
    ThreatDetails::RegisterFactory(&mock_threat_details_factory_);

    // Register any prefs that are needed by the trigger manager. By default,
    // enable Safe Browsing and Extended Reporting.
    safe_browsing::RegisterProfilePrefs(pref_service_.registry());
    SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, true);
    SetPref(prefs::kSafeBrowsingScoutReportingEnabled, true);
    SetPref(prefs::kSafeBrowsingScoutGroupSelected, true);

    MockTriggerThrottler* mock_throttler = new MockTriggerThrottler();
    ON_CALL(*mock_throttler, TriggerCanFire(_)).WillByDefault(Return(true));
    // Trigger Manager takes ownership of the mock throttler.
    trigger_manager_.set_trigger_throttler(mock_throttler);
  }

  void SetPref(const std::string& pref, bool value) {
    pref_service_.SetBoolean(pref, value);
  }

  void SetManagedPref(const std::string& pref, bool value) {
    pref_service_.SetManagedPref(pref, std::make_unique<base::Value>(value));
  }

  bool GetPref(const std::string& pref) {
    return pref_service_.GetBoolean(pref);
  }

  void SetTriggerHasQuota(const TriggerType trigger_type, bool has_quota) {
    MockTriggerThrottler* mock_throttler = static_cast<MockTriggerThrottler*>(
        trigger_manager_.trigger_throttler_.get());
    EXPECT_CALL(*mock_throttler, TriggerCanFire(trigger_type))
        .WillOnce(Return(has_quota));
  }

  content::WebContents* CreateWebContents() {
    DCHECK(!browser_context_.IsOffTheRecord())
        << "CreateWebContents() should not be called after "
           "CreateIncognitoWebContents()";
    return web_contents_factory_.CreateWebContents(&browser_context_);
  }

  content::WebContents* CreateIncognitoWebContents() {
    browser_context_.set_is_off_the_record(true);
    return web_contents_factory_.CreateWebContents(&browser_context_);
  }

  bool StartCollectingThreatDetails(const TriggerType trigger_type,
                                    content::WebContents* web_contents) {
    SBErrorOptions options =
        TriggerManager::GetSBErrorDisplayOptions(pref_service_, *web_contents);
    return trigger_manager_.StartCollectingThreatDetails(
        trigger_type, web_contents, security_interstitials::UnsafeResource(),
        nullptr, nullptr, options);
  }

  bool FinishCollectingThreatDetails(const TriggerType trigger_type,
                                     content::WebContents* web_contents,
                                     bool expect_report_sent) {
    if (expect_report_sent) {
      MockThreatDetails* threat_details = static_cast<MockThreatDetails*>(
          trigger_manager_.data_collectors_map_[web_contents]
              .threat_details.get());
      EXPECT_CALL(*threat_details, FinishCollection(_, _)).Times(1);
    }
    SBErrorOptions options =
        TriggerManager::GetSBErrorDisplayOptions(pref_service_, *web_contents);
    bool result = trigger_manager_.FinishCollectingThreatDetails(
        trigger_type, web_contents, base::TimeDelta(), false, 0, options);

    // Invoke the callback if the report was to be sent.
    if (expect_report_sent)
      trigger_manager_.ThreatDetailsDone(web_contents);

    return result;
  }

  const DataCollectorsMap& data_collectors_map() {
    return trigger_manager_.data_collectors_map_;
  }

  void SetCollectDontSendFeature(bool enabled) {
    feature_list_.reset(new base::test::ScopedFeatureList);
    if (enabled)
      feature_list_->InitAndEnableFeature(kAdSamplerCollectButDontSendFeature);
    else
      feature_list_->InitAndDisableFeature(kAdSamplerCollectButDontSendFeature);
  }

 private:
  TriggerManager trigger_manager_;
  MockThreatDetailsFactory mock_threat_details_factory_;
  content::TestBrowserThreadBundle thread_bundle_;
  content::TestBrowserContext browser_context_;
  content::TestWebContentsFactory web_contents_factory_;
  TestingPrefServiceSimple pref_service_;
  std::unique_ptr<base::test::ScopedFeatureList> feature_list_;

  DISALLOW_COPY_AND_ASSIGN(TriggerManagerTest);
};

TEST_F(TriggerManagerTest, StartAndFinishCollectingThreatDetails) {
  // Basic workflow is to start and finish data collection with a single
  // WebContents.
  content::WebContents* web_contents1 = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents1));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents1)));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents1, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents1).threat_details);

  // More complex scenarios can happen, where collection happens on two
  // WebContents at the same time, possibly starting and completing in different
  // order.
  content::WebContents* web_contents2 = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents1));
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents2));
  EXPECT_THAT(data_collectors_map(),
              UnorderedElementsAre(Key(web_contents1), Key(web_contents2)));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents2).threat_details);
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents2, true));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents2).threat_details);
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents1, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents2).threat_details);

  // Calling Start twice with the same WebContents is an error, and will return
  // false the second time. But it can still be completed normally.
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents1));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_FALSE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents1));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents1, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents1).threat_details);

  // Calling Finish twice with the same WebContents is an error, and will return
  // false the second time. It's basically a no-op.
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents1));
  EXPECT_NE(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents1, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents1).threat_details);
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents1, false));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents1).threat_details);
}

TEST_F(TriggerManagerTest, NoDataCollection_Incognito) {
  // Data collection will not begin and no reports will be sent when incognito.
  content::WebContents* web_contents = CreateIncognitoWebContents();
  EXPECT_FALSE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents));
  EXPECT_TRUE(data_collectors_map().empty());
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_TRUE(data_collectors_map().empty());
}

TEST_F(TriggerManagerTest, NoDataCollection_SBEROptInDisallowed) {
  // Data collection will not begin and no reports will be sent when the user is
  // not allowed to opt-in to SBER.
  SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, false);
  content::WebContents* web_contents = CreateWebContents();
  EXPECT_FALSE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents));
  EXPECT_TRUE(data_collectors_map().empty());
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_TRUE(data_collectors_map().empty());
}

TEST_F(TriggerManagerTest, NoDataCollection_IncognitoAndSBEROptInDisallowed) {
  // Data collection will not begin and no reports will be sent when the user is
  // not allowed to opt-in to SBER and is also incognito.
  SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, false);
  content::WebContents* web_contents = CreateIncognitoWebContents();
  EXPECT_FALSE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents));
  EXPECT_TRUE(data_collectors_map().empty());
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_TRUE(data_collectors_map().empty());
}

TEST_F(TriggerManagerTest, UserOptedOutOfSBER_DataCollected_NoReportSent) {
  // When the user is opted-out of SBER then data collection will begin but no
  // report will be sent when data collection ends.
  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, false);
  content::WebContents* web_contents = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);
}

TEST_F(TriggerManagerTest, UserOptsOutOfSBER_DataCollected_NoReportSent) {
  // If the user opts-out of Extended Reporting while data is being collected
  // then no report is sent. Note that the test fixture opts the user into
  // Extended Reporting by default.
  content::WebContents* web_contents = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));

  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, false);

  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);
}

TEST_F(TriggerManagerTest, UserOptsInToSBER_DataCollected_ReportSent) {
  // When the user is opted-out of SBER then data collection will begin. If they
  // opt-in to SBER while data collection is in progress then the report will
  // also be sent.
  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, false);
  content::WebContents* web_contents = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));

  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, true);

  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                            web_contents, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);
}

TEST_F(TriggerManagerTest,
       SBEROptInBecomesDisallowed_DataCollected_NoReportSent) {
  // If the user loses the ability to opt-in to SBER in the middle of data
  // collection then the report will not be sent.
  content::WebContents* web_contents = CreateWebContents();
  EXPECT_TRUE(StartCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                           web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));

  // Remove the ability to opt-in to SBER.
  SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, false);

  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::SECURITY_INTERSTITIAL,
                                             web_contents, false));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);
}

TEST_F(TriggerManagerTest, NoCollectionWhenOutOfQuota) {
  // Triggers are not allowed to collect data when they're out of quota, even if
  // all other conditions are as expected.
  content::WebContents* web_contents = CreateWebContents();

  // Turn on the AD_SAMPLE trigger inside the throttler and confirm that it can
  // fire normally.
  SetTriggerHasQuota(TriggerType::AD_SAMPLE, true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);

  // Turn off the AD_SAMPLE trigger inside the throttler, the trigger should no
  // longer be able to fire.
  SetTriggerHasQuota(TriggerType::AD_SAMPLE, false);
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);
}

TEST_F(TriggerManagerTest, NoCollectionWhenSBERDisabledByPolicy) {
  // Confirm that disabling SBER through an enterprise policy does disable
  // triggers.
  content::WebContents* web_contents = CreateWebContents();

  SetManagedPref(prefs::kSafeBrowsingScoutReportingEnabled, false);
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(data_collectors_map().empty());
  EXPECT_FALSE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                             web_contents, false));
  EXPECT_TRUE(data_collectors_map().empty());
}

TEST_F(TriggerManagerTest, AdSamplerTrigger) {
  // Check the conditions required for the Ad Sampler trigger to fire. It needs
  // opt-in to start collecting data, scout opt-in, and quota.
  content::WebContents* web_contents = CreateWebContents();

  // The default setup in this test makes the trigger fire (all prefs enabled,
  // all triggers have quota).
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_THAT(data_collectors_map(), UnorderedElementsAre(Key(web_contents)));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
  EXPECT_EQ(nullptr, data_collectors_map().at(web_contents).threat_details);

  // Disabling SBEROptInAllowed disables this trigger.
  SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, false);
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  // It can be forced on via a finch feature.
  SetCollectDontSendFeature(true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
  SetCollectDontSendFeature(false);
  // Confirm it can fire when we re-enable SBEROptInAllowed
  SetPref(prefs::kSafeBrowsingExtendedReportingOptInAllowed, true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));

  // Disabling Scout disables this trigger even if the legacy SBER is enabled.
  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, false);
  SetPref(prefs::kSafeBrowsingExtendedReportingEnabled, true);
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  // It can be forced on via a finch feature.
  SetCollectDontSendFeature(true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
  SetCollectDontSendFeature(false);
  // Confirm it can fire when we re-enable Scout and disable legacy SBER.
  SetPref(prefs::kSafeBrowsingScoutReportingEnabled, true);
  SetPref(prefs::kSafeBrowsingExtendedReportingEnabled, false);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));

  // Finally, make sure the trigger can't fire if it has no quota.
  SetTriggerHasQuota(TriggerType::AD_SAMPLE, false);
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  // It can be forced on via a finch feature.
  SetCollectDontSendFeature(true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
  SetCollectDontSendFeature(false);
  // Confirm it can fire again when quota is available.
  SetTriggerHasQuota(TriggerType::AD_SAMPLE, true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
}

TEST_F(TriggerManagerTest, AdSamplerTrigger_Incognito) {
  // Check the conditions required for the Ad Sampler trigger to fire. It needs
  // opt-in to start collecting data, scout opt-in, and quota, and it can't fire
  // in inconito (except when forced on by finch feature).
  content::WebContents* web_contents = CreateIncognitoWebContents();

  // The default setup in this test makes the trigger fire (all prefs enabled,
  // all triggers have quota), but the incognito window prevents it from firing.
  EXPECT_FALSE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));

  // The Finch feature makes the trigger fire even in incognito (which is safe
  // because data is discarded and not sent to Google downstream).
  SetCollectDontSendFeature(true);
  EXPECT_TRUE(
      StartCollectingThreatDetails(TriggerType::AD_SAMPLE, web_contents));
  EXPECT_TRUE(FinishCollectingThreatDetails(TriggerType::AD_SAMPLE,
                                            web_contents, true));
}
}  // namespace safe_browsing
