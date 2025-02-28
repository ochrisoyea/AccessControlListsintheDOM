// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync_sessions/session_sync_bridge.h"

#include <map>
#include <utility>
#include <vector>

#include "base/bind_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/test/bind_test_util.h"
#include "base/test/mock_callback.h"
#include "components/sync/base/hash_util.h"
#include "components/sync/base/sync_prefs.h"
#include "components/sync/device_info/local_device_info_provider_mock.h"
#include "components/sync/model/data_batch.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/mock_model_type_change_processor.h"
#include "components/sync/model/model_type_store_test_util.h"
#include "components/sync/model/model_type_sync_bridge.h"
#include "components/sync/model/sync_metadata_store.h"
#include "components/sync/model_impl/client_tag_based_model_type_processor.h"
#include "components/sync/protocol/proto_value_conversions.h"
#include "components/sync/protocol/sync.pb.h"
#include "components/sync/test/test_matchers.h"
#include "components/sync_sessions/favicon_cache.h"
#include "components/sync_sessions/mock_sync_sessions_client.h"
#include "components/sync_sessions/test_matchers.h"
#include "components/sync_sessions/test_synced_window_delegates_getter.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace sync_sessions {
namespace {

using sync_pb::EntityMetadata;
using sync_pb::SessionSpecifics;
using syncer::DataBatch;
using syncer::EntityChangeList;
using syncer::EntityData;
using syncer::IsEmptyMetadataBatch;
using syncer::MetadataBatch;
using syncer::MetadataChangeList;
using syncer::MockModelTypeChangeProcessor;
using testing::Contains;
using testing::ElementsAre;
using testing::Eq;
using testing::InSequence;
using testing::IsEmpty;
using testing::IsNull;
using testing::Matcher;
using testing::Not;
using testing::NotNull;
using testing::Pair;
using testing::Pointee;
using testing::UnorderedElementsAre;
using testing::Return;
using testing::SaveArg;
using testing::SizeIs;
using testing::WithArg;
using testing::_;

const char kLocalSessionTag[] = "sessiontag1";

class MockSessionSyncPrefs : public syncer::SessionSyncPrefs {
 public:
  MockSessionSyncPrefs() = default;
  ~MockSessionSyncPrefs() override = default;

  MOCK_CONST_METHOD0(GetSyncSessionsGUID, std::string());
  MOCK_METHOD1(SetSyncSessionsGUID, void(const std::string& guid));
};

MATCHER_P(EntityDataHasSpecifics, session_specifics_matcher, "") {
  return session_specifics_matcher.MatchAndExplain(arg->specifics.session(),
                                                   result_listener);
}

syncer::EntityDataPtr SpecificsToEntity(
    const sync_pb::SessionSpecifics& specifics,
    base::Time mtime = base::Time::Now()) {
  syncer::EntityData data;
  data.client_tag_hash = syncer::GenerateSyncableHash(
      syncer::SESSIONS, SessionStore::GetClientTag(specifics));
  *data.specifics.mutable_session() = specifics;
  data.modification_time = mtime;
  return data.PassToPtr();
}

syncer::UpdateResponseData SpecificsToUpdateResponse(
    const sync_pb::SessionSpecifics& specifics,
    base::Time mtime = base::Time::Now()) {
  syncer::UpdateResponseData data;
  data.entity = SpecificsToEntity(specifics, mtime);
  return data;
}

std::map<std::string, std::unique_ptr<EntityData>> BatchToEntityDataMap(
    std::unique_ptr<DataBatch> batch) {
  std::map<std::string, std::unique_ptr<EntityData>> storage_key_to_data;
  while (batch && batch->HasNext()) {
    storage_key_to_data.insert(batch->Next());
  }
  return storage_key_to_data;
}

syncer::UpdateResponseData CreateTombstone(const std::string& client_tag) {
  EntityData tombstone;
  tombstone.client_tag_hash =
      syncer::GenerateSyncableHash(syncer::SESSIONS, client_tag);

  syncer::UpdateResponseData data;
  data.entity = tombstone.PassToPtr();
  data.response_version = 2;
  return data;
}

syncer::CommitResponseData CreateSuccessResponse(
    const std::string& client_tag) {
  syncer::CommitResponseData response;
  response.client_tag_hash =
      syncer::GenerateSyncableHash(syncer::SESSIONS, client_tag);
  response.sequence_number = 1;
  return response;
}

sync_pb::SessionSpecifics CreateHeaderSpecificsWithOneTab(
    const std::string& session_tag,
    int window_id,
    int tab_id) {
  sync_pb::SessionSpecifics specifics;
  specifics.set_session_tag(session_tag);
  specifics.mutable_header()->set_client_name("Some client name");
  specifics.mutable_header()->set_device_type(
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX);
  sync_pb::SessionWindow* window = specifics.mutable_header()->add_window();
  window->set_browser_type(sync_pb::SessionWindow_BrowserType_TYPE_TABBED);
  window->set_window_id(window_id);
  window->add_tab(tab_id);
  return specifics;
}

sync_pb::SessionSpecifics CreateTabSpecifics(const std::string& session_tag,
                                             int window_id,
                                             int tab_id,
                                             int tab_node_id,
                                             const std::string& url) {
  sync_pb::SessionSpecifics specifics;
  specifics.set_session_tag(session_tag);
  specifics.set_tab_node_id(tab_node_id);
  specifics.mutable_tab()->add_navigation()->set_virtual_url(url);
  specifics.mutable_tab()->set_window_id(window_id);
  specifics.mutable_tab()->set_tab_id(tab_id);
  return specifics;
}

class SessionSyncBridgeTest : public ::testing::Test {
 protected:
  SessionSyncBridgeTest()
      : store_(syncer::ModelTypeStoreTestUtil::CreateInMemoryStoreForTest(
            syncer::SESSIONS)),
        favicon_cache_(/*favicon_service=*/nullptr,
                       /*history_service=*/nullptr,
                       /*max_sync_favicon_limit=*/0) {
    ON_CALL(mock_sync_sessions_client_, GetSyncedWindowDelegatesGetter())
        .WillByDefault(Return(&window_getter_));
    ON_CALL(mock_sync_sessions_client_, GetLocalSessionEventRouter())
        .WillByDefault(Return(window_getter_.router()));
    ON_CALL(mock_sync_prefs_, GetSyncSessionsGUID())
        .WillByDefault(Return(kLocalSessionTag));

    // Even if we use NiceMock, let's be strict about errors and let tests
    // explicitly list them.
    EXPECT_CALL(mock_processor_, ReportError(_)).Times(0);
  }

