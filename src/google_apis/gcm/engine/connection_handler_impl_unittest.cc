// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "google_apis/gcm/engine/connection_handler_impl.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/test_timeouts.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/wire_format_lite.h"
#include "google_apis/gcm/base/mcs_util.h"
#include "google_apis/gcm/base/socket_stream.h"
#include "google_apis/gcm/protocol/mcs.pb.h"
#include "net/base/ip_address.h"
#include "net/log/net_log_source.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/stream_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace gcm {
namespace {

typedef std::unique_ptr<google::protobuf::MessageLite> ScopedMessage;
typedef std::vector<net::MockRead> ReadList;
typedef std::vector<net::MockWrite> WriteList;

const uint64_t kAuthId = 54321;
const uint64_t kAuthToken = 12345;
const char kMCSVersion = 41;  // The protocol version.
const int kMCSPort = 5228;    // The server port.
const char kDataMsgFrom[] = "data_from";
const char kDataMsgCategory[] = "data_category";
const char kDataMsgFrom2[] = "data_from2";
const char kDataMsgCategory2[] = "data_category2";
const char kDataMsgFromLong[] =
    "this is a long from that will result in a message > 128 bytes";
const char kDataMsgCategoryLong[] =
    "this is a long category that will result in a message > 128 bytes";
const char kDataMsgFromLong2[] =
    "this is a second long from that will result in a message > 128 bytes";
const char kDataMsgCategoryLong2[] =
    "this is a second long category that will result in a message > 128 bytes";
const uint8_t kInvalidTag = 100;  // An invalid tag.

// ---- Helpers for building messages. ----

// Encode a protobuf packet with protobuf type |tag| and serialized protobuf
// bytes |proto| into the MCS message form (tag + varint size + bytes).
std::string EncodePacket(uint8_t tag, const std::string& proto) {
  std::string result;
  google::protobuf::io::StringOutputStream string_output_stream(&result);
  {
    google::protobuf::io::CodedOutputStream coded_output_stream(
      &string_output_stream);
    const unsigned char tag_byte[1] = { tag };
    coded_output_stream.WriteRaw(tag_byte, 1);
    coded_output_stream.WriteVarint32(proto.size());
    coded_output_stream.WriteRaw(proto.c_str(), proto.size());
    // ~CodedOutputStream must run before the move constructor at the
    // return statement. http://crbug.com/338962
  }
  return result;
}

// Encode a handshake request into the MCS message form.
std::string EncodeHandshakeRequest() {
  std::string result;
  const char version_byte[1] = {kMCSVersion};
  result.append(version_byte, 1);
  ScopedMessage login_request(
      BuildLoginRequest(kAuthId, kAuthToken, ""));
  result.append(EncodePacket(kLoginRequestTag,
                             login_request->SerializeAsString()));
  return result;
}

// Build a serialized login response protobuf.
std::string BuildLoginResponse() {
  std::string result;
  mcs_proto::LoginResponse login_response;
  login_response.set_id("id");
  result.append(login_response.SerializeAsString());
  return result;
}

// Encoode a handshake response into the MCS message form.
std::string EncodeHandshakeResponse() {
  std::string result;
  const char version_byte[1] = {kMCSVersion};
  result.append(version_byte, 1);
  result.append(EncodePacket(kLoginResponseTag, BuildLoginResponse()));
  return result;
}

// Build a serialized data message stanza protobuf.
std::string BuildDataMessage(const std::string& from,
                             const std::string& category) {
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(from);
  data_message.set_category(category);
  return data_message.SerializeAsString();
}

// Build a corrupt data message that will force the protobuf parser to backup
// after completion (useful in testing memory corruption cases due to a
// CodedInputStream going out of scope).
std::string BuildCorruptDataMessage() {
  // Manually construct the message with invalid data. We set field 2 (id) to
  // be an invalid string.
  const int kMsgTag =
      (2 << google::protobuf::internal::WireFormatLite::kTagTypeBits) |
      google::protobuf::internal::WireFormatLite::WIRETYPE_LENGTH_DELIMITED;
  const int kStringLength = -1;  // Corrupted length.
  const char kStringData[] = "id";
  std::string data_message_proto;
  google::protobuf::io::StringOutputStream string_output_stream(
      &data_message_proto);
  {
    google::protobuf::io::CodedOutputStream coded_output_stream(
        &string_output_stream);
    coded_output_stream.WriteVarint32(kMsgTag);
    coded_output_stream.WriteVarint32(
        static_cast<google::protobuf::uint32>(kStringLength));
    coded_output_stream.WriteRaw(&kStringData, sizeof(kStringData));
    // ~CodedOutputStream must run before the move constructor at the
    // return statement. http://crbug.com/338962
  }

  return data_message_proto;
}

class GCMConnectionHandlerImplTest : public testing::Test {
 public:
  GCMConnectionHandlerImplTest();
  ~GCMConnectionHandlerImplTest() override;

