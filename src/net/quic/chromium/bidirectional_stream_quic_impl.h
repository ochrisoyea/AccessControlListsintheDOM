// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_QUIC_CHROMIUM_BIDIRECTIONAL_STREAM_QUIC_IMPL_H_
#define NET_QUIC_CHROMIUM_BIDIRECTIONAL_STREAM_QUIC_IMPL_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "net/base/load_timing_info.h"
#include "net/base/net_export.h"
#include "net/http/bidirectional_stream_impl.h"
#include "net/quic/chromium/quic_chromium_client_session.h"
#include "net/quic/chromium/quic_chromium_client_stream.h"
#include "net/third_party/spdy/core/spdy_header_block.h"

namespace base {
class Timer;
}  // namespace base

namespace net {

struct BidirectionalStreamRequestInfo;
class IOBuffer;

class NET_EXPORT_PRIVATE BidirectionalStreamQuicImpl
    : public BidirectionalStreamImpl {
 public:
  explicit BidirectionalStreamQuicImpl(
      std::unique_ptr<QuicChromiumClientSession::Handle> session);

  ~BidirectionalStreamQuicImpl() override;

  // BidirectionalStreamImpl implementation:
  void Start(const BidirectionalStreamRequestInfo* request_info,
             const NetLogWithSource& net_log,
             bool send_request_headers_automatically,
             BidirectionalStreamImpl::Delegate* delegate,
             std::unique_ptr<base::Timer> timer,
             const NetworkTrafficAnnotationTag& traffic_annotation) override;
  void SendRequestHeaders() override;
  int ReadData(IOBuffer* buffer, int buffer_len) override;
  void SendvData(const std::vector<scoped_refptr<IOBuffer>>& buffers,
                 const std::vector<int>& lengths,
                 bool end_stream) override;
  NextProto GetProtocol() const override;
  int64_t GetTotalReceivedBytes() const override;
  int64_t GetTotalSentBytes() const override;
  bool GetLoadTimingInfo(LoadTimingInfo* load_timing_info) const override;
  void PopulateNetErrorDetails(NetErrorDetails* details) override;

 private:
  int WriteHeaders();
  void OnStreamReady(int rv);
  void OnSendDataComplete(int rv);
  void ReadInitialHeaders();
  void OnReadInitialHeadersComplete(int rv);
  void ReadTrailingHeaders();
  void OnReadTrailingHeadersComplete(int rv);
  void OnReadDataComplete(int rv);

  // Notifies the delegate of an error, clears |stream_| and |delegate_|,
  // and cancels any pending callbacks.
  void NotifyError(int error);
  // Notifies the delegate of an error, clears |stream_| and |delegate_|,
  // and cancels any pending callbacks. If |notify_delegate_later| is true
  // then the delegate will be notified asynchronously via a posted task,
  // otherwise the notification will be synchronous.
  void NotifyErrorImpl(int error, bool notify_delegate_later);
  // Notifies the delegate that the stream is ready.
  void NotifyStreamReady();
  // Resets the stream and ensures that |delegate_| won't be called back.
  void ResetStream();
  // Invokes OnFailure(error) on |delegate|.
  void NotifyFailure(BidirectionalStreamImpl::Delegate* delegate, int error);

  const std::unique_ptr<QuicChromiumClientSession::Handle> session_;
  std::unique_ptr<QuicChromiumClientStream::Handle> stream_;

  const BidirectionalStreamRequestInfo* request_info_;
  BidirectionalStreamImpl::Delegate* delegate_;
  // Saves the response status if the stream is explicitly closed via OnError
  // or OnClose with an error. Once all buffered data has been returned, this
  // will be used as the final response.
  int response_status_;

  // The protocol that is negotiated.
  NextProto negotiated_protocol_;
  // Connect timing information for this stream. Populated when headers are
  // received.
  LoadTimingInfo::ConnectTiming connect_timing_;

  SpdyHeaderBlock initial_headers_;
  SpdyHeaderBlock trailing_headers_;

  // User provided read buffer for ReadData() response.
  scoped_refptr<IOBuffer> read_buffer_;
  int read_buffer_len_;

  // Number of bytes received by the headers stream on behalf of this stream.
  int64_t headers_bytes_received_;
  // Number of bytes sent by the headers stream on behalf of this stream.
  int64_t headers_bytes_sent_;
  // After |stream_| has been closed, this keeps track of the total number of
  // bytes received over the network for |stream_| while it was open.
  int64_t closed_stream_received_bytes_;
  // After |stream_| has been closed, this keeps track of the total number of
  // bytes sent over the network for |stream_| while it was open.
  int64_t closed_stream_sent_bytes_;
  // True if the stream is the first stream negotiated on the session. Set when
  // the stream was closed. If |stream_| is failed to be created, this takes on
  // the default value of false.
  bool closed_is_first_stream_;
  // Indicates whether initial headers have been sent.
  bool has_sent_headers_;

  // Whether to automatically send request headers when stream is negotiated.
  // If false, headers will not be sent until SendRequestHeaders() is called or
  // until next SendData/SendvData, during which QUIC will try to combine header
  // frame with data frame in the same packet if possible.
  bool send_request_headers_automatically_;

  // True when callbacks to the delegate may be invoked synchronously.
  bool may_invoke_callbacks_;

  base::WeakPtrFactory<BidirectionalStreamQuicImpl> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(BidirectionalStreamQuicImpl);
};

}  // namespace net

#endif  // NET_QUIC_CHROMIUM_BIDIRECTIONAL_STREAM_QUIC_IMPL_H_
