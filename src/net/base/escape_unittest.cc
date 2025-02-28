// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <string>

#include "net/base/escape.h"

#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

struct EscapeCase {
  const char* input;
  const char* output;
};

struct UnescapeURLCase {
  const char* input;
  UnescapeRule::Type rules;
  const char* output;
};

struct UnescapeAndDecodeCase {
  const char* input;

  // The expected output when run through UnescapeURL.
  const char* url_unescaped;

  // The expected output when run through UnescapeQuery.
  const char* query_unescaped;

  // The expected output when run through UnescapeAndDecodeURLComponent.
  const wchar_t* decoded;
};

struct AdjustOffsetCase {
  const char* input;
  size_t input_offset;
  size_t output_offset;
};

struct EscapeForHTMLCase {
  const char* input;
  const char* expected_output;
};

TEST(EscapeTest, EscapeTextForFormSubmission) {
  const EscapeCase escape_cases[] = {
    {"foo", "foo"},
    {"foo bar", "foo+bar"},
    {"foo++", "foo%2B%2B"}
  };
  for (const auto& escape_case : escape_cases) {
    EXPECT_EQ(escape_case.output,
              EscapeQueryParamValue(escape_case.input, true));
  }

  const EscapeCase escape_cases_no_plus[] = {
    {"foo", "foo"},
    {"foo bar", "foo%20bar"},
    {"foo++", "foo%2B%2B"}
  };
  for (const auto& escape_case : escape_cases_no_plus) {
    EXPECT_EQ(escape_case.output,
              EscapeQueryParamValue(escape_case.input, false));
  }

  // Test all the values in we're supposed to be escaping.
  const std::string no_escape(
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!'()*-._~");
  for (int i = 0; i < 256; ++i) {
    std::string in;
    in.push_back(i);
    std::string out = EscapeQueryParamValue(in, true);
    if (0 == i) {
      EXPECT_EQ(out, std::string("%00"));
    } else if (32 == i) {
      // Spaces are plus escaped like web forms.
      EXPECT_EQ(out, std::string("+"));
    } else if (no_escape.find(in) == std::string::npos) {
      // Check %hex escaping
      std::string expected = base::StringPrintf("%%%02X", i);
      EXPECT_EQ(expected, out);
    } else {
      // No change for things in the no_escape list.
      EXPECT_EQ(out, in);
    }
  }
}

TEST(EscapeTest, EscapePath) {
  ASSERT_EQ(
    // Most of the character space we care about, un-escaped
    EscapePath(
      "\x02\n\x1d !\"#$%&'()*+,-./0123456789:;"
      "<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "[\\]^_`abcdefghijklmnopqrstuvwxyz"
      "{|}~\x7f\x80\xff"),
    // Escaped
    "%02%0A%1D%20!%22%23$%25&'()*+,-./0123456789%3A;"
    "%3C=%3E%3F@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "%5B%5C%5D%5E_%60abcdefghijklmnopqrstuvwxyz"
    "%7B%7C%7D~%7F%80%FF");
}

TEST(EscapeTest, DataURLWithAccentedCharacters) {
  const std::string url =
      "text/html;charset=utf-8,%3Chtml%3E%3Cbody%3ETonton,%20ton%20th%C3"
      "%A9%20t'a-t-il%20%C3%B4t%C3%A9%20ta%20toux%20";

  base::OffsetAdjuster::Adjustments adjustments;
  UnescapeAndDecodeUTF8URLComponentWithAdjustments(url, UnescapeRule::SPACES,
                                                   &adjustments);
}

TEST(EscapeTest, EscapeUrlEncodedData) {
  ASSERT_EQ(
    // Most of the character space we care about, un-escaped
    EscapeUrlEncodedData(
      "\x02\n\x1d !\"#$%&'()*+,-./0123456789:;"
      "<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "[\\]^_`abcdefghijklmnopqrstuvwxyz"
      "{|}~\x7f\x80\xff", true),
    // Escaped
    "%02%0A%1D+!%22%23%24%25%26%27()*%2B,-./0123456789:%3B"
    "%3C%3D%3E%3F%40ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "%5B%5C%5D%5E_%60abcdefghijklmnopqrstuvwxyz"
    "%7B%7C%7D~%7F%80%FF");
}

TEST(EscapeTest, EscapeUrlEncodedDataSpace) {
  ASSERT_EQ(EscapeUrlEncodedData("a b", true), "a+b");
  ASSERT_EQ(EscapeUrlEncodedData("a b", false), "a%20b");
}

TEST(EscapeTest, UnescapeURLComponent) {
  const UnescapeURLCase kUnescapeCases[] = {
      {"", UnescapeRule::NORMAL, ""},
      {"%2", UnescapeRule::NORMAL, "%2"},
      {"%%%%%%", UnescapeRule::NORMAL, "%%%%%%"},
      {"Don't escape anything", UnescapeRule::NORMAL, "Don't escape anything"},
      {"Invalid %escape %2", UnescapeRule::NORMAL, "Invalid %escape %2"},
      {"Some%20random text %25%2dOK", UnescapeRule::NONE,
       "Some%20random text %25%2dOK"},
      {"Some%20random text %25%2dOK", UnescapeRule::NORMAL,
       "Some%20random text %25-OK"},
      {"Some%20random text %25%E1%A6", UnescapeRule::NORMAL,
       "Some%20random text %25\xE1\xA6"},
      {"Some%20random text %25%E1%A6OK", UnescapeRule::NORMAL,
       "Some%20random text %25\xE1\xA6OK"},
      {"Some%20random text %25%E1%A6%99OK", UnescapeRule::NORMAL,
       "Some%20random text %25\xE1\xA6\x99OK"},

      // BiDi Control characters should not be unescaped.
      {"Some%20random text %25%D8%9COK", UnescapeRule::NORMAL,
       "Some%20random text %25%D8%9COK"},
      {"Some%20random text %25%E2%80%8EOK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%80%8EOK"},
      {"Some%20random text %25%E2%80%8FOK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%80%8FOK"},
      {"Some%20random text %25%E2%80%AAOK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%80%AAOK"},
      {"Some%20random text %25%E2%80%ABOK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%80%ABOK"},
      {"Some%20random text %25%E2%80%AEOK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%80%AEOK"},
      {"Some%20random text %25%E2%81%A6OK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%81%A6OK"},
      {"Some%20random text %25%E2%81%A9OK", UnescapeRule::NORMAL,
       "Some%20random text %25%E2%81%A9OK"},

      // Certain banned characters should not be unescaped.
      // U+1F50F LOCK WITH INK PEN
      {"Some%20random text %25%F0%9F%94%8FOK", UnescapeRule::NORMAL,
       "Some%20random text %25%F0%9F%94%8FOK"},
      // U+1F510 CLOSED LOCK WITH KEY
      {"Some%20random text %25%F0%9F%94%90OK", UnescapeRule::NORMAL,
       "Some%20random text %25%F0%9F%94%90OK"},
      // U+1F512 LOCK
      {"Some%20random text %25%F0%9F%94%92OK", UnescapeRule::NORMAL,
       "Some%20random text %25%F0%9F%94%92OK"},
      // U+1F513 OPEN LOCK
      {"Some%20random text %25%F0%9F%94%93OK", UnescapeRule::NORMAL,
       "Some%20random text %25%F0%9F%94%93OK"},

      // Spaces
      {"(%C2%85)(%C2%A0)(%E1%9A%80)(%E2%80%80)", UnescapeRule::NORMAL,
       "(%C2%85)(%C2%A0)(%E1%9A%80)(%E2%80%80)"},
      {"(%E2%80%81)(%E2%80%82)(%E2%80%83)(%E2%80%84)", UnescapeRule::NORMAL,
       "(%E2%80%81)(%E2%80%82)(%E2%80%83)(%E2%80%84)"},
      {"(%E2%80%85)(%E2%80%86)(%E2%80%87)(%E2%80%88)", UnescapeRule::NORMAL,
       "(%E2%80%85)(%E2%80%86)(%E2%80%87)(%E2%80%88)"},
      {"(%E2%80%89)(%E2%80%8A)(%E2%80%A8)(%E2%80%A9)", UnescapeRule::NORMAL,
       "(%E2%80%89)(%E2%80%8A)(%E2%80%A8)(%E2%80%A9)"},
      {"(%E2%80%AF)(%E2%81%9F)(%E3%80%80)", UnescapeRule::NORMAL,
       "(%E2%80%AF)(%E2%81%9F)(%E3%80%80)"},

      // Two spoofing characters in a row should not be unescaped.
      {"%D8%9C%D8%9C", UnescapeRule::NORMAL, "%D8%9C%D8%9C"},
      // Non-spoofing characters surrounded by spoofing characters should be
      // unescaped.
      {"%D8%9C%C2%A1%D8%9C%C2%A1", UnescapeRule::NORMAL,
       "%D8%9C\xC2\xA1%D8%9C\xC2\xA1"},
      // Invalid UTF-8 characters surrounded by spoofing characters should be
      // unescaped.
      {"%D8%9C%85%D8%9C%85", UnescapeRule::NORMAL, "%D8%9C\x85%D8%9C\x85"},
      // Test with enough trail bytes to overflow the CBU8_MAX_LENGTH-byte
      // buffer. The first two bytes are a spoofing character as well.
      {"%D8%9C%9C%9C%9C%9C%9C%9C%9C%9C%9C", UnescapeRule::NORMAL,
       "%D8%9C\x9C\x9C\x9C\x9C\x9C\x9C\x9C\x9C\x9C"},

      {"Some%20random text %25%2dOK", UnescapeRule::SPACES,
       "Some random text %25-OK"},
      {"Some%20random text %25%2dOK", UnescapeRule::PATH_SEPARATORS,
       "Some%20random text %25-OK"},
      {"Some%20random text %25%2dOK",
       UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS,
       "Some%20random text %-OK"},
      {"Some%20random text %25%2dOK",
       UnescapeRule::SPACES |
           UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS,
       "Some random text %-OK"},
      {"%A0%B1%C2%D3%E4%F5", UnescapeRule::NORMAL, "\xA0\xB1\xC2\xD3\xE4\xF5"},
      {"%Aa%Bb%Cc%Dd%Ee%Ff", UnescapeRule::NORMAL, "\xAa\xBb\xCc\xDd\xEe\xFf"},
      // Certain URL-sensitive characters should not be unescaped unless asked.
      {"Hello%20%13%10world %23# %3F? %3D= %26& %25% %2B+",
       UnescapeRule::SPACES, "Hello %13%10world %23# %3F? %3D= %26& %25% %2B+"},
      {"Hello%20%13%10world %23# %3F? %3D= %26& %25% %2B+",
       UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS,
       "Hello%20%13%10world ## ?? == && %% ++"},
      // We can neither escape nor unescape '@' since some websites expect it to
      // be preserved as either '@' or "%40".
      // See http://b/996720 and http://crbug.com/23933 .
      {"me@my%40example", UnescapeRule::NORMAL, "me@my%40example"},
      // Control characters.
      {"%01%02%03%04%05%06%07%08%09 %25",
       UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS,
       "%01%02%03%04%05%06%07%08%09 %"},
      {"Hello%20%13%10%02", UnescapeRule::SPACES, "Hello %13%10%02"},

      // '/' and '\\' should only be unescaped by PATH_SEPARATORS.
      {"%2F%5C", UnescapeRule::PATH_SEPARATORS, "/\\"},
  };

  for (const auto unescape_case : kUnescapeCases) {
    EXPECT_EQ(unescape_case.output,
              UnescapeURLComponent(unescape_case.input, unescape_case.rules));
  }

  // Test NULL character unescaping, which can't be tested above since those are
  // just char pointers.
  std::string input("Null");
  input.push_back(0);  // Also have a NULL in the input.
  input.append("%00%39Test");

  std::string expected = "Null";
  expected.push_back(0);
  expected.append("%009Test");
  EXPECT_EQ(expected, UnescapeURLComponent(input, UnescapeRule::NORMAL));
}

TEST(EscapeTest, UnescapeAndDecodeUTF8URLComponent) {
  const UnescapeAndDecodeCase unescape_cases[] = {
    { "%",
      "%",
      "%",
     L"%"},
    { "+",
      "+",
      " ",
     L"+"},
    { "%2+",
      "%2+",
      "%2 ",
     L"%2+"},
    { "+%%%+%%%",
      "+%%%+%%%",
      " %%% %%%",
     L"+%%%+%%%"},
    { "Don't escape anything",
      "Don't escape anything",
      "Don't escape anything",
     L"Don't escape anything"},
    { "+Invalid %escape %2+",
      "+Invalid %escape %2+",
      " Invalid %escape %2 ",
     L"+Invalid %escape %2+"},
    { "Some random text %25%2dOK",
      "Some random text %25-OK",
      "Some random text %25-OK",
     L"Some random text %25-OK"},
    { "%01%02%03%04%05%06%07%08%09",
      "%01%02%03%04%05%06%07%08%09",
      "%01%02%03%04%05%06%07%08%09",
     L"%01%02%03%04%05%06%07%08%09"},
    { "%E4%BD%A0+%E5%A5%BD",
      "\xE4\xBD\xA0+\xE5\xA5\xBD",
      "\xE4\xBD\xA0 \xE5\xA5\xBD",
     L"\x4f60+\x597d"},
    { "%ED%ED",  // Invalid UTF-8.
      "\xED\xED",
      "\xED\xED",
     L"%ED%ED"},  // Invalid UTF-8 -> kept unescaped.
  };

  for (const auto& unescape_case : unescape_cases) {
    std::string unescaped =
        UnescapeURLComponent(unescape_case.input, UnescapeRule::NORMAL);
    EXPECT_EQ(std::string(unescape_case.url_unescaped), unescaped);

    unescaped = UnescapeURLComponent(unescape_case.input,
                                     UnescapeRule::REPLACE_PLUS_WITH_SPACE);
    EXPECT_EQ(std::string(unescape_case.query_unescaped), unescaped);

    // TODO: Need to test unescape_spaces and unescape_percent.
    base::string16 decoded = UnescapeAndDecodeUTF8URLComponent(
        unescape_case.input, UnescapeRule::NORMAL);
    EXPECT_EQ(base::WideToUTF16(unescape_case.decoded), decoded);
  }
}

TEST(EscapeTest, AdjustOffset) {
  const AdjustOffsetCase adjust_cases[] = {
      {"", 0, 0},
      {"test", 0, 0},
      {"test", 2, 2},
      {"test", 4, 4},
      {"test", std::string::npos, std::string::npos},
      {"%2dtest", 6, 4},
      {"%2dtest", 3, 1},
      {"%2dtest", 2, std::string::npos},
      {"%2dtest", 1, std::string::npos},
      {"%2dtest", 0, 0},
      {"test%2d", 2, 2},
      {"test%2e", 2, 2},
      {"%E4%BD%A0+%E5%A5%BD", 9, 1},
      {"%E4%BD%A0+%E5%A5%BD", 6, std::string::npos},
      {"%E4%BD%A0+%E5%A5%BD", 0, 0},
      {"%E4%BD%A0+%E5%A5%BD", 10, 2},
      {"%E4%BD%A0+%E5%A5%BD", 19, 3},

      {"hi%41test%E4%BD%A0+%E5%A5%BD", 18, 8},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 15, std::string::npos},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 9, 7},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 19, 9},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 28, 10},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 0, 0},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 2, 2},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 3, std::string::npos},
      {"hi%41test%E4%BD%A0+%E5%A5%BD", 5, 3},

      {"%E4%BD%A0+%E5%A5%BDhi%41test", 9, 1},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 6, std::string::npos},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 0, 0},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 10, 2},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 19, 3},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 21, 5},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 22, std::string::npos},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 24, 6},
      {"%E4%BD%A0+%E5%A5%BDhi%41test", 28, 10},

      {"%ED%B0%80+%E5%A5%BD", 6, 6},  // not convertable to UTF-8
  };

  for (const auto& adjust_case : adjust_cases) {
    size_t offset = adjust_case.input_offset;
    base::OffsetAdjuster::Adjustments adjustments;
    UnescapeAndDecodeUTF8URLComponentWithAdjustments(
        adjust_case.input, UnescapeRule::NORMAL, &adjustments);
    base::OffsetAdjuster::AdjustOffset(adjustments, &offset);
    EXPECT_EQ(adjust_case.output_offset, offset)
        << "input=" << adjust_case.input
        << " offset=" << adjust_case.input_offset;
  }
}

