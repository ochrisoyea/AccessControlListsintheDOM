// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_THIRD_PARTY_QUIC_TEST_TOOLS_SIMULATOR_QUIC_ENDPOINT_H_
#define NET_THIRD_PARTY_QUIC_TEST_TOOLS_SIMULATOR_QUIC_ENDPOINT_H_

#include "net/third_party/quic/core/crypto/null_decrypter.h"
#include "net/third_party/quic/core/crypto/null_encrypter.h"
#include "net/third_party/quic/core/quic_connection.h"
#include "net/third_party/quic/core/quic_default_packet_writer.h"
#include "net/third_party/quic/core/quic_packets.h"
#include "net/third_party/quic/core/quic_stream_frame_data_producer.h"
#include "net/third_party/quic/platform/api/quic_containers.h"
#include "net/third_party/quic/test_tools/simple_session_notifier.h"
#include "net/third_party/quic/test_tools/simulator/link.h"
#include "net/third_party/quic/test_tools/simulator/queue.h"

namespace net {
namespace simulator {

// Size of the TX queue used by the kernel/NIC.  1000 is the Linux
// kernel default.
const QuicByteCount kTxQueueSize = 1000;

// Generate a random local network host-port tuple based on the name of the
// endpoint.
QuicSocketAddress GetAddressFromName(std::string name);

// A QUIC connection endpoint.  Wraps around QuicConnection.  In order to
// initiate a transfer, the caller has to call AddBytesToTransfer().  The data
// transferred is always the same and is always transferred on a single stream.
// The endpoint receives all packets addressed to it, and verifies that the data
// received is what it's supposed to be.
class QuicEndpoint : public Endpoint,
                     public UnconstrainedPortInterface,
                     public Queue::ListenerInterface,
                     public QuicConnectionVisitorInterface,
                     public SessionNotifierInterface {
 public:
  QuicEndpoint(Simulator* simulator,
               std::string name,
               std::string peer_name,
               Perspective perspective,
               QuicConnectionId connection_id);
  ~QuicEndpoint() override;

  inline QuicConnection* connection() { return &connection_; }
  QuicByteCount bytes_to_transfer() const;
  QuicByteCount bytes_transferred() const;
  QuicByteCount bytes_received() const;
  inline size_t write_blocked_count() { return write_blocked_count_; }
  inline bool wrong_data_received() const { return wrong_data_received_; }

  // Send |bytes| bytes.  Initiates the transfer if one is not already in
  // progress.
  void AddBytesToTransfer(QuicByteCount bytes);

  // UnconstrainedPortInterface method.  Called whenever the endpoint receives a
  // packet.
  void AcceptPacket(std::unique_ptr<Packet> packet) override;

  // Begin Endpoint implementation.
  UnconstrainedPortInterface* GetRxPort() override;
  void SetTxPort(ConstrainedPortInterface* port) override;
  // End Endpoint implementation.

  // Actor method.
  void Act() override {}

  // Queue::ListenerInterface method.
  void OnPacketDequeued() override;

  // Begin QuicConnectionVisitorInterface implementation.
  void OnStreamFrame(const QuicStreamFrame& frame) override;
  void OnCanWrite() override;
  bool WillingAndAbleToWrite() const override;
  bool HasPendingHandshake() const override;
  bool HasOpenDynamicStreams() const override;

  void OnWindowUpdateFrame(const QuicWindowUpdateFrame& frame) override {}
  void OnBlockedFrame(const QuicBlockedFrame& frame) override {}
  void OnRstStream(const QuicRstStreamFrame& frame) override {}
  void OnGoAway(const QuicGoAwayFrame& frame) override {}
  void OnConnectionClosed(QuicErrorCode error,
                          const std::string& error_details,
                          ConnectionCloseSource source) override {}
  void OnWriteBlocked() override {}
  void OnSuccessfulVersionNegotiation(
      const ParsedQuicVersion& version) override {}
  void OnConnectivityProbeReceived(
      const QuicSocketAddress& self_address,
      const QuicSocketAddress& peer_address) override {}
  void OnCongestionWindowChange(QuicTime now) override {}
  void OnConnectionMigration(AddressChangeType type) override {}
  void OnPathDegrading() override {}
  void PostProcessAfterData() override {}
  void OnAckNeedsRetransmittableFrame() override {}
  void SendPing() override {}
  bool AllowSelfAddressChange() const override;
  void OnForwardProgressConfirmed() override {}
  // End QuicConnectionVisitorInterface implementation.

  // Begin SessionNotifierInterface methods:
  bool OnFrameAcked(const QuicFrame& frame,
                    QuicTime::Delta ack_delay_time) override;
  void OnStreamFrameRetransmitted(const QuicStreamFrame& frame) override {}
  void OnFrameLost(const QuicFrame& frame) override;
  void RetransmitFrames(const QuicFrames& frames,
                        TransmissionType type) override;
  bool IsFrameOutstanding(const QuicFrame& frame) const override;
  bool HasPendingCryptoData() const override;
  // End SessionNotifierInterface implementation.

 private:
  // A Writer object that writes into the |nic_tx_queue_|.
  class Writer : public QuicPacketWriter {
   public:
    explicit Writer(QuicEndpoint* endpoint);
    ~Writer() override;

    WriteResult WritePacket(const char* buffer,
                            size_t buf_len,
                            const QuicIpAddress& self_address,
                            const QuicSocketAddress& peer_address,
                            PerPacketOptions* options) override;
    bool IsWriteBlockedDataBuffered() const override;
    bool IsWriteBlocked() const override;
    void SetWritable() override;
    QuicByteCount GetMaxPacketSize(
        const QuicSocketAddress& peer_address) const override;

   private:
    QuicEndpoint* endpoint_;

    bool is_blocked_;
  };

  // The producer outputs the repetition of the same byte.  That sequence is
  // verified by the receiver.
  class DataProducer : public QuicStreamFrameDataProducer {
   public:
    bool WriteStreamData(QuicStreamId id,
                         QuicStreamOffset offset,
                         QuicByteCount data_length,
                         QuicDataWriter* writer) override;
  };

  // Write stream data until |bytes_to_transfer_| is zero or the connection is
  // write-blocked.
  void WriteStreamData();

  std::string peer_name_;

  Writer writer_;
  DataProducer producer_;
  // The queue for the outgoing packets.  In reality, this might be either on
  // the network card, or in the kernel, but for concreteness we assume it's on
  // the network card.
  Queue nic_tx_queue_;
  QuicConnection connection_;

  QuicByteCount bytes_to_transfer_;
  QuicByteCount bytes_transferred_;

  // Counts the number of times the writer became write-blocked.
  size_t write_blocked_count_;

  // Set to true if the endpoint receives stream data different from what it
  // expects.
  bool wrong_data_received_;

  // Record of received offsets in the data stream.
  QuicIntervalSet<QuicStreamOffset> offsets_received_;

  std::unique_ptr<test::SimpleSessionNotifier> notifier_;
};

// Multiplexes multiple connections at the same host on the network.
class QuicEndpointMultiplexer : public Endpoint,
                                public UnconstrainedPortInterface {
 public:
  QuicEndpointMultiplexer(std::string name,
                          std::initializer_list<QuicEndpoint*> endpoints);
  ~QuicEndpointMultiplexer() override;

  // Receives a packet and passes it to the specified endpoint if that endpoint
  // is one of the endpoints being multiplexed, otherwise ignores the packet.
  void AcceptPacket(std::unique_ptr<Packet> packet) override;
  UnconstrainedPortInterface* GetRxPort() override;

  // Sets the egress port for all the endpoints being multiplexed.
  void SetTxPort(ConstrainedPortInterface* port) override;

  void Act() override {}

 private:
  QuicUnorderedMap<std::string, QuicEndpoint*> mapping_;
};

}  // namespace simulator
}  // namespace net

#endif  // NET_THIRD_PARTY_QUIC_TEST_TOOLS_SIMULATOR_QUIC_ENDPOINT_H_