  net::StreamSocket* BuildSocket(const ReadList& read_list,
                                 const WriteList& write_list);

  // Pump |message_loop_|, resetting |run_loop_| after completion.
  void PumpLoop();

  ConnectionHandlerImpl* connection_handler() {
    return connection_handler_.get();
  }
  base::MessageLoop* message_loop() { return &message_loop_; }
  net::StaticSocketDataProvider* data_provider() {
    return data_provider_.get();
  }
  int last_error() const { return last_error_; }

  // Initialize the connection handler, setting |dst_proto| as the destination
  // for any received messages.
  void Connect(ScopedMessage* dst_proto);

  // Runs the message loop until a message is received.
  void WaitForMessage();

 private:
  void ReadContinuation(ScopedMessage* dst_proto, ScopedMessage new_proto);
  void WriteContinuation();
  void ConnectionContinuation(int error);

  // SocketStreams and their data provider.
  ReadList mock_reads_;
  WriteList mock_writes_;
  std::unique_ptr<net::StaticSocketDataProvider> data_provider_;

  // The connection handler being tested.
  std::unique_ptr<ConnectionHandlerImpl> connection_handler_;

  // The last connection error received.
  int last_error_;

  // net:: components.
  std::unique_ptr<net::StreamSocket> socket_;
  net::MockClientSocketFactory socket_factory_;
  net::AddressList address_list_;

