// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/base_switches.h"
#include "base/macros.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/autofill/content/browser/content_autofill_driver.h"
#include "components/autofill/content/browser/content_autofill_driver_factory.h"
#include "components/autofill/core/browser/test_autofill_client.h"
#include "components/autofill/core/browser/test_autofill_provider.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/autofill/core/common/form_data.h"
#include "components/autofill/core/common/submission_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::Invoke;

namespace autofill {
namespace {

const base::FilePath::CharType kDocRoot[] =
    FILE_PATH_LITERAL("chrome/test/data");

class MockAutofillProvider : public TestAutofillProvider {
 public:
  MockAutofillProvider() {}
  ~MockAutofillProvider() override {}

  MOCK_METHOD5(OnFormSubmitted,
               void(AutofillHandlerProxy* handler,
                    const FormData& form,
                    bool,
                    SubmissionSource,
                    base::TimeTicks));

  MOCK_METHOD5(OnQueryFormFieldAutofill,
               void(AutofillHandlerProxy* handler,
                    int32_t id,
                    const FormData& form,
                    const FormFieldData& field,
                    const gfx::RectF& bounding_box));

  void OnQueryFormFieldAutofillImpl(AutofillHandlerProxy* handler,
                                    int32_t id,
                                    const FormData& form,
                                    const FormFieldData& field,
                                    const gfx::RectF& bounding_box) {
    queried_form_ = form;
    is_queried_ = true;
  }

  void OnFormSubmittedImpl(AutofillHandlerProxy*,
                           const FormData& form,
                           bool success,
                           SubmissionSource source,
                           base::TimeTicks timestamp) {
    submitted_form_ = form;
  }

  const FormData& queried_form() { return queried_form_; }

  const FormData& submitted_form() { return submitted_form_; }

  bool is_queried() { return is_queried_; }

 private:
  bool is_queried_ = false;
  FormData queried_form_;
  FormData submitted_form_;
};

}  // namespace

class AutofillProviderBrowserTest : public InProcessBrowserTest {
 public:
  AutofillProviderBrowserTest() {}
  ~AutofillProviderBrowserTest() override {}

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(::switches::kForceFieldTrials,
                                    "AutofillSingleClick/Disabled");
  }