  ~SessionSyncBridgeTest() override {}

  void InitializeBridge() {
    real_processor_ =
        std::make_unique<syncer::ClientTagBasedModelTypeProcessor>(
            syncer::SESSIONS, /*dump_stack=*/base::DoNothing(),
            /*commit_only=*/false);
    mock_processor_.DelegateCallsByDefaultTo(real_processor_.get());
    // Instantiate the bridge.
    bridge_ = std::make_unique<SessionSyncBridge>(
        &mock_sync_sessions_client_, &mock_sync_prefs_,
        &mock_device_info_provider_,
        /*store_factory=*/
        syncer::ModelTypeStoreTestUtil::FactoryForForwardingStore(store_.get()),
        mock_foreign_sessions_updated_callback_.Get(),
        mock_processor_.CreateForwardingProcessor());
  }

  void ShutdownBridge() {
    bridge_.reset();
    // The mock is still delegating to |real_processor_|, so we reset it too.
    ASSERT_TRUE(testing::Mock::VerifyAndClear(&mock_processor_));
    real_processor_.reset();
  }

  void StartSyncing(const std::vector<SessionSpecifics>& remote_data = {}) {
    // DeviceInfo is provided when sync is being enabled, which should lead to
    // ModelReadyToSync().
    mock_device_info_provider_.Initialize(std::make_unique<syncer::DeviceInfo>(
        "cache_guid", "Wayne Gretzky's Hacking Box", "Chromium 10k",
        "Chrome 10k", sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "device_id"));

    base::RunLoop loop;
    real_processor_->OnSyncStarting(
        /*error_handler=*/base::DoNothing(),
        base::BindLambdaForTesting(
            [&loop](std::unique_ptr<syncer::ActivationContext>) {
              loop.Quit();
            }));
    loop.Run();

    sync_pb::ModelTypeState state;
    state.set_initial_sync_done(true);
    syncer::UpdateResponseDataList initial_updates;
    for (const SessionSpecifics& specifics : remote_data) {
      initial_updates.push_back(SpecificsToUpdateResponse(specifics));
    }
    real_processor_->OnUpdateReceived(state, initial_updates);
  }

  std::map<std::string, std::unique_ptr<EntityData>> GetAllData() {
    base::RunLoop loop;
    std::unique_ptr<DataBatch> batch;
    bridge_->GetAllData(base::BindLambdaForTesting(
        [&loop, &batch](std::unique_ptr<DataBatch> input_batch) {
          batch = std::move(input_batch);
          loop.Quit();
        }));
    loop.Run();
    EXPECT_NE(nullptr, batch);
    return BatchToEntityDataMap(std::move(batch));
  }

  std::map<std::string, std::unique_ptr<EntityData>> GetData(
      const std::vector<std::string>& storage_keys) {
    base::RunLoop loop;
    std::unique_ptr<DataBatch> batch;
    bridge_->GetData(
        storage_keys,
        base::BindLambdaForTesting(
            [&loop, &batch](std::unique_ptr<DataBatch> input_batch) {
              batch = std::move(input_batch);
              loop.Quit();
            }));
    loop.Run();
    EXPECT_NE(nullptr, batch);
    return BatchToEntityDataMap(std::move(batch));
  }

  std::unique_ptr<EntityData> GetData(const std::string& storage_key) {
    std::map<std::string, std::unique_ptr<EntityData>> entity_data_map =
        GetData(std::vector<std::string>{storage_key});
    EXPECT_LE(entity_data_map.size(), 1U);
    if (entity_data_map.empty()) {
      return nullptr;
    }
    EXPECT_EQ(storage_key, entity_data_map.begin()->first);
    return std::move(entity_data_map.begin()->second);
  }

  void ResetWindows() { window_getter_.ResetWindows(); }

  TestSyncedWindowDelegate* AddWindow(
      int window_id,
      sync_pb::SessionWindow_BrowserType type =
          sync_pb::SessionWindow_BrowserType_TYPE_TABBED) {
    return window_getter_.AddWindow(type,
                                    SessionID::FromSerializedValue(window_id));
  }

  TestSyncedTabDelegate* AddTab(int window_id,
                                const std::string& url,
                                int tab_id = SessionID::NewUnique().id()) {
    TestSyncedTabDelegate* tab =
        window_getter_.AddTab(SessionID::FromSerializedValue(window_id),
                              SessionID::FromSerializedValue(tab_id));
    tab->Navigate(url, base::Time::Now());
    return tab;
  }

  SessionSyncBridge* bridge() { return bridge_.get(); }

  syncer::MockModelTypeChangeProcessor& mock_processor() {
    return mock_processor_;
  }

  syncer::ClientTagBasedModelTypeProcessor* real_processor() {
    return real_processor_.get();
  }

