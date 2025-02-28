// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/spdy/core/hpack/hpack_entry.h"

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "net/third_party/spdy/platform/api/spdy_estimate_memory_usage.h"
#include "net/third_party/spdy/platform/api/spdy_string_utils.h"

namespace net {

const size_t HpackEntry::kSizeOverhead = 32;

HpackEntry::HpackEntry(SpdyStringPiece name,
                       SpdyStringPiece value,
                       bool is_static,
                       size_t insertion_index)
    : name_(name.data(), name.size()),
      value_(value.data(), value.size()),
      name_ref_(name_),
      value_ref_(value_),
      insertion_index_(insertion_index),
      type_(is_static ? STATIC : DYNAMIC),
      time_added_(0) {}

HpackEntry::HpackEntry(SpdyStringPiece name, SpdyStringPiece value)
    : name_ref_(name),
      value_ref_(value),
      insertion_index_(0),
      type_(LOOKUP),
      time_added_(0) {}

HpackEntry::HpackEntry() : insertion_index_(0), type_(LOOKUP), time_added_(0) {}

HpackEntry::HpackEntry(const HpackEntry& other)
    : insertion_index_(other.insertion_index_),
      type_(other.type_),
      time_added_(0) {
  if (type_ == LOOKUP) {
    name_ref_ = other.name_ref_;
    value_ref_ = other.value_ref_;
  } else {
    name_ = other.name_;
    value_ = other.value_;
    name_ref_ = SpdyStringPiece(name_.data(), name_.size());
    value_ref_ = SpdyStringPiece(value_.data(), value_.size());
  }
}

HpackEntry& HpackEntry::operator=(const HpackEntry& other) {
  insertion_index_ = other.insertion_index_;
  type_ = other.type_;
  if (type_ == LOOKUP) {
    name_ref_ = other.name_ref_;
    value_ref_ = other.value_ref_;
    return *this;
  }
  name_ = other.name_;
  value_ = other.value_;
  name_ref_ = SpdyStringPiece(name_.data(), name_.size());
  value_ref_ = SpdyStringPiece(value_.data(), value_.size());
  return *this;
}

HpackEntry::~HpackEntry() = default;

// static
size_t HpackEntry::Size(SpdyStringPiece name, SpdyStringPiece value) {
  return name.size() + value.size() + kSizeOverhead;
}

size_t HpackEntry::Size() const {
  return Size(name(), value());
}

SpdyString HpackEntry::GetDebugString() const {
  return SpdyStringPrintf(
      "{ name: \"%.*s\", value: \"%.*s\", index: %d %s }", name_ref_.size(),
      name_ref_.data(), value_ref_.size(), value_ref_.data(), insertion_index_,
      (IsStatic() ? " static" : (IsLookup() ? " lookup" : " dynamic")));
}

size_t HpackEntry::EstimateMemoryUsage() const {
  return SpdyEstimateMemoryUsage(name_) + SpdyEstimateMemoryUsage(value_);
}

}  // namespace net
