// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A toy server, which listens on a specified address for QUIC traffic and
// handles incoming responses.

#ifndef NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_H_
#define NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_H_

#include <memory>

#include "base/macros.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_endpoint.h"
#include "net/log/net_log.h"
#include "net/quic/chromium/quic_chromium_alarm_factory.h"
#include "net/quic/chromium/quic_chromium_connection_helper.h"
#include "net/third_party/quic/core/crypto/quic_crypto_server_config.h"
#include "net/third_party/quic/core/quic_config.h"
#include "net/third_party/quic/core/quic_version_manager.h"
#include "net/third_party/quic/platform/impl/quic_chromium_clock.h"
#include "net/third_party/quic/tools/quic_http_response_cache.h"

namespace net {

class UDPServerSocket;


class QuicDispatcher;

namespace test {
class QuicSimpleServerPeer;
}  // namespace test

class QuicSimpleServer {
 public:
  QuicSimpleServer(
      std::unique_ptr<ProofSource> proof_source,
      const QuicConfig& config,
      const QuicCryptoServerConfig::ConfigOptions& crypto_config_options,
      const ParsedQuicVersionVector& supported_versions,
      QuicHttpResponseCache* response_cache);

  virtual ~QuicSimpleServer();

  // Start listening on the specified address. Returns an error code.
  int Listen(const IPEndPoint& address);

  // Server deletion is imminent. Start cleaning up.
  void Shutdown();

  // Start reading on the socket. On asynchronous reads, this registers
  // OnReadComplete as the callback, which will then call StartReading again.
  void StartReading();

  // Called on reads that complete asynchronously. Dispatches the packet and
  // continues the read loop.
  void OnReadComplete(int result);

  QuicDispatcher* dispatcher() { return dispatcher_.get(); }

  IPEndPoint server_address() const { return server_address_; }

 private:
  friend class test::QuicSimpleServerPeer;

  // Initialize the internal state of the server.
  void Initialize();

  QuicVersionManager version_manager_;

  // Accepts data from the framer and demuxes clients to sessions.
  std::unique_ptr<QuicDispatcher> dispatcher_;

  // Used by the helper_ to time alarms.
  QuicChromiumClock clock_;

  // Used to manage the message loop. Owned by dispatcher_.
  QuicChromiumConnectionHelper* helper_;

  // Used to manage the message loop. Owned by dispatcher_.
  QuicChromiumAlarmFactory* alarm_factory_;

  // Listening socket. Also used for outbound client communication.
  std::unique_ptr<UDPServerSocket> socket_;

  // config_ contains non-crypto parameters that are negotiated in the crypto
  // handshake.
  QuicConfig config_;
  // crypto_config_ contains crypto parameters that are negotiated in the crypto
  // handshake.
  QuicCryptoServerConfig::ConfigOptions crypto_config_options_;
  // crypto_config_ contains crypto parameters for the handshake.
  QuicCryptoServerConfig crypto_config_;

  // The address that the server listens on.
  IPEndPoint server_address_;

  // Keeps track of whether a read is currently in flight, after which
  // OnReadComplete will be called.
  bool read_pending_;

  // The number of iterations of the read loop that have completed synchronously
  // and without posting a new task to the message loop.
  int synchronous_read_count_;

  // The target buffer of the current read.
  scoped_refptr<IOBufferWithSize> read_buffer_;

  // The source address of the current read.
  IPEndPoint client_address_;

  // The log to use for the socket.
  NetLog net_log_;

  QuicHttpResponseCache* response_cache_;

  base::WeakPtrFactory<QuicSimpleServer> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(QuicSimpleServer);
};

}  // namespace net

#endif  // NET_TOOLS_QUIC_QUIC_SIMPLE_SERVER_H_
