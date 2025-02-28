// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/about_flags.h"

#include <stddef.h>

#include <map>
#include <set>
#include <string>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/format_macros.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/flags_ui/feature_entry.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/libxml/chromium/libxml_utils.h"

namespace about_flags {

namespace {

typedef base::HistogramBase::Sample Sample;
typedef std::map<std::string, Sample> SwitchToIdMap;

// This is a helper function to the ReadEnumFromHistogramsXml().
// Extracts single enum (with integer values) from histograms.xml.
// Expects |reader| to point at given enum.
// Returns map { value => label }.
// Returns empty map on error.
std::map<Sample, std::string> ParseEnumFromHistogramsXml(
    const std::string& enum_name,
    XmlReader* reader) {
  int entries_index = -1;

  std::map<Sample, std::string> result;
  bool success = true;

  while (true) {
    const std::string node_name = reader->NodeName();
    if (node_name == "enum" && reader->IsClosingElement())
      break;

    if (node_name == "int") {
      ++entries_index;
      std::string value_str;
      std::string label;
      const bool has_value = reader->NodeAttribute("value", &value_str);
      const bool has_label = reader->NodeAttribute("label", &label);
      if (!has_value) {
        ADD_FAILURE() << "Bad " << enum_name << " enum entry (at index "
                      << entries_index << ", label='" << label
                      << "'): No 'value' attribute.";
        success = false;
      }
      if (!has_label) {
        ADD_FAILURE() << "Bad " << enum_name << " enum entry (at index "
                      << entries_index << ", value_str='" << value_str
                      << "'): No 'label' attribute.";
        success = false;
      }

      Sample value;
      if (has_value && !base::StringToInt(value_str, &value)) {
        ADD_FAILURE() << "Bad " << enum_name << " enum entry (at index "
                      << entries_index << ", label='" << label
                      << "', value_str='" << value_str
                      << "'): 'value' attribute is not integer.";
        success = false;
      }
      if (result.count(value)) {
        ADD_FAILURE() << "Bad " << enum_name << " enum entry (at index "
                      << entries_index << ", label='" << label
                      << "', value_str='" << value_str
                      << "'): duplicate value '" << value_str
                      << "' found in enum. The previous one has label='"
                      << result[value] << "'.";
        success = false;
      }
      if (success) {
        result[value] = label;
      }
    }
    // All enum entries are on the same level, so it is enough to iterate
    // until possible.
    reader->Next();
  }
  return (success ? result : std::map<Sample, std::string>());
}

// Find and read given enum (with integer values) from histograms.xml.
// |enum_name| - enum name.
// |histograms_xml| - must be loaded histograms.xml file.
//
// Returns map { value => label } so that:
//   <int value="9" label="enable-pinch-virtual-viewport"/>
// becomes:
//   { 9 => "enable-pinch-virtual-viewport" }
// Returns empty map on error.
std::map<Sample, std::string> ReadEnumFromHistogramsXml(
    const std::string& enum_name,
    XmlReader* histograms_xml) {
  std::map<Sample, std::string> login_custom_flags;

  // Implement simple depth first search.
  while (true) {
    const std::string node_name = histograms_xml->NodeName();
    if (node_name == "enum") {
      std::string name;
      if (histograms_xml->NodeAttribute("name", &name) && name == enum_name) {
        if (!login_custom_flags.empty()) {
          EXPECT_TRUE(login_custom_flags.empty())
              << "Duplicate enum '" << enum_name << "' found in histograms.xml";
          return std::map<Sample, std::string>();
        }

        const bool got_into_enum = histograms_xml->Read();
        if (got_into_enum) {
          login_custom_flags =
              ParseEnumFromHistogramsXml(enum_name, histograms_xml);
          EXPECT_FALSE(login_custom_flags.empty())
              << "Bad enum '" << enum_name
              << "' found in histograms.xml (format error).";
        } else {
          EXPECT_TRUE(got_into_enum)
              << "Bad enum '" << enum_name
              << "' (looks empty) found in histograms.xml.";
        }
        if (login_custom_flags.empty())
          return std::map<Sample, std::string>();
      }
    }
    // Go deeper if possible (stops at the closing tag of the deepest node).
    if (histograms_xml->Read())
      continue;

    // Try next node on the same level (skips closing tag).
    if (histograms_xml->Next())
      continue;

    // Go up until next node on the same level exists.
    while (histograms_xml->Depth() && !histograms_xml->SkipToElement()) {
    }

    // Reached top. histograms.xml consists of the single top level node
    // 'histogram-configuration', so this is the end.
    if (!histograms_xml->Depth())
      break;
  }
  EXPECT_FALSE(login_custom_flags.empty())
      << "Enum '" << enum_name << "' is not found in histograms.xml.";
  return login_custom_flags;
}

std::string FilePathStringTypeToString(const base::FilePath::StringType& path) {
#if defined(OS_WIN)
  return base::UTF16ToUTF8(path);
#else
  return path;
#endif
}

// Get all associated switches corresponding to defined about_flags.cc entries.
std::set<std::string> GetAllSwitchesAndFeaturesForTesting() {
  std::set<std::string> result;

  size_t num_entries = 0;
  const flags_ui::FeatureEntry* entries =
      testing::GetFeatureEntries(&num_entries);

  for (size_t i = 0; i < num_entries; ++i) {
    const flags_ui::FeatureEntry& entry = entries[i];
    switch (entry.type) {
      case flags_ui::FeatureEntry::SINGLE_VALUE:
      case flags_ui::FeatureEntry::SINGLE_DISABLE_VALUE:
        result.insert(entry.command_line_switch);
        break;
      case flags_ui::FeatureEntry::MULTI_VALUE:
        for (int j = 0; j < entry.num_options; ++j) {
          result.insert(entry.ChoiceForOption(j).command_line_switch);
        }
        break;
      case flags_ui::FeatureEntry::ENABLE_DISABLE_VALUE:
        result.insert(entry.command_line_switch);
        result.insert(entry.disable_command_line_switch);
        break;
      case flags_ui::FeatureEntry::FEATURE_VALUE:
      case flags_ui::FeatureEntry::FEATURE_WITH_PARAMS_VALUE:
        result.insert(std::string(entry.feature->name) + ":enabled");
        result.insert(std::string(entry.feature->name) + ":disabled");
        break;
    }
  }
  return result;
}

}  // anonymous namespace

// Makes sure there are no separators in any of the entry names.
TEST(AboutFlagsTest, NoSeparators) {
  size_t count;
  const flags_ui::FeatureEntry* entries = testing::GetFeatureEntries(&count);
  for (size_t i = 0; i < count; ++i) {
    std::string name = entries[i].internal_name;
    EXPECT_EQ(std::string::npos, name.find(flags_ui::testing::kMultiSeparator))
        << i;
  }
}

class AboutFlagsHistogramTest : public ::testing::Test {
 protected:
  // This is a helper function to check that all IDs in enum LoginCustomFlags in
  // histograms.xml are unique.
  void SetSwitchToHistogramIdMapping(const std::string& switch_name,
                                     const Sample switch_histogram_id,
                                     std::map<std::string, Sample>* out_map) {
    const std::pair<std::map<std::string, Sample>::iterator, bool> status =
        out_map->insert(std::make_pair(switch_name, switch_histogram_id));
    if (!status.second) {
      EXPECT_TRUE(status.first->second == switch_histogram_id)
          << "Duplicate switch '" << switch_name
          << "' found in enum 'LoginCustomFlags' in histograms.xml.";
    }
  }

