// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/escape.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "base/third_party/icu/icu_utf.h"

namespace net {

namespace {

const char kHexString[] = "0123456789ABCDEF";
inline char IntToHex(int i) {
  DCHECK_GE(i, 0) << i << " not a hex value";
  DCHECK_LE(i, 15) << i << " not a hex value";
  return kHexString[i];
}

// A fast bit-vector map for ascii characters.
//
// Internally stores 256 bits in an array of 8 ints.
// Does quick bit-flicking to lookup needed characters.
struct Charmap {
  bool Contains(unsigned char c) const {
    return ((map[c >> 5] & (1 << (c & 31))) != 0);
  }

  uint32_t map[8];
};

// Given text to escape and a Charmap defining which values to escape,
// return an escaped string.  If use_plus is true, spaces are converted
// to +, otherwise, if spaces are in the charmap, they are converted to
// %20. And if keep_escaped is true, %XX will be kept as it is, otherwise, if
// '%' is in the charmap, it is converted to %25.
std::string Escape(base::StringPiece text,
                   const Charmap& charmap,
                   bool use_plus,
                   bool keep_escaped = false) {
  std::string escaped;
  escaped.reserve(text.length() * 3);
  for (unsigned int i = 0; i < text.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (use_plus && ' ' == c) {
      escaped.push_back('+');
    } else if (keep_escaped && '%' == c && i + 2 < text.length() &&
               base::IsHexDigit(text[i + 1]) && base::IsHexDigit(text[i + 2])) {
      escaped.push_back('%');
    } else if (charmap.Contains(c)) {
      escaped.push_back('%');
      escaped.push_back(IntToHex(c >> 4));
      escaped.push_back(IntToHex(c & 0xf));
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

// Contains nonzero when the corresponding character is unescapable for normal
// URLs. These characters are the ones that may change the parsing of a URL, so
// we don't want to unescape them sometimes. In many case we won't want to
// unescape spaces, but that is controlled by parameters to Unescape*.
//
// The basic rule is that we can't unescape anything that would changing parsing
// like # or ?. We also can't unescape &, =, or + since that could be part of a
// query and that could change the server's parsing of the query. Nor can we
// unescape \ since src/url/ will convert it to a /.
//
// Lastly, we can't unescape anything that doesn't have a canonical
// representation in a URL. This means that unescaping will change the URL, and
// you could get different behavior if you copy and paste the URL, or press
// enter in the URL bar. The list of characters that fall into this category
// are the ones labeled PASS (allow either escaped or unescaped) in the big
// lookup table at the top of url/url_canon_path.cc.  Also, characters
// that have CHAR_QUERY set in url/url_canon_internal.cc but are not
// allowed in query strings according to http://www.ietf.org/rfc/rfc3261.txt are
// not unescaped, to avoid turning a valid url according to spec into an
// invalid one.
const char kUrlUnescape[128] = {
//   NULL, control chars...
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
//  ' ' !  "  #  $  %  &  '  (  )  *  +  ,  -  .  /
     0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
//   0  1  2  3  4  5  6  7  8  9  :  ;  <  =  >  ?
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0,
//   @  A  B  C  D  E  F  G  H  I  J  K  L  M  N  O
     0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
//   P  Q  R  S  T  U  V  W  X  Y  Z  [  \  ]  ^  _
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
//   `  a  b  c  d  e  f  g  h  i  j  k  l  m  n  o
     0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
//   p  q  r  s  t  u  v  w  x  y  z  {  |  }  ~  <NBSP>
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0
};

// Attempts to unescape the sequence at |index| within |escaped_text|.  If
// successful, sets |value| to the unescaped value.  Returns whether
// unescaping succeeded.
bool UnescapeUnsignedByteAtIndex(base::StringPiece escaped_text,
                                 size_t index,
                                 unsigned char* value) {
  if ((index + 2) >= escaped_text.size())
    return false;
  if (escaped_text[index] != '%')
    return false;
  char most_sig_digit(escaped_text[index + 1]);
  char least_sig_digit(escaped_text[index + 2]);
  if (base::IsHexDigit(most_sig_digit) && base::IsHexDigit(least_sig_digit)) {
    *value = base::HexDigitToInt(most_sig_digit) * 16 +
             base::HexDigitToInt(least_sig_digit);
    return true;
  }
  return false;
}

// Attempts to unescape and decode a UTF-8-encoded percent-escaped character at
// the specified index. On success, returns true, sets |code_point_out| to be
// the character's code point and |unescaped_out| to be the unescaped UTF-8
// string. |unescaped_out| will always be 1/3rd the length of the substring of
// |escaped_text| that corresponds to the unescaped character.
bool UnescapeUTF8CharacterAtIndex(base::StringPiece escaped_text,
                                  size_t index,
                                  uint32_t* code_point_out,
                                  std::string* unescaped_out) {
  DCHECK(unescaped_out->empty());

  unsigned char bytes[CBU8_MAX_LENGTH];
  if (!UnescapeUnsignedByteAtIndex(escaped_text, index, &bytes[0]))
    return false;

  size_t num_bytes = 1;

  // If this is a lead byte, need to collect trail bytes as well.
  if (CBU8_IS_LEAD(bytes[0])) {
    // Look for the last trail byte of the UTF-8 character.  Give up once
    // reach max character length number of bytes, or hit an unescaped
    // character. No need to check length of escaped_text, as
    // UnescapeUnsignedByteAtIndex checks lengths.
    while (num_bytes < base::size(bytes) &&
           UnescapeUnsignedByteAtIndex(escaped_text, index + num_bytes * 3,
                                       &bytes[num_bytes]) &&
           CBU8_IS_TRAIL(bytes[num_bytes])) {
      ++num_bytes;
    }
  }

  int32_t char_index = 0;
  // Check if the unicode "character" that was just unescaped is valid.
  if (!base::ReadUnicodeCharacter(reinterpret_cast<char*>(bytes), num_bytes,
                                  &char_index, code_point_out)) {
    return false;
  }

  // It's possible that a prefix of |bytes| forms a valid UTF-8 character,
  // and the rest are not valid UTF-8, so need to update |num_bytes| based
  // on the result of ReadUnicodeCharacter().
  num_bytes = char_index + 1;
  *unescaped_out = std::string(reinterpret_cast<char*>(bytes), num_bytes);
  return true;
}

// This method takes a Unicode code point and returns true if it should be
// unescaped, based on |rules|.
bool ShouldUnescapeCodePoint(UnescapeRule::Type rules, uint32_t code_point) {
  // If this is an ASCII character, use the lookup table.
  if (code_point < 0x80) {
    return kUrlUnescape[code_point] ||
           // Allow some additional unescaping when flags are set.
           (code_point == ' ' && (rules & UnescapeRule::SPACES)) ||
           // Allow any of the prohibited but non-control characters when doing
           // "special" chars.
           ((code_point == '/' || code_point == '\\') &&
            (rules & UnescapeRule::PATH_SEPARATORS)) ||
           (code_point > ' ' && code_point != '/' && code_point != '\\' &&
            (rules & UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS));
  }

  // Compare the codepoint against a list of characters that can be used
  // to spoof other URLs.
  //
  // Can't use icu to make this cleaner, because Cronet cannot depend on
  // icu, and currently uses this file.
  // TODO(https://crbug.com/829873): Try to make this use icu, both to
  // protect against regressions as the Unicode stndard is updated and to
  // reduce the number of long lists of characters.
  // TODO(https://crbug.com/824715): Add default ignoreable and formatting
  // codepoints.
  return !(
      // Per http://tools.ietf.org/html/rfc3987#section-4.1, certain BiDi
      // control characters are not allowed to appear unescaped in URLs.
      code_point == 0x200E ||  // LEFT-TO-RIGHT MARK         (%E2%80%8E)
      code_point == 0x200F ||  // RIGHT-TO-LEFT MARK         (%E2%80%8F)
      code_point == 0x202A ||  // LEFT-TO-RIGHT EMBEDDING    (%E2%80%AA)
      code_point == 0x202B ||  // RIGHT-TO-LEFT EMBEDDING    (%E2%80%AB)
      code_point == 0x202C ||  // POP DIRECTIONAL FORMATTING (%E2%80%AC)
      code_point == 0x202D ||  // LEFT-TO-RIGHT OVERRIDE     (%E2%80%AD)
      code_point == 0x202E ||  // RIGHT-TO-LEFT OVERRIDE     (%E2%80%AE)

      // The Unicode Technical Report (TR9) as referenced by RFC 3987 above has
      // since added some new BiDi control characters that are not safe to
      // unescape. http://www.unicode.org/reports/tr9
      code_point == 0x061C ||  // ARABIC LETTER MARK         (%D8%9C)
      code_point == 0x2066 ||  // LEFT-TO-RIGHT ISOLATE      (%E2%81%A6)
      code_point == 0x2067 ||  // RIGHT-TO-LEFT ISOLATE      (%E2%81%A7)
      code_point == 0x2068 ||  // FIRST STRONG ISOLATE       (%E2%81%A8)
      code_point == 0x2069 ||  // POP DIRECTIONAL ISOLATE    (%E2%81%A9)

      // The following spoofable characters are also banned in unescaped URLs,
      // because they could be used to imitate parts of a web browser's UI.
      code_point == 0x1F50F ||  // LOCK WITH INK PEN    (%F0%9F%94%8F)
      code_point == 0x1F510 ||  // CLOSED LOCK WITH KEY (%F0%9F%94%90)
      code_point == 0x1F512 ||  // LOCK                 (%F0%9F%94%92)
      code_point == 0x1F513 ||  // OPEN LOCK            (%F0%9F%94%93)

      // Spaces are also banned, as they can be used to scroll text out of view.
      code_point == 0x0085 ||  // NEXT LINE                  (%C2%85)
      code_point == 0x00A0 ||  // NO-BREAK SPACE             (%C2%A0)
      code_point == 0x1680 ||  // OGHAM SPACE MARK           (%E1%9A%80)
      code_point == 0x2000 ||  // EN QUAD                    (%E2%80%80)
      code_point == 0x2001 ||  // EM QUAD                    (%E2%80%81)
      code_point == 0x2002 ||  // EN SPACE                   (%E2%80%82)
      code_point == 0x2003 ||  // EM SPACE                   (%E2%80%83)
      code_point == 0x2004 ||  // THREE-PER-EM SPACE         (%E2%80%84)
      code_point == 0x2005 ||  // FOUR-PER-EM SPACE          (%E2%80%85)
      code_point == 0x2006 ||  // SIX-PER-EM SPACE           (%E2%80%86)
      code_point == 0x2007 ||  // FIGURE SPACE               (%E2%80%87)
      code_point == 0x2008 ||  // PUNCTUATION SPACE          (%E2%80%88)
      code_point == 0x2009 ||  // THIN SPACE                 (%E2%80%89)
      code_point == 0x200A ||  // HAIR SPACE                 (%E2%80%8A)
      code_point == 0x2028 ||  // LINE SEPARATOR             (%E2%80%A8)
      code_point == 0x2029 ||  // PARAGRAPH SEPARATOR        (%E2%80%A9)
      code_point == 0x202F ||  // NARROW NO-BREAK SPACE      (%E2%80%AF)
      code_point == 0x205F ||  // MEDIUM MATHEMATICAL SPACE  (%E2%81%9F)
      code_point == 0x3000);   // IDEOGRAPHIC SPACE          (%E3%80%80)
}

// Unescapes |escaped_text| according to |rules|, returning the resulting
// string.  Fills in an |adjustments| parameter, if non-NULL, so it reflects
// the alterations done to the string that are not one-character-to-one-
// character.  The resulting |adjustments| will always be sorted by increasing
// offset.
std::string UnescapeURLWithAdjustmentsImpl(
    base::StringPiece escaped_text,
    UnescapeRule::Type rules,
    base::OffsetAdjuster::Adjustments* adjustments) {
  if (adjustments)
    adjustments->clear();
  // Do not unescape anything, return the |escaped_text| text.
  if (rules == UnescapeRule::NONE)
    return escaped_text.as_string();

  // The output of the unescaping is always smaller than the input, so we can
  // reserve the input size to make sure we have enough buffer and don't have
  // to allocate in the loop below.
  std::string result;
  result.reserve(escaped_text.length());

  // Locations of adjusted text.
  for (size_t i = 0, max = escaped_text.size(); i < max;) {
    // Try to unescape the character.
    uint32_t code_point;
    std::string unescaped;
    if (!UnescapeUTF8CharacterAtIndex(escaped_text, i, &code_point,
                                      &unescaped)) {
      // Check if the next character can be unescaped, but not as a valid UTF-8
      // character. In that case, just unescaped and write the non-sense
      // character.
      //
      // TODO(https://crbug.com/829868): Do not unescape illegal UTF-8
      // sequences.
      unsigned char non_utf8_byte;
      if (UnescapeUnsignedByteAtIndex(escaped_text, i, &non_utf8_byte)) {
        result.push_back(non_utf8_byte);
        if (adjustments)
          adjustments->push_back(base::OffsetAdjuster::Adjustment(i, 3, 1));
        i += 3;
        continue;
      }

      // Character is not escaped, so append as is, unless it's a '+' and
      // REPLACE_PLUS_WITH_SPACE is being applied.
      if (escaped_text[i] == '+' &&
          (rules & UnescapeRule::REPLACE_PLUS_WITH_SPACE)) {
        result.push_back(' ');
      } else {
        result.push_back(escaped_text[i]);
      }
      ++i;
      continue;
    }

    DCHECK(!unescaped.empty());

    if (!ShouldUnescapeCodePoint(rules, code_point)) {
      // If it's a valid UTF-8 character, but not safe to unescape, copy all
      // bytes directly.
      result.append(escaped_text.begin() + i,
                    escaped_text.begin() + i + 3 * unescaped.length());
      i += unescaped.length() * 3;
      continue;
    }

    // If the code point is allowed, and append the entire unescaped character.
    result.append(unescaped);
    if (adjustments) {
      for (size_t j = 0; j < unescaped.length(); ++j) {
        adjustments->push_back(
            base::OffsetAdjuster::Adjustment(i + j * 3, 3, 1));
      }
    }
    i += 3 * unescaped.length();
  }

  return result;
}

template <class str>
void AppendEscapedCharForHTMLImpl(typename str::value_type c, str* output) {
  static constexpr struct {
    char key;
    base::StringPiece replacement;
  } kCharsToEscape[] = {
    { '<', "&lt;" },
    { '>', "&gt;" },
    { '&', "&amp;" },
    { '"', "&quot;" },
    { '\'', "&#39;" },
  };
  for (const auto& char_to_escape : kCharsToEscape) {
    if (c == char_to_escape.key) {
      output->append(std::begin(char_to_escape.replacement),
                     std::end(char_to_escape.replacement));
      return;
    }
  }
  output->push_back(c);
}

template <class str>
str EscapeForHTMLImpl(base::BasicStringPiece<str> input) {
  str result;
  result.reserve(input.size());  // Optimize for no escaping.

  for (auto c : input) {
    AppendEscapedCharForHTMLImpl(c, &result);
  }

  return result;
}

// Everything except alphanumerics and !'()*-._~
// See RFC 2396 for the list of reserved characters.
static const Charmap kQueryCharmap = {{
  0xffffffffL, 0xfc00987dL, 0x78000001L, 0xb8000001L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};

// non-printable, non-7bit, and (including space)  "#%:<>?[\]^`{|}
static const Charmap kPathCharmap = {{
  0xffffffffL, 0xd400002dL, 0x78000000L, 0xb8000001L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};

#if defined(OS_MACOSX)
// non-printable, non-7bit, and (including space)  "#%<>[\]^`{|}
static const Charmap kNSURLCharmap = {{
  0xffffffffL, 0x5000002dL, 0x78000000L, 0xb8000001L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};
#endif  // defined(OS_MACOSX)

// non-printable, non-7bit, and (including space) ?>=<;+'&%$#"![\]^`{|}
static const Charmap kUrlEscape = {{
  0xffffffffL, 0xf80008fdL, 0x78000001L, 0xb8000001L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};

// non-7bit
static const Charmap kNonASCIICharmap = {{
  0x00000000L, 0x00000000L, 0x00000000L, 0x00000000L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};

// Everything except alphanumerics, the reserved characters(;/?:@&=+$,) and
// !'()*-._~#[]
static const Charmap kExternalHandlerCharmap = {{
  0xffffffffL, 0x50000025L, 0x50000000L, 0xb8000001L,
  0xffffffffL, 0xffffffffL, 0xffffffffL, 0xffffffffL
}};

}  // namespace

std::string EscapeQueryParamValue(base::StringPiece text, bool use_plus) {
  return Escape(text, kQueryCharmap, use_plus);
}

std::string EscapePath(base::StringPiece path) {
  return Escape(path, kPathCharmap, false);
}

#if defined(OS_MACOSX)
std::string EscapeNSURLPrecursor(base::StringPiece precursor) {
  return Escape(precursor, kNSURLCharmap, false, true);
}
#endif  // defined(OS_MACOSX)

std::string EscapeUrlEncodedData(base::StringPiece path, bool use_plus) {
  return Escape(path, kUrlEscape, use_plus);
}

std::string EscapeNonASCII(base::StringPiece input) {
  return Escape(input, kNonASCIICharmap, false);
}

std::string EscapeExternalHandlerValue(base::StringPiece text) {
  return Escape(text, kExternalHandlerCharmap, false, true);
}

void AppendEscapedCharForHTML(char c, std::string* output) {
  AppendEscapedCharForHTMLImpl(c, output);
}

std::string EscapeForHTML(base::StringPiece input) {
  return EscapeForHTMLImpl(input);
}

base::string16 EscapeForHTML(base::StringPiece16 input) {
  return EscapeForHTMLImpl(input);
}

std::string UnescapeURLComponent(base::StringPiece escaped_text,
                                 UnescapeRule::Type rules) {
  return UnescapeURLWithAdjustmentsImpl(escaped_text, rules, NULL);
}

base::string16 UnescapeAndDecodeUTF8URLComponent(base::StringPiece text,
                                                 UnescapeRule::Type rules) {
  return UnescapeAndDecodeUTF8URLComponentWithAdjustments(text, rules, NULL);
}

base::string16 UnescapeAndDecodeUTF8URLComponentWithAdjustments(
    base::StringPiece text,
    UnescapeRule::Type rules,
    base::OffsetAdjuster::Adjustments* adjustments) {
  base::string16 result;
  base::OffsetAdjuster::Adjustments unescape_adjustments;
  std::string unescaped_url(UnescapeURLWithAdjustmentsImpl(
      text, rules, &unescape_adjustments));
  if (base::UTF8ToUTF16WithAdjustments(unescaped_url.data(),
                                       unescaped_url.length(),
                                       &result, adjustments)) {
    // Character set looks like it's valid.
    if (adjustments) {
      base::OffsetAdjuster::MergeSequentialAdjustments(unescape_adjustments,
                                                       adjustments);
    }
    return result;
  }
  // Character set is not valid.  Return the escaped version.
  return base::UTF8ToUTF16WithAdjustments(text, adjustments);
}

std::string UnescapeBinaryURLComponent(base::StringPiece escaped_text,
                                       UnescapeRule::Type rules) {
  // Only NORMAL and REPLACE_PLUS_WITH_SPACE are supported.
  DCHECK(rules != UnescapeRule::NONE);
  DCHECK(!(rules &
           ~(UnescapeRule::NORMAL | UnescapeRule::REPLACE_PLUS_WITH_SPACE)));

  // The output of the unescaping is always smaller than the input, so we can
  // reserve the input size to make sure we have enough buffer and don't have
  // to allocate in the loop below.
  std::string result;
  result.reserve(escaped_text.length());

  for (size_t i = 0, max = escaped_text.size(); i < max;) {
    unsigned char byte;
    // UnescapeUnsignedByteAtIndex does bounds checking, so this is always safe
    // to call.
    if (UnescapeUnsignedByteAtIndex(escaped_text, i, &byte)) {
      result.push_back(byte);
      i += 3;
      continue;
    }

    if ((rules & UnescapeRule::REPLACE_PLUS_WITH_SPACE) &&
        escaped_text[i] == '+') {
      result.push_back(' ');
      ++i;
      continue;
    }

    result.push_back(escaped_text[i]);
    ++i;
  }

  return result;
}

base::string16 UnescapeForHTML(base::StringPiece16 input) {
  static const struct {
    const char* ampersand_code;
    const char replacement;
  } kEscapeToChars[] = {
    { "&lt;", '<' },
    { "&gt;", '>' },
    { "&amp;", '&' },
    { "&quot;", '"' },
    { "&#39;", '\''},
  };

  if (input.find(base::ASCIIToUTF16("&")) == std::string::npos)
    return input.as_string();

  base::string16 ampersand_chars[base::size(kEscapeToChars)];
  base::string16 text = input.as_string();
  for (base::string16::iterator iter = text.begin();
       iter != text.end(); ++iter) {
    if (*iter == '&') {
      // Potential ampersand encode char.
      size_t index = iter - text.begin();
      for (size_t i = 0; i < base::size(kEscapeToChars); i++) {
        if (ampersand_chars[i].empty()) {
          ampersand_chars[i] =
              base::ASCIIToUTF16(kEscapeToChars[i].ampersand_code);
        }
        if (text.find(ampersand_chars[i], index) == index) {
          text.replace(iter, iter + ampersand_chars[i].length(),
                       1, kEscapeToChars[i].replacement);
          break;
        }
      }
    }
  }
  return text;
}

}  // namespace net