  base::MockCallback<base::RepeatingClosure>&
  mock_foreign_sessions_updated_callback() {
    return mock_foreign_sessions_updated_callback_;
  }

 private:
  base::MessageLoop message_loop_;
  const std::unique_ptr<syncer::ModelTypeStore> store_;

  // Dependencies.
  testing::NiceMock<MockSyncSessionsClient> mock_sync_sessions_client_;
  testing::NiceMock<MockSessionSyncPrefs> mock_sync_prefs_;
  syncer::LocalDeviceInfoProviderMock mock_device_info_provider_;
  testing::NiceMock<MockModelTypeChangeProcessor> mock_processor_;
  testing::NiceMock<base::MockCallback<base::RepeatingClosure>>
      mock_foreign_sessions_updated_callback_;
  TestSyncedWindowDelegatesGetter window_getter_;
  FaviconCache favicon_cache_;

  std::unique_ptr<SessionSyncBridge> bridge_;
  std::unique_ptr<syncer::ClientTagBasedModelTypeProcessor> real_processor_;
};

TEST_F(SessionSyncBridgeTest, ShouldCallDoModelReadyToSyncWhenSyncEnabled) {
  EXPECT_CALL(mock_processor(), DoModelReadyToSync(_, _)).Times(0);
  InitializeBridge();
  EXPECT_CALL(mock_processor(),
              DoModelReadyToSync(bridge(), IsEmptyMetadataBatch()));
  StartSyncing();
}

// Test that handling of local events (i.e. propagating the local state to
// sync) does not start while a session restore is in progress.
TEST_F(SessionSyncBridgeTest, ShouldDeferLocalEventDueToSessionRestore) {
  const int kWindowId = 1000001;
  const int kTabId1 = 1000002;
  const int kTabId2 = 1000003;

  // No notifications expected until OnSessionRestoreComplete().
  EXPECT_CALL(mock_processor(), DoPut(_, _, _)).Times(0);

  AddWindow(kWindowId)->SetIsSessionRestoreInProgress(true);
  // Initial tab should be ignored (not exposed to processor) while session
  // restore is in progress.
  AddTab(kWindowId, "http://foo.com/", kTabId1);

  InitializeBridge();
  StartSyncing();
  EXPECT_THAT(GetAllData(),
              ElementsAre(Pair(
                  _, EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag,
                                                          /*window_ids=*/{},
                                                          /*tab_ids=*/{})))));

  // Create the actual tab, which should be ignored because session restore
  // is in progress.
  AddTab(kWindowId, "http://bar.com/", kTabId2);
  EXPECT_THAT(GetAllData(), SizeIs(1));

  // OnSessionRestoreComplete() should issue three Put() calls, one updating the
  // header and one for each of the two added tabs.
  EXPECT_CALL(mock_processor(), DoPut(_, _, _)).Times(3);
  bridge()->OnSessionRestoreComplete();
  EXPECT_THAT(GetAllData(), SizeIs(3));
}

TEST_F(SessionSyncBridgeTest, ShouldCreateHeaderByDefault) {
  InitializeBridge();

  EXPECT_CALL(mock_processor(), DoModelReadyToSync(_, IsEmptyMetadataBatch()));
  StartSyncing();

  EXPECT_THAT(GetAllData(), SizeIs(1));
}

// Tests that local windows and tabs that exist at the time the bridge is
// started (e.g. after a Chrome restart) are properly exposed via the bridge's
// GetData() and GetAllData() methods, as well as notified via Put().
TEST_F(SessionSyncBridgeTest, ShouldExposeInitialLocalTabsToProcessor) {
  const int kWindowId = 1000001;
  const int kTabId1 = 1000002;
  const int kTabId2 = 1000003;

  AddWindow(kWindowId);
  AddTab(kWindowId, "http://foo.com/", kTabId1);
  AddTab(kWindowId, "http://bar.com/", kTabId2);

  InitializeBridge();

  const std::string header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  const std::string tab_storage_key1 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, 0);
  const std::string tab_storage_key2 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, 1);

  EXPECT_CALL(mock_processor(),
              DoPut(header_storage_key,
                    EntityDataHasSpecifics(MatchesHeader(
                        kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2})),
                    _));
  EXPECT_CALL(mock_processor(),
              DoPut(tab_storage_key1,
                    EntityDataHasSpecifics(
                        MatchesTab(kLocalSessionTag, kWindowId, kTabId1,
                                   /*tab_node_id=*/_, {"http://foo.com/"})),
                    _));
  EXPECT_CALL(mock_processor(),
              DoPut(tab_storage_key2,
                    EntityDataHasSpecifics(
                        MatchesTab(kLocalSessionTag, kWindowId, kTabId2,
                                   /*tab_node_id=*/_, {"http://bar.com/"})),
                    _));

  StartSyncing();

  EXPECT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(MatchesHeader(
                  kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2})));
  EXPECT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(
                   kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2}))),
          Pair(tab_storage_key1, EntityDataHasSpecifics(MatchesTab(
                                     kLocalSessionTag, kWindowId, kTabId1,
                                     /*tab_node_id=*/_, {"http://foo.com/"}))),
          Pair(tab_storage_key2,
               EntityDataHasSpecifics(
                   MatchesTab(kLocalSessionTag, kWindowId, kTabId2,
                              /*tab_node_id=*/_, {"http://bar.com/"})))));
}

