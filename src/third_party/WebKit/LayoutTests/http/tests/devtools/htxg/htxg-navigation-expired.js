// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
(async function() {
  TestRunner.addResult('Tests the signed exchange information are available when the navigation failed.\n');
  await TestRunner.loadModule('network_test_runner');
  await TestRunner.loadModule('console_test_runner');
  await TestRunner.showPanel('network');
  await TestRunner.addScriptTag('/loading/htxg/resources/htxg-util.js');
  // The timestamp of the test HTXG file is "Apr 1 2018 00:00 UTC" and valid
  // until "Apr 8 2018 00:00 UTC". So in Apr 10, the page load should fail.
  await TestRunner.evaluateInPageAsync(
    'setSignedExchangeVerificationTime(new Date("Apr 10 2018 00:01 UTC"))');
  BrowserSDK.networkLog.reset();
  await TestRunner.addIframe('/loading/htxg/resources/htxg-location.htxg');
  ConsoleTestRunner.dumpConsoleMessages();
  for (var request of BrowserSDK.networkLog.requests()) {
    TestRunner.addResult(`* ${request.url()}`);
    TestRunner.addResult(`  failed: ${!!request.failed}`);
    TestRunner.addResult(`  statusCode: ${request.statusCode}`);
    TestRunner.addResult(`  resourceType: ${request.resourceType().name()}`);
    // TODO(crbug/830505): Check the existance of signed exchange information.
  }
  TestRunner.completeTest();
})();
