// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/header_coalescer.h"

#include <string>
#include <vector>

#include "net/log/test_net_log.h"
#include "net/spdy/spdy_test_util_common.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::ElementsAre;
using ::testing::Pair;

namespace net {
namespace test {

class HeaderCoalescerTest : public ::testing::Test {
 public:
  HeaderCoalescerTest()
      : header_coalescer_(kMaxHeaderListSizeForTest, net_log_.bound()) {}

  void ExpectEntry(base::StringPiece expected_header_name,
                   base::StringPiece expected_header_value,
                   base::StringPiece expected_error_message) {
    TestNetLogEntry::List entry_list;
    net_log_.GetEntries(&entry_list);
    ASSERT_EQ(1u, entry_list.size());
    EXPECT_EQ(entry_list[0].type,
              NetLogEventType::HTTP2_SESSION_RECV_INVALID_HEADER);
    EXPECT_EQ(entry_list[0].source.id, net_log_.bound().source().id);
    std::string value;
    EXPECT_TRUE(entry_list[0].GetStringValue("header_name", &value));
    EXPECT_EQ(expected_header_name, value);
    EXPECT_TRUE(entry_list[0].GetStringValue("header_value", &value));
    EXPECT_EQ(expected_header_value, value);
    EXPECT_TRUE(entry_list[0].GetStringValue("error", &value));
    EXPECT_EQ(expected_error_message, value);
  }

 protected:
  BoundTestNetLog net_log_;
  HeaderCoalescer header_coalescer_;
};

TEST_F(HeaderCoalescerTest, CorrectHeaders) {
  header_coalescer_.OnHeader(":foo", "bar");
  header_coalescer_.OnHeader("baz", "qux");
  EXPECT_FALSE(header_coalescer_.error_seen());

  SpdyHeaderBlock header_block = header_coalescer_.release_headers();
  EXPECT_THAT(header_block,
              ElementsAre(Pair(":foo", "bar"), Pair("baz", "qux")));
}

TEST_F(HeaderCoalescerTest, EmptyHeaderKey) {
  header_coalescer_.OnHeader("", "foo");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("", "foo", "Header name must not be empty.");
}

TEST_F(HeaderCoalescerTest, HeaderBlockTooLarge) {
  // key + value + overhead = 3 + kMaxHeaderListSizeForTest - 40 + 32
  // = kMaxHeaderListSizeForTest - 5
  std::string data(kMaxHeaderListSizeForTest - 40, 'a');
  header_coalescer_.OnHeader("foo", data);
  EXPECT_FALSE(header_coalescer_.error_seen());

  // Another 3 + 4 + 32 bytes: too large.
  base::StringPiece header_value("abcd");
  header_coalescer_.OnHeader("bar", header_value);
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("bar", "abcd", "Header list too large.");
}

TEST_F(HeaderCoalescerTest, PseudoHeadersMustNotFollowRegularHeaders) {
  header_coalescer_.OnHeader("foo", "bar");
  EXPECT_FALSE(header_coalescer_.error_seen());
  header_coalescer_.OnHeader(":baz", "qux");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry(":baz", "qux", "Pseudo header must not follow regular headers.");
}

TEST_F(HeaderCoalescerTest, Append) {
  header_coalescer_.OnHeader("foo", "bar");
  header_coalescer_.OnHeader("cookie", "baz");
  header_coalescer_.OnHeader("foo", "quux");
  header_coalescer_.OnHeader("cookie", "qux");
  EXPECT_FALSE(header_coalescer_.error_seen());

  SpdyHeaderBlock header_block = header_coalescer_.release_headers();
  EXPECT_THAT(header_block,
              ElementsAre(Pair("foo", base::StringPiece("bar\0quux", 8)),
                          Pair("cookie", "baz; qux")));
}

TEST_F(HeaderCoalescerTest, HeaderNameNotValid) {
  base::StringPiece header_name("\x1\x7F\x80\xFF");
  header_coalescer_.OnHeader(header_name, "foo");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("%01%7F%80%FF", "foo", "Invalid character in header name.");
}

// RFC 7540 Section 8.1.2.6. Uppercase in header name is invalid.
TEST_F(HeaderCoalescerTest, HeaderNameHasUppercase) {
  base::StringPiece header_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  header_coalescer_.OnHeader(header_name, "foo");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "foo",
              "Upper case characters in header name.");
}

// RFC 7230 Section 3.2. Valid header name is defined as:
// field-name     = token
// token          = 1*tchar
// tchar          = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
//                  "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
TEST_F(HeaderCoalescerTest, HeaderNameValid) {
  // Due to RFC 7540 Section 8.1.2.6. Uppercase characters are not included.
  base::StringPiece header_name(
      "abcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-."
      "^_`|~");
  header_coalescer_.OnHeader(header_name, "foo");
  EXPECT_FALSE(header_coalescer_.error_seen());
  SpdyHeaderBlock header_block = header_coalescer_.release_headers();
  EXPECT_THAT(header_block, ElementsAre(Pair(header_name, "foo")));
}

// According to RFC 7540 Section 10.3 and RFC 7230 Section 3.2, allowed
// characters in header values are '\t', '  ', 0x21 to 0x7E, and 0x80 to 0xFF.
TEST_F(HeaderCoalescerTest, HeaderValueValid) {
  header_coalescer_.OnHeader("foo", " bar \x21 \x7e baz\tqux\x80\xff ");
  EXPECT_FALSE(header_coalescer_.error_seen());
}

TEST_F(HeaderCoalescerTest, HeaderValueContainsLF) {
  header_coalescer_.OnHeader("foo", "bar\nbaz");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("foo", "bar%0Abaz", "Invalid character 0x0A in header value.");
}

TEST_F(HeaderCoalescerTest, HeaderValueContainsCR) {
  header_coalescer_.OnHeader("foo", "bar\rbaz");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("foo", "bar%0Dbaz", "Invalid character 0x0D in header value.");
}

TEST_F(HeaderCoalescerTest, HeaderValueContains0x7f) {
  header_coalescer_.OnHeader("foo", "bar\x7f baz");
  EXPECT_TRUE(header_coalescer_.error_seen());
  ExpectEntry("foo", "bar%7F%20baz", "Invalid character 0x7F in header value.");
}

}  // namespace test

}  // namespace net