  void SetUpOnMainThread() override {
    autofill_provider_ = std::make_unique<MockAutofillProvider>();
    embedded_test_server()->AddDefaultHandlers(base::FilePath(kDocRoot));
    // Serve both a.com and b.com (and any other domain).
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  void CreateContentAutofillDriverFactoryForSubFrame() {
    content::WebContents* web_contents = WebContents();
    ASSERT_TRUE(web_contents != NULL);

    web_contents->RemoveUserData(
        ContentAutofillDriverFactory::
            kContentAutofillDriverFactoryWebContentsUserDataKey);

    // Replace the ContentAutofillDriverFactory for sub frame.
    ContentAutofillDriverFactory::CreateForWebContentsAndDelegate(
        web_contents, &autofill_client_, "en-US",
        AutofillManager::DISABLE_AUTOFILL_DOWNLOAD_MANAGER,
        autofill_provider_.get());
  }

  void ReplaceAutofillDriver() {
    content::WebContents* web_contents = WebContents();
    // Set AutofillProvider for current WebContents.
    ContentAutofillDriverFactory* factory =
        ContentAutofillDriverFactory::FromWebContents(web_contents);
    ContentAutofillDriver* driver =
        factory->DriverForFrame(web_contents->GetMainFrame());
    driver->SetAutofillProviderForTesting(autofill_provider_.get());
  }

  void TearDownOnMainThread() override {
    testing::Mock::VerifyAndClearExpectations(autofill_provider_.get());
  }

  content::RenderViewHost* RenderViewHost() {
    return WebContents()->GetRenderViewHost();
  }

  content::WebContents* WebContents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  void SimulateUserTypingInFocuedField() {
    content::WebContents* web_contents = WebContents();

    content::SimulateKeyPress(web_contents, ui::DomKey::FromCharacter('O'),
                              ui::DomCode::US_O, ui::VKEY_O, false, false,
                              false, false);
    content::SimulateKeyPress(web_contents, ui::DomKey::FromCharacter('R'),
                              ui::DomCode::US_R, ui::VKEY_R, false, false,
                              false, false);
    content::SimulateKeyPress(web_contents, ui::DomKey::FromCharacter('A'),
                              ui::DomCode::US_A, ui::VKEY_A, false, false,
                              false, false);
    content::SimulateKeyPress(web_contents, ui::DomKey::FromCharacter('R'),
                              ui::DomCode::US_R, ui::VKEY_R, false, false,
                              false, false);
    content::SimulateKeyPress(web_contents, ui::DomKey::FromCharacter('Y'),
                              ui::DomCode::US_Y, ui::VKEY_Y, false, false,
                              false, false);
  }

  void SetLabelChangeExpectationAndTriggerQuery() {
    ReplaceAutofillDriver();

    // If AutofillSingleClick is enabled, there may be multiple queries.
    EXPECT_CALL(*autofill_provider_, OnQueryFormFieldAutofill(_, _, _, _, _))
        .WillOnce(Invoke(autofill_provider_.get(),
                         &MockAutofillProvider::OnQueryFormFieldAutofillImpl));

    EXPECT_CALL(*autofill_provider_, OnFormSubmitted(_, _, _, _, _))
        .WillOnce(Invoke(autofill_provider_.get(),
                         &MockAutofillProvider::OnFormSubmittedImpl));

    ui_test_utils::NavigateToURL(browser(), embedded_test_server()->GetURL(
                                                "/autofill/label_change.html"));

    std::string focus("document.getElementById('address').focus();");
    ASSERT_TRUE(content::ExecuteScript(RenderViewHost(), focus));

    SimulateUserTypingInFocuedField();
    while (!autofill_provider_->is_queried()) {
      base::RunLoop().RunUntilIdle();
    }
  }

  void ChangeLabelAndCheckResult(const std::string& element_id,
                                 bool expect_forms_same) {
    std::string change_label_and_submit =
        "document.getElementById('" + element_id +
        "').innerHTML='address change';"
        "document.getElementById('submit_button').click();";

    ASSERT_TRUE(
        content::ExecuteScript(RenderViewHost(), change_label_and_submit));
    // Need to pay attention for a message that XHR has finished since there
    // is no navigation to wait for.
    content::DOMMessageQueue message_queue;
    std::string message;
    while (message_queue.WaitForMessage(&message)) {
      if (message == "\"SUBMISSION_FINISHED\"")
        break;
    }

    EXPECT_EQ("\"SUBMISSION_FINISHED\"", message);
    EXPECT_EQ(2u, autofill_provider_->queried_form().fields.size());
    EXPECT_EQ(2u, autofill_provider_->submitted_form().fields.size());
    EXPECT_EQ(expect_forms_same,
              autofill_provider_->submitted_form().SimilarFormAs(
                  autofill_provider_->queried_form()));
  }

 protected:
  std::unique_ptr<MockAutofillProvider> autofill_provider_;
  base::test::ScopedFeatureList scoped_feature_list_;

 private:
  TestAutofillClient autofill_client_;
};

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       FrameDetachedOnFormlessSubmission) {
  CreateContentAutofillDriverFactoryForSubFrame();
  EXPECT_CALL(*autofill_provider_,
              OnFormSubmitted(_, _, _, SubmissionSource::FRAME_DETACHED, _))
      .Times(1);
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/autofill/frame_detached_on_formless_submit.html"));

  // Need to pay attention for a message that XHR has finished since there
  // is no navigation to wait for.
  content::DOMMessageQueue message_queue;

  std::string focus(
      "var iframe = document.getElementById('address_iframe');"
      "var frame_doc = iframe.contentDocument;"
      "frame_doc.getElementById('address_field').focus();");
  ASSERT_TRUE(content::ExecuteScript(RenderViewHost(), focus));

  SimulateUserTypingInFocuedField();
  std::string fill_and_submit =
      "var iframe = document.getElementById('address_iframe');"
      "var frame_doc = iframe.contentDocument;"
      "frame_doc.getElementById('submit_button').click();";

  ASSERT_TRUE(content::ExecuteScript(RenderViewHost(), fill_and_submit));
  std::string message;
  while (message_queue.WaitForMessage(&message)) {
    if (message == "\"SUBMISSION_FINISHED\"")
      break;
  }
  EXPECT_EQ("\"SUBMISSION_FINISHED\"", message);
}

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       FrameDetachedOnFormSubmission) {
  CreateContentAutofillDriverFactoryForSubFrame();
  EXPECT_CALL(*autofill_provider_,
              OnFormSubmitted(_, _, _, SubmissionSource::FORM_SUBMISSION, _))
      .Times(1);
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL(
                     "/autofill/frame_detached_on_form_submit.html"));

  // Need to pay attention for a message that XHR has finished since there
  // is no navigation to wait for.
  content::DOMMessageQueue message_queue;

  std::string focus(
      "var iframe = document.getElementById('address_iframe');"
      "var frame_doc = iframe.contentDocument;"
      "frame_doc.getElementById('address_field').focus();");
  ASSERT_TRUE(content::ExecuteScript(RenderViewHost(), focus));

  SimulateUserTypingInFocuedField();
  std::string fill_and_submit =
      "var iframe = document.getElementById('address_iframe');"
      "var frame_doc = iframe.contentDocument;"
      "frame_doc.getElementById('submit_button').click();";

  ASSERT_TRUE(content::ExecuteScript(RenderViewHost(), fill_and_submit));
  std::string message;
  while (message_queue.WaitForMessage(&message)) {
    if (message == "\"SUBMISSION_FINISHED\"")
      break;
  }
  EXPECT_EQ("\"SUBMISSION_FINISHED\"", message);
}

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       LabelTagChangeImpactFormComparingWithFlagOn) {
  scoped_feature_list_.InitAndEnableFeature(
      features::kAutofillSkipComparingInferredLabels);
  SetLabelChangeExpectationAndTriggerQuery();
  ChangeLabelAndCheckResult("label_id", false /*expect_forms_same*/);
}

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       InferredLabelChangeNotImpactFormComparingWithFlagOn) {
  scoped_feature_list_.InitAndEnableFeature(
      features::kAutofillSkipComparingInferredLabels);
  SetLabelChangeExpectationAndTriggerQuery();
  ChangeLabelAndCheckResult("p_id", true /*expect_forms_same*/);
}

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       LabelTagChangeImpactFormComparingWithFlagOff) {
  scoped_feature_list_.InitAndDisableFeature(
      features::kAutofillSkipComparingInferredLabels);
  SetLabelChangeExpectationAndTriggerQuery();
  ChangeLabelAndCheckResult("label_id", false /*expect_forms_same*/);
}

IN_PROC_BROWSER_TEST_F(AutofillProviderBrowserTest,
                       InferredLabelChangeImpactFormComparingWithFlagOff) {
  scoped_feature_list_.InitAndDisableFeature(
      features::kAutofillSkipComparingInferredLabels);
  SetLabelChangeExpectationAndTriggerQuery();
  ChangeLabelAndCheckResult("p_id", false /*expect_forms_same*/);
}

}  // namespace autofill
