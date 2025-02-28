// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/resource_coordinator/leveldb_site_characteristics_database.h"

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/resource_coordinator/site_characteristics.pb.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace resource_coordinator {

namespace {

const char kOrigin1[] = "foo.com";

// Initialize a SiteCharacteristicsProto object with a test value (the same
// value is used to initialize all fields).
void InitSiteCharacteristicProto(SiteCharacteristicsProto* proto,
                                 ::google::protobuf::int64 test_value) {
  proto->set_last_loaded(test_value);

  SiteCharacteristicsFeatureProto feature_proto;
  feature_proto.set_observation_duration(test_value);
  feature_proto.set_use_timestamp(test_value);

  proto->mutable_updates_favicon_in_background()->CopyFrom(feature_proto);
  proto->mutable_updates_title_in_background()->CopyFrom(feature_proto);
  proto->mutable_uses_notifications_in_background()->CopyFrom(feature_proto);
  proto->mutable_uses_audio_in_background()->CopyFrom(feature_proto);
}

}  // namespace

class LevelDBSiteCharacteristicsDatabaseTest : public ::testing::Test {
 public:
  LevelDBSiteCharacteristicsDatabaseTest() {}

  void SetUp() override {
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
    db_ = LevelDBSiteCharacteristicsDatabase::OpenOrCreateDatabase(
        temp_dir_.GetPath());
    EXPECT_TRUE(db_);
  }

  void TearDown() override {
    db_.reset();
    EXPECT_TRUE(temp_dir_.Delete());
  }

 protected:
  // Try to read an entry from the database, returns true if the entry is
  // present and false otherwise. |receiving_proto| will receive the protobuf
  // corresponding to this entry on success.
  bool ReadFromDB(const std::string& origin,
                  SiteCharacteristicsProto* receiving_proto) {
    EXPECT_TRUE(receiving_proto);
    bool success = false;
    bool init_called = false;
    auto init_callback = base::BindOnce(
        [](SiteCharacteristicsProto* proto, bool* res, bool* init_called,
           base::Optional<SiteCharacteristicsProto> proto_opt) {
          *res = proto_opt.has_value();
          *init_called = true;
          if (proto_opt)
            proto->CopyFrom(proto_opt.value());
        },
        base::Unretained(receiving_proto), base::Unretained(&success),
        base::Unretained(&init_called));
    db_->ReadSiteCharacteristicsFromDB(origin, std::move(init_callback));
    EXPECT_TRUE(init_called);
    return success;
  }

  base::ScopedTempDir temp_dir_;
  std::unique_ptr<LevelDBSiteCharacteristicsDatabase> db_;
};

TEST_F(LevelDBSiteCharacteristicsDatabaseTest, InitAndStoreSiteCharacteristic) {
  // Initializing an entry that doesn't exist in the database should fail.
  SiteCharacteristicsProto early_read_proto;
  EXPECT_FALSE(ReadFromDB(kOrigin1, &early_read_proto));

  // Add an entry to the database and make sure that we can read it back.
  ::google::protobuf::int64 test_value = 42;
  SiteCharacteristicsProto stored_proto;
  InitSiteCharacteristicProto(&stored_proto, test_value);
  db_->WriteSiteCharacteristicsIntoDB(kOrigin1, stored_proto);
  SiteCharacteristicsProto read_proto;
  EXPECT_TRUE(ReadFromDB(kOrigin1, &read_proto));
  EXPECT_TRUE(read_proto.IsInitialized());
  EXPECT_EQ(stored_proto.SerializeAsString(), read_proto.SerializeAsString());
}

TEST_F(LevelDBSiteCharacteristicsDatabaseTest, RemoveEntries) {
  // Add multiple origins to the database.
  const size_t kEntryCount = 10;
  SiteCharacteristicsProto protos[kEntryCount];
  std::vector<std::string> site_origins;
  for (size_t i = 0; i < kEntryCount; ++i) {
    std::string site_origin = base::StringPrintf("%zu.com", i);
    InitSiteCharacteristicProto(&protos[i],
                                static_cast<::google::protobuf::int64>(i));
    db_->WriteSiteCharacteristicsIntoDB(site_origin, protos[i]);
    site_origins.emplace_back(site_origin);
  }

  for (size_t i = 0; i < kEntryCount; ++i)
    EXPECT_TRUE(protos[i].IsInitialized());

  // Remove half the origins from the database.
  std::vector<std::string> site_origins_to_remove(
      site_origins.begin(), site_origins.begin() + kEntryCount / 2);
  db_->RemoveSiteCharacteristicsFromDB(site_origins_to_remove);

  // Verify that the origins were removed correctly.
  SiteCharacteristicsProto proto_temp;
  for (const auto& iter : site_origins_to_remove)
    EXPECT_FALSE(ReadFromDB(iter, &proto_temp));

  for (auto iter = site_origins.begin() + kEntryCount / 2;
       iter != site_origins.end(); ++iter) {
    EXPECT_TRUE(ReadFromDB(*iter, &proto_temp));
  }

  // Clear the database.
  db_->ClearDatabase();

  // Verify that no origin remains.
  for (auto iter : site_origins)
    EXPECT_FALSE(ReadFromDB(iter, &proto_temp));
}

}  // namespace resource_coordinator
