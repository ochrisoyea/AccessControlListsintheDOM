// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/multiplexed_http_stream.h"

#include <utility>

#include "base/logging.h"
#include "net/http/http_raw_request_headers.h"
#include "net/third_party/spdy/core/spdy_header_block.h"

namespace net {

MultiplexedHttpStream::MultiplexedHttpStream(
    std::unique_ptr<MultiplexedSessionHandle> session)
    : session_(std::move(session)) {}

MultiplexedHttpStream::~MultiplexedHttpStream() = default;

bool MultiplexedHttpStream::GetRemoteEndpoint(IPEndPoint* endpoint) {
  return session_->GetRemoteEndpoint(endpoint);
}

void MultiplexedHttpStream::GetSSLInfo(SSLInfo* ssl_info) {
  session_->GetSSLInfo(ssl_info);
}

void MultiplexedHttpStream::SaveSSLInfo() {
  session_->SaveSSLInfo();
}

void MultiplexedHttpStream::GetSSLCertRequestInfo(
    SSLCertRequestInfo* cert_request_info) {
  // A multiplexed stream cannot request client certificates. Client
  // authentication may only occur during the initial SSL handshake.
  NOTREACHED();
}

Error MultiplexedHttpStream::GetTokenBindingSignature(
    crypto::ECPrivateKey* key,
    TokenBindingType tb_type,
    std::vector<uint8_t>* out) {
  return session_->GetTokenBindingSignature(key, tb_type, out);
}

void MultiplexedHttpStream::Drain(HttpNetworkSession* session) {
  NOTREACHED();
  Close(false);
  delete this;
}

HttpStream* MultiplexedHttpStream::RenewStreamForAuth() {
  return nullptr;
}

void MultiplexedHttpStream::SetConnectionReused() {}

bool MultiplexedHttpStream::CanReuseConnection() const {
  // Multiplexed streams aren't considered reusable.
  return false;
}

void MultiplexedHttpStream::SetRequestHeadersCallback(
    RequestHeadersCallback callback) {
  request_headers_callback_ = std::move(callback);
}

void MultiplexedHttpStream::DispatchRequestHeadersCallback(
    const SpdyHeaderBlock& spdy_headers) {
  if (!request_headers_callback_)
    return;
  HttpRawRequestHeaders raw_headers;
  for (const auto& entry : spdy_headers)
    raw_headers.Add(entry.first, entry.second);
  request_headers_callback_.Run(std::move(raw_headers));
}

}  // namespace net