TEST(EscapeTest, UnescapeBinaryURLComponent) {
  const UnescapeURLCase kTestCases[] = {
      // Check that ASCII characters with special handling in
      // UnescapeURLComponent() are still unescaped.
      {"%09%20%25foo%2F", UnescapeRule::NORMAL, "\x09 %foo/"},

      // UTF-8 Characters banned by UnescapeURLComponent() should also be
      // unescaped.
      {"Some random text %D8%9COK", UnescapeRule::NORMAL,
       "Some random text \xD8\x9COK"},
      {"Some random text %F0%9F%94%8FOK", UnescapeRule::NORMAL,
       "Some random text \xF0\x9F\x94\x8FOK"},

      // As should invalid UTF-8 characters.
      {"%A0%A0%E9%E9%A0%A0%A0%A0", UnescapeRule::NORMAL,
       "\xA0\xA0\xE9\xE9\xA0\xA0\xA0\xA0"},

      // And valid UTF-8 characters that are not banned by
      // UnescapeURLComponent() should be unescaped, too!
      {"%C2%A1%C2%A1", UnescapeRule::NORMAL, "\xC2\xA1\xC2\xA1"},

      // '+' should be left alone by default
      {"++%2B++", UnescapeRule::NORMAL, "+++++"},
      // But should magically be turned into a space if requested.
      {"++%2B++", UnescapeRule::REPLACE_PLUS_WITH_SPACE, "  +  "},
  };

  for (const auto& test_case : kTestCases) {
    EXPECT_EQ(std::string(test_case.output),
              UnescapeBinaryURLComponent(test_case.input, test_case.rules));
  }

  // Test NULL character unescaping, which can't be tested above since those are
  // just char pointers.
  std::string input("Null");
  input.push_back(0);  // Also have a NULL in the input.
  input.append("%00%39Test");

  std::string expected("Null");
  expected.push_back(0);
  expected.push_back(0);
  expected.append("9Test");
  EXPECT_EQ(expected, UnescapeBinaryURLComponent(input));
}