  base::MessageLoopForIO message_loop_;
  std::unique_ptr<base::RunLoop> run_loop_;
};

GCMConnectionHandlerImplTest::GCMConnectionHandlerImplTest()
  : last_error_(0) {
  address_list_ = net::AddressList::CreateFromIPAddress(
      net::IPAddress::IPv4Localhost(), kMCSPort);
}

GCMConnectionHandlerImplTest::~GCMConnectionHandlerImplTest() {
}

net::StreamSocket* GCMConnectionHandlerImplTest::BuildSocket(
    const ReadList& read_list,
    const WriteList& write_list) {
  mock_reads_ = read_list;
  mock_writes_ = write_list;
  data_provider_.reset(
      new net::StaticSocketDataProvider(mock_reads_, mock_writes_));
  socket_factory_.AddSocketDataProvider(data_provider_.get());

  socket_ = socket_factory_.CreateTransportClientSocket(
      address_list_, NULL, NULL, net::NetLogSource());
  socket_->Connect(net::CompletionCallback());

  run_loop_.reset(new base::RunLoop());
  PumpLoop();

  DCHECK(socket_->IsConnected());
  return socket_.get();
}

void GCMConnectionHandlerImplTest::PumpLoop() {
  run_loop_->RunUntilIdle();
  run_loop_.reset(new base::RunLoop());
}

void GCMConnectionHandlerImplTest::Connect(
    ScopedMessage* dst_proto) {
  connection_handler_.reset(new ConnectionHandlerImpl(
      TestTimeouts::tiny_timeout(),
          base::Bind(&GCMConnectionHandlerImplTest::ReadContinuation,
                     base::Unretained(this),
                     dst_proto),
          base::Bind(&GCMConnectionHandlerImplTest::WriteContinuation,
                     base::Unretained(this)),
          base::Bind(&GCMConnectionHandlerImplTest::ConnectionContinuation,
                     base::Unretained(this))));
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  connection_handler_->Init(*BuildLoginRequest(kAuthId, kAuthToken, ""),
                            TRAFFIC_ANNOTATION_FOR_TESTS, socket_.get());
}

void GCMConnectionHandlerImplTest::ReadContinuation(
    ScopedMessage* dst_proto,
    ScopedMessage new_proto) {
  *dst_proto = std::move(new_proto);
  run_loop_->Quit();
}

void GCMConnectionHandlerImplTest::WaitForMessage() {
  run_loop_->Run();
  run_loop_.reset(new base::RunLoop());
}

void GCMConnectionHandlerImplTest::WriteContinuation() {
  run_loop_->Quit();
}

void GCMConnectionHandlerImplTest::ConnectionContinuation(int error) {
  last_error_ = error;
  if (error != net::OK)
    connection_handler_->Reset();
  run_loop_->Quit();
}

// Initialize the connection handler and ensure the handshake completes
// successfully.
TEST_F(GCMConnectionHandlerImplTest, Init) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list(1, net::MockRead(net::ASYNC,
                                      handshake_response.c_str(),
                                      handshake_response.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(BuildLoginResponse(), received_message->SerializeAsString());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Simulate the handshake response returning an older version. Initialization
// should fail.
TEST_F(GCMConnectionHandlerImplTest, InitFailedVersionCheck) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  // Overwrite the version byte.
  handshake_response[0] = 37;
  ReadList read_list(1, net::MockRead(net::ASYNC,
                                      handshake_response.c_str(),
                                      handshake_response.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_FAILED, last_error());
}

// Attempt to initialize, but receive no server response, resulting in a time
// out.
TEST_F(GCMConnectionHandlerImplTest, InitTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  ReadList read_list(1, net::MockRead(net::SYNCHRONOUS,
                                      net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
}

// Attempt to initialize, but receive an incomplete server response, resulting
// in a time out.
TEST_F(GCMConnectionHandlerImplTest, InitIncompleteTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size() / 2));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
}

// Reinitialize the connection handler after failing to initialize.
TEST_F(GCMConnectionHandlerImplTest, ReInit) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  ReadList read_list(1, net::MockRead(net::SYNCHRONOUS,
                                      net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response. Should result in a connection error.
  EXPECT_FALSE(received_message.get());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());

  // Build a new socket and reconnect, successfully this time.
  std::string handshake_response = EncodeHandshakeResponse();
  read_list[0] = net::MockRead(net::ASYNC,
                               handshake_response.c_str(),
                               handshake_response.size());
  BuildSocket(read_list, write_list);
  Connect(&received_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(BuildLoginResponse(), received_message->SerializeAsString());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Verify that messages can be received after initialization.
TEST_F(GCMConnectionHandlerImplTest, RecvMsg) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Verify that if two messages arrive at once, they're treated appropriately.
TEST_F(GCMConnectionHandlerImplTest, Recv2Msgs) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_proto2 = BuildDataMessage(kDataMsgFrom2,
                                                     kDataMsgCategory2);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  data_message_pkt += EncodePacket(kDataMessageStanzaTag, data_message_proto2);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The first data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  received_message.reset();
  WaitForMessage();  // The second data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto2, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Receive a long (>128 bytes) message.
TEST_F(GCMConnectionHandlerImplTest, RecvLongMsg) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  DCHECK_GT(data_message_pkt.size(), 128U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Receive a long (>128 bytes) message in two synchronous parts.
TEST_F(GCMConnectionHandlerImplTest, RecvLongMsg2Parts) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  DCHECK_GT(data_message_pkt.size(), 128U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));

  int bytes_in_first_message = data_message_pkt.size() / 2;
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    bytes_in_first_message));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str() +
                                        bytes_in_first_message,
                                    data_message_pkt.size() -
                                        bytes_in_first_message));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(net::OK, last_error());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
}