// Tests that the creation of a new tab while sync is enabled is propagated to:
// 1) The processor, via Put().
// 2) The in-memory representation exposed via GetData().
// 3) The persisted store, exposed via GetAllData().
TEST_F(SessionSyncBridgeTest, ShouldReportLocalTabCreation) {
  const int kWindowId = 1000001;
  const int kTabId1 = 1000002;
  const int kTabId2 = 1000003;

  AddWindow(kWindowId);
  AddTab(kWindowId, "http://foo.com/", kTabId1);

  InitializeBridge();
  StartSyncing();

  ASSERT_THAT(GetAllData(), SizeIs(2));
  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run()).Times(0);

  // Expectations for the processor.
  std::string header_storage_key;
  std::string tab_storage_key;
  // Tab creation triggers an update event due to the tab parented notification,
  // so the event handler issues two commits as well (one for tab creation, one
  // for tab update). During the first update, however, the tab is not syncable
  // and is hence skipped.
  testing::Expectation put_transient_header = EXPECT_CALL(
      mock_processor(), DoPut(_,
                              EntityDataHasSpecifics(MatchesHeader(
                                  kLocalSessionTag, {kWindowId}, {kTabId1})),
                              _));
  EXPECT_CALL(mock_processor(),
              DoPut(_,
                    EntityDataHasSpecifics(MatchesHeader(
                        kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2})),
                    _))
      .After(put_transient_header)
      .WillOnce(SaveArg<0>(&header_storage_key));
  EXPECT_CALL(mock_processor(),
              DoPut(_,
                    EntityDataHasSpecifics(
                        MatchesTab(kLocalSessionTag, kWindowId, kTabId2,
                                   /*tab_node_id=*/_, {"http://bar.com/"})),
                    _))
      .WillOnce(SaveArg<0>(&tab_storage_key));

  // Create the actual tab, now that we're syncing.
  AddTab(kWindowId, "http://bar.com/", kTabId2);

  ASSERT_THAT(header_storage_key,
              Eq(SessionStore::GetHeaderStorageKey(kLocalSessionTag)));
  ASSERT_THAT(tab_storage_key, Not(IsEmpty()));

  // Verify the bridge's state exposed via the getters.
  EXPECT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(
                   kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2}))),
          Pair(_, EntityDataHasSpecifics(
                      MatchesTab(kLocalSessionTag, kWindowId, kTabId1,
                                 /*tab_node_id=*/_, {"http://foo.com/"}))),
          Pair(tab_storage_key, EntityDataHasSpecifics(MatchesTab(
                                    kLocalSessionTag, kWindowId, kTabId2,
                                    /*tab_node_id=*/_, {"http://bar.com/"})))));
  EXPECT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(MatchesHeader(
                  kLocalSessionTag, {kWindowId}, {kTabId1, kTabId2})));
  EXPECT_THAT(GetData(tab_storage_key),
              EntityDataHasSpecifics(
                  MatchesTab(kLocalSessionTag, kWindowId, kTabId2,
                             /*tab_node_id=*/_, {"http://bar.com/"})));
}

TEST_F(SessionSyncBridgeTest, ShouldUpdateIdsDuringRestore) {
  const int kWindowId = 1000001;
  const int kTabIdBeforeRestore1 = 1000002;
  const int kTabIdBeforeRestore2 = 1000003;
  const int kTabIdAfterRestore1 = 1000004;
  const int kTabIdAfterRestore2 = 1000005;
  // Zero is the first assigned tab node ID.
  const int kTabNodeId1 = 0;
  const int kTabNodeId2 = 1;

  AddWindow(kWindowId);
  AddTab(kWindowId, "http://foo.com/", kTabIdBeforeRestore1);
  AddTab(kWindowId, "http://bar.com/", kTabIdBeforeRestore2);

  const std::string header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  const std::string tab_storage_key1 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, kTabNodeId1);
  const std::string tab_storage_key2 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, kTabNodeId2);

  InitializeBridge();
  StartSyncing();

  ASSERT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(
                  MatchesHeader(kLocalSessionTag, {kWindowId},
                                {kTabIdBeforeRestore1, kTabIdBeforeRestore2})));
  ASSERT_THAT(GetData(tab_storage_key1),
              EntityDataHasSpecifics(
                  MatchesTab(kLocalSessionTag, kWindowId, kTabIdBeforeRestore1,
                             kTabNodeId1, {"http://foo.com/"})));
  ASSERT_THAT(GetData(tab_storage_key2),
              EntityDataHasSpecifics(
                  MatchesTab(kLocalSessionTag, kWindowId, kTabIdBeforeRestore2,
                             kTabNodeId2, {"http://bar.com/"})));

  ShutdownBridge();

  // Override tabs with placeholder tab delegates.
  ResetWindows();
  PlaceholderTabDelegate placeholder_tab1(
      SessionID::FromSerializedValue(kTabIdAfterRestore1), kTabNodeId1);
  PlaceholderTabDelegate placeholder_tab2(
      SessionID::FromSerializedValue(kTabIdAfterRestore2), kTabNodeId2);
  TestSyncedWindowDelegate* window = AddWindow(kWindowId);
  window->OverrideTabAt(0, &placeholder_tab1);
  window->OverrideTabAt(1, &placeholder_tab2);

  // When the bridge gets restarted, we expected tab IDs being updated, but the
  // rest of the information such as navigation URLs should be reused.
  EXPECT_CALL(mock_processor(),
              DoPut(header_storage_key,
                    EntityDataHasSpecifics(MatchesHeader(
                        kLocalSessionTag, {kWindowId},
                        {kTabIdAfterRestore1, kTabIdAfterRestore2})),
                    _));
  EXPECT_CALL(mock_processor(),
              DoPut(tab_storage_key1,
                    EntityDataHasSpecifics(MatchesTab(
                        kLocalSessionTag, kWindowId, kTabIdAfterRestore1,
                        kTabNodeId1, {"http://foo.com/"})),
                    _));
  EXPECT_CALL(mock_processor(),
              DoPut(tab_storage_key2,
                    EntityDataHasSpecifics(MatchesTab(
                        kLocalSessionTag, kWindowId, kTabIdAfterRestore2,
                        kTabNodeId2, {"http://bar.com/"})),
                    _));

  // Start the bridge again.
  InitializeBridge();
  StartSyncing();

  EXPECT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(
                  MatchesHeader(kLocalSessionTag, {kWindowId},
                                {kTabIdAfterRestore1, kTabIdAfterRestore2})));
  EXPECT_THAT(GetData(tab_storage_key1),
              EntityDataHasSpecifics(
                  MatchesTab(kLocalSessionTag, kWindowId, kTabIdAfterRestore1,
                             kTabNodeId1, {"http://foo.com/"})));
  EXPECT_THAT(GetData(tab_storage_key2),
              EntityDataHasSpecifics(
                  MatchesTab(kLocalSessionTag, kWindowId, kTabIdAfterRestore2,
                             kTabNodeId2, {"http://bar.com/"})));

  EXPECT_THAT(GetAllData(),
              UnorderedElementsAre(
                  Pair(header_storage_key,
                       EntityDataHasSpecifics(MatchesHeader(
                           kLocalSessionTag, {kWindowId},
                           {kTabIdAfterRestore1, kTabIdAfterRestore2}))),
                  Pair(tab_storage_key1,
                       EntityDataHasSpecifics(MatchesTab(
                           kLocalSessionTag, kWindowId, kTabIdAfterRestore1,
                           kTabNodeId1, {"http://foo.com/"}))),
                  Pair(tab_storage_key2,
                       EntityDataHasSpecifics(MatchesTab(
                           kLocalSessionTag, kWindowId, kTabIdAfterRestore2,
                           kTabNodeId2, {"http://bar.com/"})))));
}

