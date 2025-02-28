// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/sys_info.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_timeouts.h"
#include "build/build_config.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_paths.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/test_utils.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "net/base/escape.h"
#include "net/ssl/ssl_server_config.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/spawned_test_server/spawned_test_server.h"
#include "net/test/test_data_directory.h"
#include "url/gurl.h"

namespace content {

namespace {

bool SupportsSharedWorker() {
#if defined(OS_ANDROID)
  // SharedWorkers are not enabled on Android. https://crbug.com/154571
  //
  // TODO(davidben): Move other SharedWorker exclusions from
  // build/android/pylib/gtest/filter/ inline.
  return false;
#else
  return true;
#endif
}

}  // namespace

class WorkerTest : public ContentBrowserTest {
 public:
  WorkerTest() : select_certificate_count_(0) {}

  void SetUpOnMainThread() override {
    ShellContentBrowserClient::Get()->set_select_client_certificate_callback(
        base::Bind(&WorkerTest::OnSelectClientCertificate,
                   base::Unretained(this)));
  }

  void TearDownOnMainThread() override {
    ShellContentBrowserClient::Get()->set_select_client_certificate_callback(
        base::Closure());
  }

  int select_certificate_count() const { return select_certificate_count_; }

  GURL GetTestURL(const std::string& test_case, const std::string& query) {
    base::FilePath test_file_path = GetTestFilePath(
        "workers", test_case.c_str());
    return GetFileUrlWithQuery(test_file_path, query);
  }

  void RunTest(Shell* window, const GURL& url) {
    const base::string16 expected_title = base::ASCIIToUTF16("OK");
    TitleWatcher title_watcher(window->web_contents(), expected_title);
    NavigateToURL(window, url);
    base::string16 final_title = title_watcher.WaitAndGetTitle();
    EXPECT_EQ(expected_title, final_title);
  }

  void RunTest(const GURL& url) { RunTest(shell(), url); }

  static void QuitUIMessageLoop(base::Callback<void()> callback) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE, std::move(callback));
  }

  void NavigateAndWaitForAuth(const GURL& url) {
    ShellContentBrowserClient* browser_client =
        ShellContentBrowserClient::Get();
    scoped_refptr<MessageLoopRunner> runner = new MessageLoopRunner();
    browser_client->set_login_request_callback(
        base::Bind(&QuitUIMessageLoop, runner->QuitClosure()));
    shell()->LoadURL(url);
    runner->Run();
  }

 private:
  void OnSelectClientCertificate() { select_certificate_count_++; }

  int select_certificate_count_;
};

IN_PROC_BROWSER_TEST_F(WorkerTest, SingleWorker) {
  RunTest(GetTestURL("single_worker.html", std::string()));
}

IN_PROC_BROWSER_TEST_F(WorkerTest, MultipleWorkers) {
  RunTest(GetTestURL("multi_worker.html", std::string()));
}

IN_PROC_BROWSER_TEST_F(WorkerTest, SingleSharedWorker) {
  if (!SupportsSharedWorker())
    return;

  RunTest(GetTestURL("single_worker.html", "shared=true"));
}

// http://crbug.com/96435
IN_PROC_BROWSER_TEST_F(WorkerTest, MultipleSharedWorkers) {
  if (!SupportsSharedWorker())
    return;

  RunTest(GetTestURL("multi_worker.html", "shared=true"));
}

// Incognito windows should not share workers with non-incognito windows
// http://crbug.com/30021
IN_PROC_BROWSER_TEST_F(WorkerTest, IncognitoSharedWorkers) {
  if (!SupportsSharedWorker())
    return;

  // Launch the server to host a shared worker on http environment because a
  // local file (file://) is treated like it has an opaque origin and the
  // shared worker on the origin cannot be shared.
  ASSERT_TRUE(embedded_test_server()->Start());

  // Load a non-incognito tab and have it create a shared worker
  RunTest(embedded_test_server()->GetURL("/workers/incognito_worker.html"));

  // Incognito worker should not share with non-incognito
  RunTest(CreateOffTheRecordBrowser(),
          embedded_test_server()->GetURL("/workers/incognito_worker.html"));
}