// Receive two long (>128 bytes) message.
TEST_F(GCMConnectionHandlerImplTest, Recv2LongMsgs) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_proto2 =
      BuildDataMessage(kDataMsgFromLong2, kDataMsgCategoryLong2);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  data_message_pkt += EncodePacket(kDataMessageStanzaTag, data_message_proto2);
  DCHECK_GT(data_message_pkt.size(), 256U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The first data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  received_message.reset();
  WaitForMessage();  // The second data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto2, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Simulate a message where the end of the data does not arrive in time and the
// read times out.
TEST_F(GCMConnectionHandlerImplTest, ReadTimeout) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto = BuildDataMessage(kDataMsgFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  int bytes_in_first_message = data_message_pkt.size() / 2;
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    bytes_in_first_message));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    net::ERR_IO_PENDING));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str() +
                                        bytes_in_first_message,
                                    data_message_pkt.size() -
                                        bytes_in_first_message));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // Should time out.
  EXPECT_FALSE(received_message.get());
  EXPECT_EQ(net::ERR_TIMED_OUT, last_error());
  EXPECT_FALSE(connection_handler()->CanSendMessage());
}

// Receive a message with zero data bytes.
TEST_F(GCMConnectionHandlerImplTest, RecvMsgNoData) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_pkt = EncodePacket(kHeartbeatPingTag, "");
  ASSERT_EQ(data_message_pkt.size(), 2U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // The heartbeat ping.
  EXPECT_TRUE(received_message.get());
  EXPECT_EQ(GetMCSProtoTag(*received_message), kHeartbeatPingTag);
  EXPECT_EQ(net::OK, last_error());
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Send a message after performing the handshake.
TEST_F(GCMConnectionHandlerImplTest, SendMsg) {
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(kDataMsgFrom);
  data_message.set_category(kDataMsgCategory);
  std::string handshake_request = EncodeHandshakeRequest();
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message.SerializeAsString());
  WriteList write_list;
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      handshake_request.c_str(),
                                      handshake_request.size()));
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      data_message_pkt.c_str(),
                                      data_message_pkt.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
  connection_handler()->SendMessage(data_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The message send.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
}

// Attempt to send a message after the socket is disconnected due to a timeout.
TEST_F(GCMConnectionHandlerImplTest, SendMsgSocketDisconnected) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list;
  write_list.push_back(net::MockWrite(net::ASYNC,
                                      handshake_request.c_str(),
                                      handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS, net::ERR_IO_PENDING));
  net::StreamSocket* socket = BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  EXPECT_TRUE(connection_handler()->CanSendMessage());
  socket->Disconnect();
  mcs_proto::DataMessageStanza data_message;
  data_message.set_from(kDataMsgFrom);
  data_message.set_category(kDataMsgCategory);
  connection_handler()->SendMessage(data_message);
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  WaitForMessage();  // The message send. Should result in an error
  EXPECT_FALSE(connection_handler()->CanSendMessage());
  EXPECT_EQ(net::ERR_CONNECTION_CLOSED, last_error());
}