  // This method generates a hint for the user for what string should be added
  // to the enum LoginCustomFlags to make in consistent.
  std::string GetHistogramEnumEntryText(const std::string& switch_name,
                                        Sample value) {
    return base::StringPrintf(
        "<int value=\"%d\" label=\"%s\"/>", value, switch_name.c_str());
  }
};

TEST_F(AboutFlagsHistogramTest, CheckHistograms) {
  base::FilePath histograms_xml_file_path;
  ASSERT_TRUE(
      base::PathService::Get(base::DIR_SOURCE_ROOT, &histograms_xml_file_path));
  histograms_xml_file_path = histograms_xml_file_path.AppendASCII("tools")
                                 .AppendASCII("metrics")
                                 .AppendASCII("histograms")
                                 .AppendASCII("enums.xml");

  XmlReader histograms_xml;
  ASSERT_TRUE(histograms_xml.LoadFile(
      FilePathStringTypeToString(histograms_xml_file_path.value())));
  std::map<Sample, std::string> login_custom_flags =
      ReadEnumFromHistogramsXml("LoginCustomFlags", &histograms_xml);
  ASSERT_TRUE(login_custom_flags.size())
      << "Error reading enum 'LoginCustomFlags' from enums.xml.";

  // Build reverse map {switch_name => id} from login_custom_flags.
  SwitchToIdMap histograms_xml_switches_ids;

  EXPECT_TRUE(login_custom_flags.count(testing::kBadSwitchFormatHistogramId))
      << "Entry for UMA ID of incorrect command-line flag is not found in "
         "enums.xml enum LoginCustomFlags. "
         "Consider adding entry:\n"
      << "  " << GetHistogramEnumEntryText("BAD_FLAG_FORMAT", 0);
  // Check that all LoginCustomFlags entries have correct values.
  for (const auto& entry : login_custom_flags) {
    if (entry.first == testing::kBadSwitchFormatHistogramId) {
      // Add error value with empty name.
      SetSwitchToHistogramIdMapping(std::string(), entry.first,
                                    &histograms_xml_switches_ids);
      continue;
    }
    const Sample uma_id = GetSwitchUMAId(entry.second);
    EXPECT_EQ(uma_id, entry.first)
        << "enums.xml enum LoginCustomFlags "
           "entry '"
        << entry.second << "' has incorrect value=" << entry.first << ", but "
        << uma_id << " is expected. Consider changing entry to:\n"
        << "  " << GetHistogramEnumEntryText(entry.second, uma_id);
    SetSwitchToHistogramIdMapping(entry.second, entry.first,
                                  &histograms_xml_switches_ids);
  }

  // Check that all flags in about_flags.cc have entries in login_custom_flags.
  std::set<std::string> all_flags = GetAllSwitchesAndFeaturesForTesting();
  for (const std::string& flag : all_flags) {
    // Skip empty placeholders.
    if (flag.empty())
      continue;
    const Sample uma_id = GetSwitchUMAId(flag);
    EXPECT_NE(testing::kBadSwitchFormatHistogramId, uma_id)
        << "Command-line switch '" << flag
        << "' from about_flags.cc has UMA ID equal to reserved value "
           "kBadSwitchFormatHistogramId="
        << testing::kBadSwitchFormatHistogramId
        << ". Please modify switch name.";
    SwitchToIdMap::iterator enum_entry =
        histograms_xml_switches_ids.lower_bound(flag);

    // Ignore case here when switch ID is incorrect - it has already been
    // reported in the previous loop.
    EXPECT_TRUE(enum_entry != histograms_xml_switches_ids.end() &&
                enum_entry->first == flag)
        << "enums.xml enum LoginCustomFlags doesn't contain switch '" << flag
        << "' (value=" << uma_id << " expected). Consider adding entry:\n"
        << "  " << GetHistogramEnumEntryText(flag, uma_id);
  }
}

}  // namespace about_flags