// Ensure that tabbed windows from a previous session are preserved if no
// windows are present on startup.
TEST_F(SessionSyncBridgeTest, ShouldRestoreTabbedDataIfNoWindowsDuringStartup) {
  const int kWindowId = 1000001;
  const int kTabNodeId = 0;

  AddWindow(kWindowId);
  TestSyncedTabDelegate* tab = AddTab(kWindowId, "http://foo.com/");

  const std::string header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  const std::string tab_storage_key =
      SessionStore::GetTabStorageKey(kLocalSessionTag, kTabNodeId);

  InitializeBridge();
  StartSyncing();

  ASSERT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _))),
          Pair(tab_storage_key,
               EntityDataHasSpecifics(MatchesTab(
                   kLocalSessionTag, _, _, kTabNodeId, {"http://foo.com/"})))));

  ShutdownBridge();

  // Start the bridge with no local windows/tabs.
  ResetWindows();
  InitializeBridge();
  StartSyncing();

  EXPECT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _))),
          Pair(tab_storage_key,
               EntityDataHasSpecifics(MatchesTab(
                   kLocalSessionTag, _, _, kTabNodeId, {"http://foo.com/"})))));

  // Now actually resurrect the native data, which will end up having different
  // native ids, but the tab has the same sync id as before.
  EXPECT_CALL(
      mock_processor(),
      DoPut(header_storage_key,
            EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _)), _));
  EXPECT_CALL(mock_processor(),
              DoPut(tab_storage_key,
                    EntityDataHasSpecifics(MatchesTab(
                        kLocalSessionTag, /*window_id=*/_, /*tab_id=*/_,
                        kTabNodeId, {"http://foo.com/", "http://bar.com/"})),
                    _));
  AddWindow(kWindowId)->OverrideTabAt(0, tab);
  tab->Navigate("http://bar.com/");
}

TEST_F(SessionSyncBridgeTest, ShouldDisableSyncAndReenable) {
  const int kWindowId = 1000001;
  const int kTabId = 1000002;

  AddWindow(kWindowId);
  AddTab(kWindowId, "http://foo.com/", kTabId);

  InitializeBridge();
  StartSyncing();

  const std::string header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  ASSERT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(
                  MatchesHeader(kLocalSessionTag, {kWindowId}, {kTabId})));
  ASSERT_THAT(GetAllData(), Not(IsEmpty()));

  EXPECT_CALL(mock_processor(), DoModelReadyToSync(_, _)).Times(0);
  real_processor()->DisableSync();

  EXPECT_CALL(mock_processor(),
              DoModelReadyToSync(bridge(), IsEmptyMetadataBatch()));
  StartSyncing();
  ASSERT_THAT(GetData(header_storage_key),
              EntityDataHasSpecifics(
                  MatchesHeader(kLocalSessionTag, {kWindowId}, {kTabId})));
}

// Starting sync with no local data should just store the foreign entities in
// the store and expose them via OpenTabsUIDelegate.
TEST_F(SessionSyncBridgeTest, ShouldMergeForeignSession) {
  const std::string kForeignSessionTag = "foreignsessiontag";
  const int kForeignWindowId = 2000001;
  const int kForeignTabId = 2000002;
  const int kForeignTabNodeId = 2003;

  EXPECT_CALL(mock_processor(), UpdateStorageKey(_, _, _)).Times(0);
  EXPECT_CALL(mock_processor(), DoPut(_, _, _)).Times(0);
  InitializeBridge();

  const sync_pb::SessionSpecifics foreign_header =
      CreateHeaderSpecificsWithOneTab(kForeignSessionTag, kForeignWindowId,
                                      kForeignTabId);
  const sync_pb::SessionSpecifics foreign_tab =
      CreateTabSpecifics(kForeignSessionTag, kForeignWindowId, kForeignTabId,
                         kForeignTabNodeId, "http://baz.com/");

  EXPECT_CALL(
      mock_processor(),
      DoPut(_, EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _)),
            _));
  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run());

  StartSyncing({foreign_header, foreign_tab});

  std::vector<const SyncedSession*> foreign_sessions;
  EXPECT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetAllForeignSessions(
      &foreign_sessions));
  EXPECT_THAT(foreign_sessions,
              ElementsAre(MatchesSyncedSession(
                  kForeignSessionTag,
                  {{kForeignWindowId, std::vector<int>{kForeignTabId}}})));
}