// Receive a message with a custom data packet that is larger than the
// default data limit (and the socket buffer limit). Should successfully
// read the packet by using the in-memory buffer.
TEST_F(GCMConnectionHandlerImplTest, ExtraLargeDataPacket) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  const std::string kVeryLongFrom(20000, '0');
  std::string data_message_proto = BuildDataMessage(kVeryLongFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Receive two messages with a custom data packet that is larger than the
// default data limit (and the socket buffer limit). Should successfully
// read the packet by using the in-memory buffer.
TEST_F(GCMConnectionHandlerImplTest, 2ExtraLargeDataPacketMsgs) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  const std::string kVeryLongFrom(20000, '0');
  std::string data_message_proto = BuildDataMessage(kVeryLongFrom,
                                                    kDataMsgCategory);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  read_list.push_back(net::MockRead(net::SYNCHRONOUS,
                                    data_message_pkt.c_str(),
                                    data_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
  received_message.reset();
  WaitForMessage();  // The second data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Make sure a message with an invalid tag is handled gracefully and resets
// the connection with an invalid argument error.
TEST_F(GCMConnectionHandlerImplTest, InvalidTag) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string invalid_message = "0";
  std::string invalid_message_pkt =
      EncodePacket(kInvalidTag, invalid_message);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC,
                                    invalid_message_pkt.c_str(),
                                    invalid_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // The invalid message.
  EXPECT_FALSE(received_message.get());
  EXPECT_EQ(net::ERR_INVALID_ARGUMENT, last_error());
}

// Receive a message where the size field spans two socket reads.
TEST_F(GCMConnectionHandlerImplTest, RecvMsgSplitSize) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC,
                                         handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();

  std::string data_message_proto =
      BuildDataMessage(kDataMsgFromLong, kDataMsgCategoryLong);
  std::string data_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);
  DCHECK_GT(data_message_pkt.size(), 128U);
  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC,
                                    handshake_response.c_str(),
                                    handshake_response.size()));
  // The first two bytes are the tag byte and the first byte of the size packet.
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str(),
                                    2));
  // Start from the second byte of the size packet.
  read_list.push_back(net::MockRead(net::ASYNC,
                                    data_message_pkt.c_str() + 2,
                                    data_message_pkt.size() - 2));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  WaitForMessage();  // The data message.
  ASSERT_TRUE(received_message.get());
  EXPECT_EQ(data_message_proto, received_message->SerializeAsString());
  EXPECT_EQ(net::OK, last_error());
}

// Make sure a message with invalid data is handled gracefully and resets
// the connection with a FAILED error.
TEST_F(GCMConnectionHandlerImplTest, InvalidData) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC, handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  std::string data_message_proto = BuildCorruptDataMessage();
  std::string invalid_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);

  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC, handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC, invalid_message_pkt.c_str(),
                                    invalid_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // The invalid message.
  EXPECT_FALSE(received_message.get());
  EXPECT_EQ(net::ERR_FAILED, last_error());
}

// Make sure a long message with invalid data is handled gracefully and resets
// the connection with a FAILED error.
TEST_F(GCMConnectionHandlerImplTest, InvalidDataLong) {
  std::string handshake_request = EncodeHandshakeRequest();
  WriteList write_list(1, net::MockWrite(net::ASYNC, handshake_request.c_str(),
                                         handshake_request.size()));
  std::string handshake_response = EncodeHandshakeResponse();
  std::string data_message_proto = BuildCorruptDataMessage();
  // Pad the corrupt data so it's beyond the normal single packet length.
  data_message_proto.resize(1 << 12);
  std::string invalid_message_pkt =
      EncodePacket(kDataMessageStanzaTag, data_message_proto);

  ReadList read_list;
  read_list.push_back(net::MockRead(net::ASYNC, handshake_response.c_str(),
                                    handshake_response.size()));
  read_list.push_back(net::MockRead(net::ASYNC, invalid_message_pkt.c_str(),
                                    invalid_message_pkt.size()));
  BuildSocket(read_list, write_list);

  ScopedMessage received_message;
  Connect(&received_message);
  WaitForMessage();  // The login send.
  WaitForMessage();  // The login response.
  received_message.reset();
  WaitForMessage();  // The invalid message.
  EXPECT_FALSE(received_message.get());
  EXPECT_EQ(net::ERR_FAILED, last_error());
}

}  // namespace
}  // namespace gcm