// Make sure that auth dialog is displayed from worker context.
// http://crbug.com/33344
IN_PROC_BROWSER_TEST_F(WorkerTest, WorkerHttpAuth) {
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/workers/worker_auth.html");

  NavigateAndWaitForAuth(url);
}

// Tests that TLS client auth prompts for normal workers's importScripts.
IN_PROC_BROWSER_TEST_F(WorkerTest, WorkerTlsClientAuthImportScripts) {
  // Launch HTTPS server.
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  net::SSLServerConfig ssl_config;
  ssl_config.client_cert_type =
      net::SSLServerConfig::ClientCertType::REQUIRE_CLIENT_CERT;
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_OK, ssl_config);
  ASSERT_TRUE(https_server.Start());

  RunTest(GetTestURL(
      "worker_tls_client_auth.html",
      "test=import&url=" +
          net::EscapeQueryParamValue(https_server.GetURL("/").spec(), true)));
  EXPECT_EQ(1, select_certificate_count());
}

// Tests that TLS client auth prompts for normal workers's fetch() call.
IN_PROC_BROWSER_TEST_F(WorkerTest, WorkerTlsClientAuthFetch) {
  // Launch HTTPS server.
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  net::SSLServerConfig ssl_config;
  ssl_config.client_cert_type =
      net::SSLServerConfig::ClientCertType::REQUIRE_CLIENT_CERT;
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_OK, ssl_config);
  ASSERT_TRUE(https_server.Start());

  RunTest(GetTestURL(
      "worker_tls_client_auth.html",
      "test=fetch&url=" +
          net::EscapeQueryParamValue(https_server.GetURL("/").spec(), true)));
  EXPECT_EQ(1, select_certificate_count());
}

// Tests that TLS client auth does not prompt for a shared worker; shared
// workers are not associated with a WebContents.
IN_PROC_BROWSER_TEST_F(WorkerTest, SharedWorkerTlsClientAuthImportScripts) {
  if (!SupportsSharedWorker())
    return;

  // Launch HTTPS server.
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.ServeFilesFromSourceDirectory("content/test/data");
  net::SSLServerConfig ssl_config;
  ssl_config.client_cert_type =
      net::SSLServerConfig::ClientCertType::REQUIRE_CLIENT_CERT;
  https_server.SetSSLConfig(net::EmbeddedTestServer::CERT_OK, ssl_config);
  ASSERT_TRUE(https_server.Start());

  RunTest(GetTestURL(
      "worker_tls_client_auth.html",
      "test=import&shared=true&url=" +
          net::EscapeQueryParamValue(https_server.GetURL("/").spec(), true)));
  EXPECT_EQ(0, select_certificate_count());
}

IN_PROC_BROWSER_TEST_F(WorkerTest, WebSocketSharedWorker) {
  if (!SupportsSharedWorker())
    return;

  // Launch WebSocket server.
  net::SpawnedTestServer ws_server(net::SpawnedTestServer::TYPE_WS,
                                   net::GetWebSocketTestDataDirectory());
  ASSERT_TRUE(ws_server.Start());

  // Generate test URL.
  GURL::Replacements replacements;
  replacements.SetSchemeStr("http");
  GURL url = ws_server.GetURL(
      "websocket_shared_worker.html").ReplaceComponents(replacements);

  // Run test.
  Shell* window = shell();
  const base::string16 expected_title = base::ASCIIToUTF16("OK");
  TitleWatcher title_watcher(window->web_contents(), expected_title);
  NavigateToURL(window, url);
  base::string16 final_title = title_watcher.WaitAndGetTitle();
  EXPECT_EQ(expected_title, final_title);
}

IN_PROC_BROWSER_TEST_F(WorkerTest, PassMessagePortToSharedWorker) {
  if (!SupportsSharedWorker())
    return;

  RunTest(GetTestURL("pass_messageport_to_sharedworker.html", ""));
}

IN_PROC_BROWSER_TEST_F(WorkerTest,
                       PassMessagePortToSharedWorkerDontWaitForConnect) {
  if (!SupportsSharedWorker())
    return;

  RunTest(GetTestURL(
      "pass_messageport_to_sharedworker_dont_wait_for_connect.html", ""));
}

}  // namespace content