// Regression test for crbug.com/837517: Ensure that the bridge doesn't crash
// and closed foreign tabs (|kForeignTabId2| in the test) are not exposed after
// restarting the browser.
TEST_F(SessionSyncBridgeTest, ShouldNotExposeClosedTabsAfterRestart) {
  const std::string kForeignSessionTag = "foreignsessiontag";
  const int kForeignWindowId = 2000001;
  const int kForeignTabId1 = 2000002;
  const int kForeignTabId2 = 2000003;
  const int kForeignTabNodeId1 = 2004;
  const int kForeignTabNodeId2 = 2005;

  // The header only lists a single tab |kForeignTabId1|, which becomes a mapped
  // tab.
  const sync_pb::SessionSpecifics foreign_header =
      CreateHeaderSpecificsWithOneTab(kForeignSessionTag, kForeignWindowId,
                                      kForeignTabId1);
  const sync_pb::SessionSpecifics foreign_tab1 =
      CreateTabSpecifics(kForeignSessionTag, kForeignWindowId, kForeignTabId1,
                         kForeignTabNodeId1, "http://foo.com/");
  // |kForeignTabId2| is not present in the header, leading to an unmapped tab.
  const sync_pb::SessionSpecifics foreign_tab2 =
      CreateTabSpecifics(kForeignSessionTag, kForeignWindowId, kForeignTabId2,
                         kForeignTabNodeId2, "http://bar.com/");

  InitializeBridge();
  StartSyncing({foreign_header, foreign_tab1, foreign_tab2});

  const std::string local_header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  const std::string foreign_header_storage_key =
      SessionStore::GetHeaderStorageKey(kForeignSessionTag);
  const std::string foreign_tab_storage_key1 =
      SessionStore::GetTabStorageKey(kForeignSessionTag, kForeignTabNodeId1);
  const std::string foreign_tab_storage_key2 =
      SessionStore::GetTabStorageKey(kForeignSessionTag, kForeignTabNodeId2);

  ASSERT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(local_header_storage_key, _),
          Pair(foreign_header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(
                   kForeignSessionTag, {kForeignWindowId}, {kForeignTabId1}))),
          Pair(foreign_tab_storage_key1,
               EntityDataHasSpecifics(MatchesTab(
                   kForeignSessionTag, kForeignWindowId, kForeignTabId1,
                   kForeignTabNodeId1, {"http://foo.com/"}))),
          Pair(foreign_tab_storage_key2,
               EntityDataHasSpecifics(MatchesTab(
                   kForeignSessionTag, kForeignWindowId, kForeignTabId2,
                   kForeignTabNodeId2, {"http://bar.com/"})))));

  // Mimic a browser restart, which should restore the very same state (and not
  // crash!).
  ShutdownBridge();
  InitializeBridge();
  StartSyncing();

  EXPECT_THAT(GetAllData(),
              UnorderedElementsAre(Pair(local_header_storage_key, _),
                                   Pair(foreign_header_storage_key, _),
                                   Pair(foreign_tab_storage_key1, _),
                                   Pair(foreign_tab_storage_key2, _)));
}

TEST_F(SessionSyncBridgeTest, ShouldHandleRemoteDeletion) {
  const std::string kForeignSessionTag = "foreignsessiontag";
  const int kForeignWindowId = 2000001;
  const int kForeignTabId = 2000002;
  const int kForeignTabNodeId = 2003;

  InitializeBridge();

  const sync_pb::SessionSpecifics foreign_header =
      CreateHeaderSpecificsWithOneTab(kForeignSessionTag, kForeignWindowId,
                                      kForeignTabId);
  const sync_pb::SessionSpecifics foreign_tab =
      CreateTabSpecifics(kForeignSessionTag, kForeignWindowId, kForeignTabId,
                         kForeignTabNodeId, "http://baz.com/");
  StartSyncing({foreign_header, foreign_tab});

  const sessions::SessionTab* foreign_session_tab = nullptr;
  ASSERT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetForeignTab(
      kForeignSessionTag, SessionID::FromSerializedValue(kForeignTabId),
      &foreign_session_tab));
  ASSERT_THAT(foreign_session_tab, NotNull());
  std::vector<const SyncedSession*> foreign_sessions;
  ASSERT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetAllForeignSessions(
      &foreign_sessions));
  ASSERT_THAT(foreign_sessions,
              ElementsAre(MatchesSyncedSession(
                  kForeignSessionTag,
                  {{kForeignWindowId, std::vector<int>{kForeignTabId}}})));
  ASSERT_TRUE(real_processor()->IsTrackingMetadata());

  // Mimic receiving a remote deletion of the foreign session.
  sync_pb::ModelTypeState state;
  state.set_initial_sync_done(true);

  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run());
  real_processor()->OnUpdateReceived(
      state, {CreateTombstone(SessionStore::GetClientTag(foreign_header))});

  foreign_session_tab = nullptr;
  EXPECT_FALSE(bridge()->GetOpenTabsUIDelegate()->GetForeignTab(
      kForeignSessionTag, SessionID::FromSerializedValue(kForeignTabId),
      &foreign_session_tab));

  EXPECT_FALSE(bridge()->GetOpenTabsUIDelegate()->GetAllForeignSessions(
      &foreign_sessions));
}

