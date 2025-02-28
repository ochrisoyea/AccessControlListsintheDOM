// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CONTENT_TEST_LEVELDB_WRAPPER_TEST_UTIL_H_
#define CONTENT_TEST_LEVELDB_WRAPPER_TEST_UTIL_H_

#include <stdint.h>
#include <vector>

#include "base/callback.h"
#include "base/optional.h"
#include "components/services/leveldb/public/interfaces/leveldb.mojom.h"
#include "content/browser/leveldb_wrapper_impl.h"
#include "content/common/leveldb_wrapper.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"

// Utility functions and classes for testing LevelDBWrapper implementations.

namespace content {
namespace test {

// Creates a callback that sets the given |success_out| to the boolean argument,
// and then calls |callback|.
base::OnceCallback<void(bool)> MakeSuccessCallback(base::OnceClosure callback,
                                                   bool* success_out);

// Does a |Put| call on the given |wrapper| and waits until the response is
// received. Returns if the call was successful.
bool PutSync(mojom::LevelDBWrapper* wrapper,
             const std::vector<uint8_t>& key,
             const std::vector<uint8_t>& value,
             const base::Optional<std::vector<uint8_t>>& old_value,
             const std::string& source);

// This only accepts the implementation, as calling the mojo interface is a
// synchronous call which freezes the thread. If the leveldb implementation
// is asynchrounous, this requires 3 threads.
leveldb::mojom::DatabaseError GetAllSync(
    content::LevelDBWrapperImpl* wrapper,
    std::vector<mojom::KeyValuePtr>* data_out);

// Does a |Delete| call on the wrapper and waits until the response is
// received. Returns if the call was successful.
bool DeleteSync(mojom::LevelDBWrapper* wrapper,
                const std::vector<uint8_t>& key,
                const base::Optional<std::vector<uint8_t>>& client_old_value,
                const std::string& source);

// Does a |DeleteAll| call on the wrapper and waits until the response is
// received. Returns if the call was successful.
bool DeleteAllSync(mojom::LevelDBWrapper* wrapper, const std::string& source);

// Creates a callback that simply sets the  |*_out| variables to the arguments.
base::OnceCallback<void(leveldb::mojom::DatabaseError,
                        std::vector<mojom::KeyValuePtr>)>
MakeGetAllCallback(leveldb::mojom::DatabaseError* status_out,
                   std::vector<mojom::KeyValuePtr>* data_out);

// Utility class to help using the LevelDBWrapper::GetAll method. Use
// |CreateAndBind| to create the PtrInfo to send to the |GetAll| method.
// When the call is complete, the |callback| will be called.
class GetAllCallback : public mojom::LevelDBWrapperGetAllCallback {
 public:
  static mojom::LevelDBWrapperGetAllCallbackAssociatedPtrInfo CreateAndBind(
      bool* result,
      base::OnceClosure callback);

  ~GetAllCallback() override;

 private:
  GetAllCallback(bool* result, base::OnceClosure callback);

  void Complete(bool success) override;

  bool* result_;
  base::OnceClosure callback_;
};

// Mock observer implementation for use with LevelDBWrapper.
class MockLevelDBObserver : public content::mojom::LevelDBObserver {
 public:
  MockLevelDBObserver();
  ~MockLevelDBObserver() override;

  MOCK_METHOD3(KeyAdded,
               void(const std::vector<uint8_t>& key,
                    const std::vector<uint8_t>& value,
                    const std::string& source));
  MOCK_METHOD4(KeyChanged,
               void(const std::vector<uint8_t>& key,
                    const std::vector<uint8_t>& new_value,
                    const std::vector<uint8_t>& old_value,
                    const std::string& source));
  MOCK_METHOD3(KeyDeleted,
               void(const std::vector<uint8_t>& key,
                    const std::vector<uint8_t>& old_value,
                    const std::string& source));
  MOCK_METHOD1(AllDeleted, void(const std::string& source));
  MOCK_METHOD1(ShouldSendOldValueOnMutations, void(bool value));
};

}  // namespace test
}  // namespace content

#endif  // CONTENT_TEST_LEVELDB_WRAPPER_TEST_UTIL_H_