TEST(EscapeTest, EscapeForHTML) {
  const EscapeForHTMLCase tests[] = {
    { "hello", "hello" },
    { "<hello>", "&lt;hello&gt;" },
    { "don\'t mess with me", "don&#39;t mess with me" },
  };
  for (const auto& test : tests) {
    std::string result = EscapeForHTML(std::string(test.input));
    EXPECT_EQ(std::string(test.expected_output), result);
  }
}

TEST(EscapeTest, UnescapeForHTML) {
  const EscapeForHTMLCase tests[] = {
    { "", "" },
    { "&lt;hello&gt;", "<hello>" },
    { "don&#39;t mess with me", "don\'t mess with me" },
    { "&lt;&gt;&amp;&quot;&#39;", "<>&\"'" },
    { "& lt; &amp ; &; '", "& lt; &amp ; &; '" },
    { "&amp;", "&" },
    { "&quot;", "\"" },
    { "&#39;", "'" },
    { "&lt;", "<" },
    { "&gt;", ">" },
    { "&amp; &", "& &" },
  };
  for (const auto& test : tests) {
    base::string16 result = UnescapeForHTML(base::ASCIIToUTF16(test.input));
    EXPECT_EQ(base::ASCIIToUTF16(test.expected_output), result);
  }
}

TEST(EscapeTest, EscapeExternalHandlerValue) {
  ASSERT_EQ(
      // Escaped
      "%02%0A%1D%20!%22#$%25&'()*+,-./0123456789:;"
      "%3C=%3E?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "[%5C]%5E_%60abcdefghijklmnopqrstuvwxyz"
      "%7B%7C%7D~%7F%80%FF",
      // Most of the character space we care about, un-escaped
      EscapeExternalHandlerValue(
          "\x02\n\x1d !\"#$%&'()*+,-./0123456789:;"
          "<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "[\\]^_`abcdefghijklmnopqrstuvwxyz"
          "{|}~\x7f\x80\xff"));

  ASSERT_EQ(
      "!#$&'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_"
      "abcdefghijklmnopqrstuvwxyz~",
      EscapeExternalHandlerValue(
          "!#$&'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]_"
          "abcdefghijklmnopqrstuvwxyz~"));

  ASSERT_EQ("%258k", EscapeExternalHandlerValue("%8k"));
  ASSERT_EQ("a%25", EscapeExternalHandlerValue("a%"));
  ASSERT_EQ("%25a", EscapeExternalHandlerValue("%a"));
  ASSERT_EQ("a%258", EscapeExternalHandlerValue("a%8"));
  ASSERT_EQ("%ab", EscapeExternalHandlerValue("%ab"));
  ASSERT_EQ("%AB", EscapeExternalHandlerValue("%AB"));

  ASSERT_EQ("http://example.com/path/sub?q=a%7Cb%7Cc&q=1%7C2%7C3#ref%7C",
            EscapeExternalHandlerValue(
                "http://example.com/path/sub?q=a|b|c&q=1|2|3#ref|"));
  ASSERT_EQ("http://example.com/path/sub?q=a%7Cb%7Cc&q=1%7C2%7C3#ref%7C",
            EscapeExternalHandlerValue(
                "http://example.com/path/sub?q=a%7Cb%7Cc&q=1%7C2%7C3#ref%7C"));
  ASSERT_EQ("http://[2001:db8:0:1]:80",
            EscapeExternalHandlerValue("http://[2001:db8:0:1]:80"));
}

}  // namespace
}  // namespace net