TEST_F(SessionSyncBridgeTest, ShouldIgnoreRemoteDeletionOfLocalTab) {
  const int kWindowId1 = 1000001;
  const int kTabId1 = 1000002;
  const int kTabNodeId1 = 0;

  AddWindow(kWindowId1);
  AddTab(kWindowId1, "http://foo.com/", kTabId1);

  InitializeBridge();
  StartSyncing();

  const std::string header_storage_key =
      SessionStore::GetHeaderStorageKey(kLocalSessionTag);
  const std::string tab_storage_key1 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, kTabNodeId1);
  const std::string tab_client_tag1 =
      SessionStore::GetTabClientTagForTest(kLocalSessionTag, kTabNodeId1);

  ASSERT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _))),
          Pair(tab_storage_key1, EntityDataHasSpecifics(MatchesTab(
                                     kLocalSessionTag, kWindowId1, kTabId1,
                                     kTabNodeId1, {"http://foo.com/"})))));
  ASSERT_TRUE(real_processor()->IsTrackingMetadata());
  ASSERT_TRUE(real_processor()->HasLocalChangesForTest());

  // Mimic receiving a commit ack for both the tab and the header entity,
  // because otherwise it will be treated as conflict, and then local wins.
  sync_pb::ModelTypeState state;
  state.set_initial_sync_done(true);
  real_processor()->OnCommitCompleted(
      state, {CreateSuccessResponse(tab_client_tag1),
              CreateSuccessResponse(kLocalSessionTag)});
  ASSERT_FALSE(real_processor()->HasLocalChangesForTest());

  // Mimic receiving a remote deletion of both entities.
  EXPECT_CALL(mock_processor(), DoPut(_, _, _)).Times(0);
  real_processor()->OnUpdateReceived(state, {CreateTombstone(kLocalSessionTag),
                                             CreateTombstone(tab_client_tag1)});

  // State should remain unchanged (deletions ignored).
  EXPECT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(kLocalSessionTag, _, _))),
          Pair(tab_storage_key1, EntityDataHasSpecifics(MatchesTab(
                                     kLocalSessionTag, kWindowId1, kTabId1,
                                     kTabNodeId1, {"http://foo.com/"})))));

  // Creating a new tab locally should trigger Put() calls for *all* entities
  // (because the local data was out of sync).
  const int kWindowId2 = 2000001;
  const int kTabId2 = 2000002;
  const int kTabNodeId2 = 1;

  const std::string tab_storage_key2 =
      SessionStore::GetTabStorageKey(kLocalSessionTag, kTabNodeId2);

  // Window creation already triggers a header update, which will be overriden
  // later below.
  testing::Expectation put_transient_header =
      EXPECT_CALL(mock_processor(), DoPut(header_storage_key, _, _));
  AddWindow(kWindowId2);

  // In the current implementation, some of the updates are reported to the
  // processor twice, but that's OK because the processor can detect it.
  EXPECT_CALL(mock_processor(),
              DoPut(header_storage_key,
                    EntityDataHasSpecifics(MatchesHeader(
                        kLocalSessionTag, ElementsAre(kWindowId1, kWindowId2),
                        ElementsAre(kTabId1, kTabId2))),
                    _))
      .Times(2)
      .After(put_transient_header);
  EXPECT_CALL(mock_processor(), DoPut(tab_storage_key1,
                                      EntityDataHasSpecifics(MatchesTab(
                                          kLocalSessionTag, kWindowId1, kTabId1,
                                          kTabNodeId1, {"http://foo.com/"})),
                                      _));
  EXPECT_CALL(mock_processor(), DoPut(tab_storage_key2,
                                      EntityDataHasSpecifics(MatchesTab(
                                          kLocalSessionTag, kWindowId2, kTabId2,
                                          kTabNodeId2, {"http://bar.com/"})),
                                      _))
      .Times(2);

  AddTab(kWindowId2, "http://bar.com/", kTabId2);

  EXPECT_THAT(
      GetAllData(),
      UnorderedElementsAre(
          Pair(header_storage_key,
               EntityDataHasSpecifics(MatchesHeader(
                   kLocalSessionTag, ElementsAre(kWindowId1, kWindowId2),
                   ElementsAre(kTabId1, kTabId2)))),
          Pair(tab_storage_key1,
               EntityDataHasSpecifics(
                   MatchesTab(kLocalSessionTag, /*window_id=*/_, /*tab_id=*/_,
                              kTabNodeId1, {"http://foo.com/"}))),
          Pair(tab_storage_key2, EntityDataHasSpecifics(MatchesTab(
                                     kLocalSessionTag, kWindowId2, kTabId2,
                                     kTabNodeId2, {"http://bar.com/"})))));

  // Run until idle because PostTask() is used to invoke ResubmitLocalSession().
  base::RunLoop().RunUntilIdle();
}

// Verifies that a foreign session can be deleted by the user from the history
// UI (via OpenTabsUIDelegate).
TEST_F(SessionSyncBridgeTest, ShouldDeleteForeignSessionFromUI) {
  const std::string kForeignSessionTag = "foreignsessiontag";
  const int kForeignWindowId = 2000001;
  const int kForeignTabId = 2000002;
  const int kForeignTabNodeId = 2003;

  InitializeBridge();

  const sync_pb::SessionSpecifics foreign_header =
      CreateHeaderSpecificsWithOneTab(kForeignSessionTag, kForeignWindowId,
                                      kForeignTabId);
  const sync_pb::SessionSpecifics foreign_tab =
      CreateTabSpecifics(kForeignSessionTag, kForeignWindowId, kForeignTabId,
                         kForeignTabNodeId, "http://baz.com/");
  StartSyncing({foreign_header, foreign_tab});

  const std::string foreign_header_storage_key =
      SessionStore::GetHeaderStorageKey(kForeignSessionTag);
  const std::string foreign_tab_storage_key =
      SessionStore::GetTabStorageKey(kForeignSessionTag, kForeignTabNodeId);

  // Test fixture expects the two foreign entities in the model as well as the
  // underlying store.
  ASSERT_THAT(GetData(foreign_header_storage_key), NotNull());
  ASSERT_THAT(GetData(foreign_tab_storage_key), NotNull());

  const sessions::SessionTab* foreign_session_tab = nullptr;
  ASSERT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetForeignTab(
      kForeignSessionTag, SessionID::FromSerializedValue(kForeignTabId),
      &foreign_session_tab));
  ASSERT_THAT(foreign_session_tab, NotNull());
  std::vector<const SyncedSession*> foreign_sessions;
  ASSERT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetAllForeignSessions(
      &foreign_sessions));
  ASSERT_THAT(foreign_sessions,
              ElementsAre(MatchesSyncedSession(
                  kForeignSessionTag,
                  {{kForeignWindowId, std::vector<int>{kForeignTabId}}})));
  ASSERT_TRUE(real_processor()->IsTrackingMetadata());

  // Mimic the user requesting a session deletion from the UI.
  EXPECT_CALL(mock_processor(), Delete(foreign_header_storage_key, _));
  EXPECT_CALL(mock_processor(), Delete(foreign_tab_storage_key, _));
  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run());
  bridge()->GetOpenTabsUIDelegate()->DeleteForeignSession(kForeignSessionTag);

  // Verify what gets exposed to the UI.
  foreign_session_tab = nullptr;
  EXPECT_FALSE(bridge()->GetOpenTabsUIDelegate()->GetForeignTab(
      kForeignSessionTag, SessionID::FromSerializedValue(kForeignTabId),
      &foreign_session_tab));
  EXPECT_FALSE(bridge()->GetOpenTabsUIDelegate()->GetAllForeignSessions(
      &foreign_sessions));

  // Verify store.
  EXPECT_THAT(GetData(foreign_header_storage_key), IsNull());
  EXPECT_THAT(GetData(foreign_tab_storage_key), IsNull());
}

// Verifies that attempts to delete the local session from the UI are ignored,
// although the UI sholdn't really be offering that option.
TEST_F(SessionSyncBridgeTest, ShouldIgnoreLocalSessionDeletionFromUI) {
  InitializeBridge();
  StartSyncing();

  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run()).Times(0);
  EXPECT_CALL(mock_processor(), Delete(_, _)).Times(0);

  bridge()->GetOpenTabsUIDelegate()->DeleteForeignSession(kLocalSessionTag);

  const SyncedSession* session = nullptr;
  EXPECT_TRUE(bridge()->GetOpenTabsUIDelegate()->GetLocalSession(&session));
  EXPECT_THAT(session, NotNull());
  EXPECT_THAT(GetData(SessionStore::GetHeaderStorageKey(kLocalSessionTag)),
              NotNull());
}

TEST_F(SessionSyncBridgeTest, ShouldDoGarbageCollection) {
  // We construct two identical sessions, one modified recently, one modified
  // more than |kStaleSessionThreshold| ago (14 days ago).
  const base::Time stale_mtime =
      base::Time::Now() - base::TimeDelta::FromDays(15);
  const base::Time recent_mtime =
      base::Time::Now() - base::TimeDelta::FromDays(13);
  const std::string kStaleSessionTag = "stalesessiontag";
  const std::string kRecentSessionTag = "recentsessiontag";
  const int kWindowId = 2000001;
  const int kTabId = 2000002;
  const int kTabNodeId = 2003;

  InitializeBridge();
  StartSyncing();

  // Construct a remote update.
  sync_pb::ModelTypeState state;
  state.set_initial_sync_done(true);
  syncer::UpdateResponseDataList updates;
  // Two entities belong to a recent session.
  updates.push_back(SpecificsToUpdateResponse(
      CreateHeaderSpecificsWithOneTab(kStaleSessionTag, kWindowId, kTabId),
      stale_mtime));
  updates.push_back(SpecificsToUpdateResponse(
      CreateTabSpecifics(kStaleSessionTag, kWindowId, kTabId, kTabNodeId,
                         "http://baz.com/"),
      stale_mtime));
  updates.push_back(SpecificsToUpdateResponse(
      CreateHeaderSpecificsWithOneTab(kRecentSessionTag, kWindowId, kTabId),
      recent_mtime));
  updates.push_back(SpecificsToUpdateResponse(
      CreateTabSpecifics(kRecentSessionTag, kWindowId, kTabId, kTabNodeId,
                         "http://baz.com/"),
      recent_mtime));
  real_processor()->OnUpdateReceived(state, updates);

  // During garbage collection, we expect |kStaleSessionTag| to be deleted.
  EXPECT_CALL(mock_processor(),
              Delete(SessionStore::GetHeaderStorageKey(kStaleSessionTag), _));
  EXPECT_CALL(
      mock_processor(),
      Delete(SessionStore::GetTabStorageKey(kStaleSessionTag, kTabNodeId), _));
  EXPECT_CALL(mock_foreign_sessions_updated_callback(), Run());

  bridge()->ScheduleGarbageCollection();
  base::RunLoop().RunUntilIdle();
}

}  // namespace
}  // namespace sync_sessions
