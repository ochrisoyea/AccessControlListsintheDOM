// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/display/display_prefs.h"

#include <stddef.h>

#include "ash/public/cpp/ash_pref_names.h"
#include "ash/public/cpp/config.h"
#include "ash/session/session_controller.h"
#include "ash/shell.h"
#include "base/command_line.h"
#include "base/stl_util.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/sys_info.h"
#include "base/values.h"
#include "chromeos/chromeos_switches.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "third_party/cros_system_api/dbus/service_constants.h"
#include "ui/display/manager/display_layout_store.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/manager/display_manager_utilities.h"
#include "ui/display/manager/json_converter.h"
#include "ui/display/types/display_constants.h"
#include "ui/gfx/geometry/insets.h"
#include "url/url_canon.h"
#include "url/url_util.h"

using chromeos::DisplayPowerState;

namespace ash {

class DisplayPrefs::LocalState {
 public:
  explicit LocalState(std::unique_ptr<base::Value> initial_prefs)
      : initial_prefs_(std::move(initial_prefs)) {}
  ~LocalState() = default;

  bool has_initial_prefs() const { return !!initial_prefs_.get(); }
  void set_pref_service(PrefService* pref_service) {
    pref_service_ = pref_service;
  }
  PrefService* pref_service() { return pref_service_; }

  const base::Value* Get(const std::string& path) const {
    if (pref_service_)
      return pref_service_->Get(path);
    CHECK(initial_prefs_);
    return initial_prefs_->FindKey(path);
  }

 private:
  std::unique_ptr<base::Value> initial_prefs_;
  class PrefService* pref_service_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(LocalState);
};

namespace {

constexpr char kInsetsTopKey[] = "insets_top";
constexpr char kInsetsLeftKey[] = "insets_left";
constexpr char kInsetsBottomKey[] = "insets_bottom";
constexpr char kInsetsRightKey[] = "insets_right";

constexpr char kTouchCalibrationWidth[] = "touch_calibration_width";
constexpr char kTouchCalibrationHeight[] = "touch_calibration_height";
constexpr char kTouchCalibrationPointPairs[] = "touch_calibration_point_pairs";

constexpr char kTouchAssociationTimestamp[] = "touch_association_timestamp";
constexpr char kTouchAssociationCalibrationData[] =
    "touch_association_calibration_data";

constexpr char kTouchDeviceIdentifier[] = "touch_device_identifer";
constexpr char kPortAssociationDisplayId[] = "port_association_display_id";

constexpr char kMirroringSourceId[] = "mirroring_source_id";
constexpr char kMirroringDestinationIds[] = "mirroring_destination_ids";

constexpr char kDisplayZoom[] = "display_zoom_factor";

constexpr char kDisplayPowerAllOn[] = "all_on";
constexpr char kDisplayPowerInternalOffExternalOn[] =
    "internal_off_external_on";
constexpr char kDisplayPowerInternalOnExternalOff[] =
    "internal_on_external_off";

// This kind of boilerplates should be done by base::JSONValueConverter but it
// doesn't support classes like gfx::Insets for now.
// TODO(mukai): fix base::JSONValueConverter and use it here.
bool ValueToInsets(const base::DictionaryValue& value, gfx::Insets* insets) {
  DCHECK(insets);
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
  if (value.GetInteger(kInsetsTopKey, &top) &&
      value.GetInteger(kInsetsLeftKey, &left) &&
      value.GetInteger(kInsetsBottomKey, &bottom) &&
      value.GetInteger(kInsetsRightKey, &right)) {
    insets->Set(top, left, bottom, right);
    return true;
  }
  return false;
}

void InsetsToValue(const gfx::Insets& insets, base::DictionaryValue* value) {
  DCHECK(value);
  value->SetInteger(kInsetsTopKey, insets.top());
  value->SetInteger(kInsetsLeftKey, insets.left());
  value->SetInteger(kInsetsBottomKey, insets.bottom());
  value->SetInteger(kInsetsRightKey, insets.right());
}

// Unmarshalls the string containing CalibrationPointPairQuad and populates
// |point_pair_quad| with the unmarshalled data.
bool ParseTouchCalibrationStringValue(
    const std::string& str,
    display::TouchCalibrationData::CalibrationPointPairQuad* point_pair_quad) {
  DCHECK(point_pair_quad);
  int x = 0, y = 0;
  std::vector<std::string> parts = base::SplitString(
      str, " ", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  size_t total = point_pair_quad->size();
  gfx::Point display_point, touch_point;
  for (std::size_t row = 0; row < total; row++) {
    if (!base::StringToInt(parts[row * total], &x) ||
        !base::StringToInt(parts[row * total + 1], &y)) {
      return false;
    }
    display_point.SetPoint(x, y);

    if (!base::StringToInt(parts[row * total + 2], &x) ||
        !base::StringToInt(parts[row * total + 3], &y)) {
      return false;
    }
    touch_point.SetPoint(x, y);

    (*point_pair_quad)[row] = std::make_pair(display_point, touch_point);
  }
  return true;
}

// Retrieves touch calibration associated data from the dictionary and stores
// it in an instance of TouchCalibrationData struct.
bool ValueToTouchData(const base::DictionaryValue& value,
                      display::TouchCalibrationData* touch_calibration_data) {
  display::TouchCalibrationData::CalibrationPointPairQuad* point_pair_quad =
      &(touch_calibration_data->point_pairs);

  std::string str;
  if (!value.GetString(kTouchCalibrationPointPairs, &str))
    return false;

  if (!ParseTouchCalibrationStringValue(str, point_pair_quad))
    return false;

  int width, height;
  if (!value.GetInteger(kTouchCalibrationWidth, &width) ||
      !value.GetInteger(kTouchCalibrationHeight, &height)) {
    return false;
  }
  touch_calibration_data->bounds = gfx::Size(width, height);
  return true;
}

// Stores the touch calibration data into the dictionary.
void TouchDataToValue(
    const display::TouchCalibrationData& touch_calibration_data,
    base::DictionaryValue* value) {
  DCHECK(value);
  std::string str;
  for (std::size_t row = 0; row < touch_calibration_data.point_pairs.size();
       row++) {
    str +=
        base::IntToString(touch_calibration_data.point_pairs[row].first.x()) +
        " ";
    str +=
        base::IntToString(touch_calibration_data.point_pairs[row].first.y()) +
        " ";
    str +=
        base::IntToString(touch_calibration_data.point_pairs[row].second.x()) +
        " ";
    str +=
        base::IntToString(touch_calibration_data.point_pairs[row].second.y());
    if (row != touch_calibration_data.point_pairs.size() - 1)
      str += " ";
  }
  value->SetString(kTouchCalibrationPointPairs, str);
  value->SetInteger(kTouchCalibrationWidth,
                    touch_calibration_data.bounds.width());
  value->SetInteger(kTouchCalibrationHeight,
                    touch_calibration_data.bounds.height());
}

display::DisplayManager* GetDisplayManager() {
  return Shell::Get()->display_manager();
}

// Returns true id the current user can write display preferences to
// Local State.
bool UserCanSaveDisplayPreference() {
  SessionController* controller = Shell::Get()->session_controller();
  auto user_type = controller->GetUserType();
  if (!user_type)
    return false;
  return *user_type == user_manager::USER_TYPE_REGULAR ||
         *user_type == user_manager::USER_TYPE_CHILD ||
         *user_type == user_manager::USER_TYPE_SUPERVISED ||
         *user_type == user_manager::USER_TYPE_KIOSK_APP;
}

void LoadDisplayLayouts(DisplayPrefs::LocalState* local_state) {
  display::DisplayLayoutStore* layout_store =
      GetDisplayManager()->layout_store();

  const base::Value* layouts = local_state->Get(prefs::kSecondaryDisplays);
  for (const auto& it : layouts->DictItems()) {
    std::unique_ptr<display::DisplayLayout> layout(new display::DisplayLayout);
    if (!display::JsonToDisplayLayout(it.second, layout.get())) {
      LOG(WARNING) << "Invalid preference value for " << it.first;
      continue;
    }

    if (it.first.find(",") != std::string::npos) {
      std::vector<std::string> ids_str = base::SplitString(
          it.first, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
      std::vector<int64_t> ids;
      for (std::string id_str : ids_str) {
        int64_t id;
        if (!base::StringToInt64(id_str, &id))
          continue;
        ids.push_back(id);
      }
      display::DisplayIdList list =
          display::GenerateDisplayIdList(ids.begin(), ids.end());
      layout_store->RegisterLayoutForDisplayIdList(list, std::move(layout));
    }
  }
}

void LoadDisplayProperties(DisplayPrefs::LocalState* local_state) {
  const base::Value* properties = local_state->Get(prefs::kDisplayProperties);
  for (const auto& it : properties->DictItems()) {
    const base::DictionaryValue* dict_value = nullptr;
    if (!it.second.GetAsDictionary(&dict_value) || dict_value == nullptr)
      continue;
    int64_t id = display::kInvalidDisplayId;
    if (!base::StringToInt64(it.first, &id) ||
        id == display::kInvalidDisplayId) {
      continue;
    }
    display::Display::Rotation rotation = display::Display::ROTATE_0;
    float ui_scale = 1.0f;
    const gfx::Insets* insets_to_set = nullptr;

    int rotation_value = 0;
    if (dict_value->GetInteger("rotation", &rotation_value)) {
      rotation = static_cast<display::Display::Rotation>(rotation_value);
    }
    int ui_scale_value = 0;
    if (dict_value->GetInteger("ui-scale", &ui_scale_value))
      ui_scale = static_cast<float>(ui_scale_value) / 1000.0f;

    int width = 0, height = 0;
    dict_value->GetInteger("width", &width);
    dict_value->GetInteger("height", &height);
    gfx::Size resolution_in_pixels(width, height);

    float device_scale_factor = 1.0;
    int dsf_value = 0;
    if (dict_value->GetInteger("device-scale-factor", &dsf_value))
      device_scale_factor = static_cast<float>(dsf_value) / 1000.0f;

    gfx::Insets insets;
    if (ValueToInsets(*dict_value, &insets))
      insets_to_set = &insets;

    double display_zoom = 1.0;
    dict_value->GetDouble(kDisplayZoom, &display_zoom);

    GetDisplayManager()->RegisterDisplayProperty(
        id, rotation, ui_scale, insets_to_set, resolution_in_pixels,
        device_scale_factor, display_zoom);
  }
}

void LoadDisplayRotationState(DisplayPrefs::LocalState* local_state) {
  const base::Value* properties = local_state->Get(prefs::kDisplayRotationLock);
  DCHECK(properties->is_dict());
  const base::Value* rotation_lock =
      properties->FindKeyOfType("lock", base::Value::Type::BOOLEAN);
  if (!rotation_lock)
    return;

  const base::Value* rotation =
      properties->FindKeyOfType("orientation", base::Value::Type::INTEGER);
  if (!rotation)
    return;

  GetDisplayManager()->RegisterDisplayRotationProperties(
      rotation_lock->GetBool(),
      static_cast<display::Display::Rotation>(rotation->GetInt()));
}

void LoadDisplayTouchAssociations(DisplayPrefs::LocalState* local_state) {
  const base::Value* properties =
      local_state->Get(prefs::kDisplayTouchAssociations);
  DCHECK(properties->is_dict());

  display::TouchDeviceManager::TouchAssociationMap touch_associations;
  for (const auto& item : properties->DictItems()) {
    uint32_t identifier_raw;
    if (!base::StringToUint(item.first, &identifier_raw))
      continue;
    display::TouchDeviceIdentifier identifier(identifier_raw);
    touch_associations.emplace(
        identifier, display::TouchDeviceManager::AssociationInfoMap());
    if (!item.second.is_dict())
      continue;
    for (const auto& association_info_item : item.second.DictItems()) {
      display::TouchDeviceManager::TouchAssociationInfo info;
      int64_t display_id;
      if (!base::StringToInt64(association_info_item.first, &display_id))
        continue;
      auto* value =
          association_info_item.second.FindKey(kTouchAssociationTimestamp);
      if (!value->is_double())
        continue;
      info.timestamp = base::Time().FromDoubleT(value->GetDouble());

      value = association_info_item.second.FindKey(
          kTouchAssociationCalibrationData);
      if (!value->is_dict())
        continue;
      const base::DictionaryValue* calibration_data_dict = nullptr;
      if (!value->GetAsDictionary(&calibration_data_dict))
        continue;
      ValueToTouchData(*calibration_data_dict, &info.calibration_data);
      touch_associations.at(identifier).emplace(display_id, info);
    }
  }

  // Retrieve all the legacy format identifiers. This should be removed after
  // a couple of milestones when everything is stable.
  const display::TouchDeviceIdentifier& fallback_identifier =
      display::TouchDeviceIdentifier::GetFallbackTouchDeviceIdentifier();
  properties = local_state->Get(prefs::kDisplayProperties);
  for (const auto& it : properties->DictItems()) {
    const base::DictionaryValue* dict_value = nullptr;
    if (!it.second.GetAsDictionary(&dict_value) || dict_value == nullptr)
      continue;
    int64_t id = display::kInvalidDisplayId;
    if (!base::StringToInt64(it.first, &id) ||
        id == display::kInvalidDisplayId) {
      continue;
    }
    display::TouchCalibrationData calibration_data;
    display::TouchCalibrationData* calibration_data_to_set = nullptr;
    if (ValueToTouchData(*dict_value, &calibration_data))
      calibration_data_to_set = &calibration_data;

    if (calibration_data_to_set) {
      if (!base::ContainsKey(touch_associations, fallback_identifier)) {
        touch_associations.emplace(
            fallback_identifier,
            display::TouchDeviceManager::AssociationInfoMap());
      }
      display::TouchDeviceManager::TouchAssociationInfo info;
      info.calibration_data = *calibration_data_to_set;
      touch_associations.at(fallback_identifier).emplace(id, info);
    }
  }

  // Retrieve port association information.
  properties = local_state->Get(prefs::kDisplayTouchPortAssociations);
  display::TouchDeviceManager::PortAssociationMap port_associations;
  for (const auto& item : properties->DictItems()) {
    // Retrieve the secondary id that identifies the port.
    uint32_t secondary_id_raw;
    if (!base::StringToUint(item.first, &secondary_id_raw))
      continue;

    if (!item.second.is_dict())
      continue;

    // Retrieve the touch device identifier that identifies the touch device.
    auto* value = item.second.FindKey(kTouchDeviceIdentifier);
    if (!value->is_string())
      continue;
    uint32_t identifier_raw;
    if (!base::StringToUint(value->GetString(), &identifier_raw))
      continue;

    // Retrieve the display that the touch device identified by |identifier_raw|
    // was associated with.
    value = item.second.FindKey(kPortAssociationDisplayId);
    if (!value->is_string())
      continue;
    int64_t display_id;
    if (!base::StringToInt64(value->GetString(), &display_id))
      continue;

    port_associations.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(identifier_raw, secondary_id_raw),
        std::forward_as_tuple(display_id));
  }

  GetDisplayManager()->touch_device_manager()->RegisterTouchAssociations(
      touch_associations, port_associations);
}

// Loads mirror info for each external display, the info will later be used to
// restore mirror mode.
void LoadExternalDisplayMirrorInfo(DisplayPrefs::LocalState* local_state) {
  const base::Value* pref_data =
      local_state->Get(prefs::kExternalDisplayMirrorInfo);
  std::set<int64_t> external_display_mirror_info;
  for (const auto& it : pref_data->GetList()) {
    std::string display_id_str;
    if (!it.GetAsString(&display_id_str))
      continue;

    int64_t display_id;
    if (!base::StringToInt64(display_id_str, &display_id))
      continue;

    external_display_mirror_info.emplace(display_id);
  }
  GetDisplayManager()->set_external_display_mirror_info(
      external_display_mirror_info);
}

// Loads mixed mirror mode parameters which will later be used to restore mixed
// mirror mode. Return false if the parameters fail to be loaded.
void LoadDisplayMixedMirrorModeParams(DisplayPrefs::LocalState* local_state) {
  const base::Value* pref_data =
      local_state->Get(prefs::kDisplayMixedMirrorModeParams);

  // This function is called once for system (re)start, so the parameters should
  // be empty.
  DCHECK(!GetDisplayManager()->mixed_mirror_mode_params());

  auto* mirroring_source_id_value = pref_data->FindKey(kMirroringSourceId);
  if (!mirroring_source_id_value)
    return;

  DCHECK(mirroring_source_id_value->is_string());
  int64_t mirroring_source_id;
  if (!base::StringToInt64(mirroring_source_id_value->GetString(),
                           &mirroring_source_id)) {
    return;
  }

  auto* mirroring_destination_ids_value =
      pref_data->FindKey(kMirroringDestinationIds);
  if (!mirroring_destination_ids_value)
    return;

  DCHECK(mirroring_destination_ids_value->is_list());
  display::DisplayIdList mirroring_destination_ids;
  for (const auto& entry : mirroring_destination_ids_value->GetList()) {
    DCHECK(entry.is_string());
    int64_t id;
    if (!base::StringToInt64(entry.GetString(), &id))
      return;
    mirroring_destination_ids.emplace_back(id);
  }

  GetDisplayManager()->set_mixed_mirror_mode_params(
      base::Optional<display::MixedMirrorModeParams>(
          base::in_place, mirroring_source_id, mirroring_destination_ids));
}

void StoreDisplayLayoutPref(PrefService* pref_service,
                            const display::DisplayIdList& list,
                            const display::DisplayLayout& display_layout) {
  DCHECK(display::DisplayLayout::Validate(list, display_layout));
  std::string name = display::DisplayIdListToString(list);

  DictionaryPrefUpdate update(pref_service, prefs::kSecondaryDisplays);
  base::DictionaryValue* pref_data = update.Get();
  std::unique_ptr<base::Value> layout_value(new base::DictionaryValue());
  if (pref_data->HasKey(name)) {
    base::Value* value = nullptr;
    if (pref_data->Get(name, &value) && value != nullptr)
      layout_value.reset(value->DeepCopy());
  }
  if (display::DisplayLayoutToJson(display_layout, layout_value.get()))
    pref_data->Set(name, std::move(layout_value));
}

void StoreCurrentDisplayLayoutPrefs(PrefService* pref_service) {
  display::DisplayManager* display_manager = GetDisplayManager();
  if (!UserCanSaveDisplayPreference() ||
      display_manager->num_connected_displays() < 2) {
    return;
  }

  display::DisplayIdList list = display_manager->GetCurrentDisplayIdList();
  const display::DisplayLayout& display_layout =
      display_manager->layout_store()->GetRegisteredDisplayLayout(list);

  if (!display::DisplayLayout::Validate(list, display_layout)) {
    // We should never apply an invalid layout, if we do, it persists and the
    // user has no way of fixing it except by deleting the local state.
    LOG(ERROR) << "Attempting to store an invalid display layout in the local"
               << " state. Skipping.";
    return;
  }

  StoreDisplayLayoutPref(pref_service, list, display_layout);
}

void StoreCurrentDisplayProperties(PrefService* pref_service) {
  display::DisplayManager* display_manager = GetDisplayManager();

  DictionaryPrefUpdate update(pref_service, prefs::kDisplayProperties);
  base::DictionaryValue* pref_data = update.Get();

  // Pre-process data related to legacy touch calibration to opitmize lookup.
  const display::TouchDeviceIdentifier& fallback_identifier =
      display::TouchDeviceIdentifier::GetFallbackTouchDeviceIdentifier();
  display::TouchDeviceManager::AssociationInfoMap legacy_data_map;
  if (base::ContainsKey(
          display_manager->touch_device_manager()->touch_associations(),
          fallback_identifier)) {
    legacy_data_map =
        display_manager->touch_device_manager()->touch_associations().at(
            fallback_identifier);
  }

  size_t num = display_manager->GetNumDisplays();
  for (size_t i = 0; i < num; ++i) {
    const display::Display& display = display_manager->GetDisplayAt(i);
    int64_t id = display.id();
    display::ManagedDisplayInfo info = display_manager->GetDisplayInfo(id);

    std::unique_ptr<base::DictionaryValue> property_value(
        new base::DictionaryValue());
    // Don't save the display preference in unified mode because its
    // size and modes can change depending on the combination of displays.
    if (display_manager->IsInUnifiedMode())
      continue;
    property_value->SetInteger("rotation",
                               static_cast<int>(info.GetRotation(
                                   display::Display::RotationSource::USER)));
    property_value->SetInteger(
        "ui-scale", static_cast<int>(info.configured_ui_scale() * 1000));

    display::ManagedDisplayMode mode;
    if (!display.IsInternal() &&
        display_manager->GetSelectedModeForDisplayId(id, &mode) &&
        !mode.native()) {
      property_value->SetInteger("width", mode.size().width());
      property_value->SetInteger("height", mode.size().height());
      property_value->SetInteger(
          "device-scale-factor",
          static_cast<int>(mode.device_scale_factor() * 1000));
    }
    if (!info.overscan_insets_in_dip().IsEmpty())
      InsetsToValue(info.overscan_insets_in_dip(), property_value.get());

    // Store the legacy format touch calibration data. This can be removed after
    // a couple of milestones when every device has migrated to the new format.
    if (legacy_data_map.size() && base::ContainsKey(legacy_data_map, id)) {
      TouchDataToValue(legacy_data_map.at(id).calibration_data,
                       property_value.get());
    }

    property_value->SetDouble(kDisplayZoom, info.zoom_factor());

    pref_data->Set(base::Int64ToString(id), std::move(property_value));
  }
}

bool GetDisplayPowerStateFromString(const std::string& state_string,
                                    chromeos::DisplayPowerState* power_state) {
  if (state_string == kDisplayPowerAllOn) {
    *power_state = chromeos::DISPLAY_POWER_ALL_ON;
  } else if (state_string == kDisplayPowerInternalOffExternalOn) {
    *power_state = chromeos::DISPLAY_POWER_INTERNAL_OFF_EXTERNAL_ON;
  } else if (state_string == kDisplayPowerInternalOnExternalOff) {
    *power_state = chromeos::DISPLAY_POWER_INTERNAL_ON_EXTERNAL_OFF;
  } else {
    // Don't restore ALL_OFF state. http://crbug.com/318456.
    return false;
  }
  return true;
}

void StoreDisplayPowerState(PrefService* pref_service,
                            DisplayPowerState power_state) {
  const char* state_string = nullptr;
  switch (power_state) {
    case chromeos::DISPLAY_POWER_ALL_ON:
      state_string = kDisplayPowerAllOn;
      break;
    case chromeos::DISPLAY_POWER_INTERNAL_OFF_EXTERNAL_ON:
      state_string = kDisplayPowerInternalOffExternalOn;
      break;
    case chromeos::DISPLAY_POWER_INTERNAL_ON_EXTERNAL_OFF:
      state_string = kDisplayPowerInternalOnExternalOff;
      break;
    case chromeos::DISPLAY_POWER_ALL_OFF:
      // Don't store ALL_OFF state. http://crbug.com/318456.
      break;
  }
  if (state_string)
    pref_service->Set(prefs::kDisplayPowerState, base::Value(state_string));
}

void StoreCurrentDisplayPowerState(PrefService* pref_service) {
  StoreDisplayPowerState(
      pref_service,
      Shell::Get()->display_configurator()->GetRequestedPowerState());
}

void StoreDisplayRotationPrefs(PrefService* pref_service,
                               display::Display::Rotation rotation,
                               bool rotation_lock) {
  DictionaryPrefUpdate update(pref_service, prefs::kDisplayRotationLock);
  base::DictionaryValue* pref_data = update.Get();
  pref_data->SetBoolean("lock", rotation_lock);
  pref_data->SetInteger("orientation", static_cast<int>(rotation));
}

void StoreCurrentDisplayRotationLockPrefs(PrefService* pref_service) {
  if (!display::Display::HasInternalDisplay())
    return;
  display::Display::Rotation rotation =
      GetDisplayManager()
          ->GetDisplayInfo(display::Display::InternalDisplayId())
          .GetRotation(display::Display::RotationSource::ACCELEROMETER);
  bool rotation_lock = Shell::Get()
                           ->display_manager()
                           ->registered_internal_display_rotation_lock();
  StoreDisplayRotationPrefs(pref_service, rotation, rotation_lock);
}

void StoreDisplayTouchAssociations(PrefService* pref_service) {
  display::TouchDeviceManager* touch_device_manager =
      GetDisplayManager()->touch_device_manager();

  DictionaryPrefUpdate update(pref_service, prefs::kDisplayTouchAssociations);
  base::DictionaryValue* pref_data = update.Get();
  const display::TouchDeviceManager::TouchAssociationMap& touch_associations =
      touch_device_manager->touch_associations();

  for (const auto& association : touch_associations) {
    base::DictionaryValue association_info_map_value;
    for (const auto& association_info : association.second) {
      // Iteration for each pair of <Display ID, TouchAssociationInfo>.
      std::unique_ptr<base::DictionaryValue> association_info_value(
          new base::DictionaryValue());

      // Parsing each member of TouchAssociationInfo and storing them in
      // |association_info_value|.

      // Serialie timestamp.
      association_info_value->SetKey(
          kTouchAssociationTimestamp,
          base::Value(association_info.second.timestamp.ToDoubleT()));

      // Serialize TouchCalibrationData.
      base::DictionaryValue calibration_data_value;
      TouchDataToValue(association_info.second.calibration_data,
                       &calibration_data_value);
      association_info_value->SetKey(kTouchAssociationCalibrationData,
                                     calibration_data_value.Clone());

      // Move the searialzed TouchAssociationInfo stored in
      // |association_info_value| to |association_info_map_value| against the
      // display id as key. This is a 1 to 1 mapping of a single entry from
      // AssociationInfoMap to its serialized form.
      association_info_map_value.SetKey(
          base::Int64ToString(association_info.first),
          association_info_value->Clone());
    }
    if (association_info_map_value.empty())
      continue;

    // Move the already serialized entry of AssociationInfoMap from
    // |association_info_map_value| to |pref_data| against the
    // TouchDeviceIdentifier as key. This is a 1 to 1 mapping of a single entry
    // from TouchAssociationMap to its serialized form.
    pref_data->SetKey(association.first.ToString(),
                      association_info_map_value.Clone());
  }

  // Store the port mappings. What display a touch device connected to a
  // particular port is associated with.
  DictionaryPrefUpdate update_port(pref_service,
                                   prefs::kDisplayTouchPortAssociations);
  pref_data = update_port.Get();
  const display::TouchDeviceManager::PortAssociationMap& port_associations =
      touch_device_manager->port_associations();

  // For each port identified by the secondary id of TouchDeviceIdentifier,
  // we store the touch device and the display associated with it.
  for (const auto& association : port_associations) {
    std::unique_ptr<base::DictionaryValue> association_info_value(
        new base::DictionaryValue());
    association_info_value->SetKey(kTouchDeviceIdentifier,
                                   base::Value(association.first.ToString()));
    association_info_value->SetKey(
        kPortAssociationDisplayId,
        base::Value(base::Int64ToString(association.second)));

    pref_data->SetKey(association.first.SecondaryIdToString(),
                      association_info_value->Clone());
  }
}

// Stores mirror info for each external display.
void StoreExternalDisplayMirrorInfo(PrefService* pref_service) {
  ListPrefUpdate update(pref_service, prefs::kExternalDisplayMirrorInfo);
  base::ListValue* pref_data = update.Get();
  pref_data->Clear();
  const std::set<int64_t>& external_display_mirror_info =
      GetDisplayManager()->external_display_mirror_info();
  for (const auto& id : external_display_mirror_info)
    pref_data->GetList().emplace_back(base::Value(base::Int64ToString(id)));
}

// Stores mixed mirror mode parameters. Clear the preferences if
// |mixed_mirror_mode_params| is null.
void StoreDisplayMixedMirrorModeParams(
    PrefService* pref_service,
    const base::Optional<display::MixedMirrorModeParams>& mixed_params) {
  DictionaryPrefUpdate update(pref_service,
                              prefs::kDisplayMixedMirrorModeParams);
  base::DictionaryValue* pref_data = update.Get();
  pref_data->Clear();

  if (!mixed_params)
    return;

  pref_data->SetKey(kMirroringSourceId,
                    base::Value(base::Int64ToString(mixed_params->source_id)));

  base::ListValue mirroring_destination_ids_value;
  for (const auto& id : mixed_params->destination_ids) {
    mirroring_destination_ids_value.GetList().emplace_back(
        base::Value(base::Int64ToString(id)));
  }
  pref_data->SetKey(kMirroringDestinationIds,
                    std::move(mirroring_destination_ids_value));
}

void StoreCurrentDisplayMixedMirrorModeParams(PrefService* pref_service) {
  StoreDisplayMixedMirrorModeParams(
      pref_service, GetDisplayManager()->mixed_mirror_mode_params());
}

}  // namespace

// static
std::unique_ptr<base::Value>
DisplayPrefs::GetInitialDisplayPrefsFromPrefService(PrefService* pref_service) {
  auto initial_display_prefs =
      std::make_unique<base::Value>(base::Value::Type::DICTIONARY);
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayMixedMirrorModeParams,
      pref_service->Get(ash::prefs::kDisplayMixedMirrorModeParams)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayPowerState,
      pref_service->Get(ash::prefs::kDisplayPowerState)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayProperties,
      pref_service->Get(ash::prefs::kDisplayProperties)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayRotationLock,
      pref_service->Get(ash::prefs::kDisplayRotationLock)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayTouchAssociations,
      pref_service->Get(ash::prefs::kDisplayTouchAssociations)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kDisplayTouchPortAssociations,
      pref_service->Get(ash::prefs::kDisplayTouchPortAssociations)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kExternalDisplayMirrorInfo,
      pref_service->Get(ash::prefs::kExternalDisplayMirrorInfo)->Clone());
  initial_display_prefs->SetKey(
      ash::prefs::kSecondaryDisplays,
      pref_service->Get(ash::prefs::kSecondaryDisplays)->Clone());
  return initial_display_prefs;
}

// static
void DisplayPrefs::RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(prefs::kSecondaryDisplays,
                                   PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDisplayProperties,
                                   PrefRegistry::PUBLIC);
  registry->RegisterStringPref(prefs::kDisplayPowerState, kDisplayPowerAllOn,
                               PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDisplayRotationLock,
                                   PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDisplayTouchAssociations,
                                   PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDisplayTouchPortAssociations,
                                   PrefRegistry::PUBLIC);
  registry->RegisterListPref(prefs::kExternalDisplayMirrorInfo,
                             PrefRegistry::PUBLIC);
  registry->RegisterDictionaryPref(prefs::kDisplayMixedMirrorModeParams,
                                   PrefRegistry::PUBLIC);
}

// static
void DisplayPrefs::RegisterForeignPrefs(PrefRegistry* registry) {
  registry->RegisterForeignPref(prefs::kSecondaryDisplays);
  registry->RegisterForeignPref(prefs::kDisplayProperties);
  registry->RegisterForeignPref(prefs::kDisplayPowerState);
  registry->RegisterForeignPref(prefs::kDisplayRotationLock);
  registry->RegisterForeignPref(prefs::kDisplayTouchAssociations);
  registry->RegisterForeignPref(prefs::kDisplayTouchPortAssociations);
  registry->RegisterForeignPref(prefs::kExternalDisplayMirrorInfo);
  registry->RegisterForeignPref(prefs::kDisplayMixedMirrorModeParams);
}

DisplayPrefs::DisplayPrefs(std::unique_ptr<base::Value> initial_prefs)
    : local_state_(std::make_unique<LocalState>(std::move(initial_prefs))) {
  Shell::Get()->AddShellObserver(this);
  // If |initial_prefs| is not null, load the initial display prefs. Otherwise
  // the initial prefs will be loaded from OnLocalStatePrefServiceInitialized.
  if (local_state_->has_initial_prefs())
    LoadDisplayPreferences();
}

DisplayPrefs::~DisplayPrefs() {
  Shell::Get()->RemoveShellObserver(this);
}

void DisplayPrefs::OnLocalStatePrefServiceInitialized(
    PrefService* pref_service) {
  DCHECK(!local_state_->pref_service());
  local_state_->set_pref_service(pref_service);

  // Only load the display prefs if no initial prefs were provided.
  if (!local_state_->has_initial_prefs())
    LoadDisplayPreferences();

  if (store_requested_) {
    StoreDisplayPrefs();
    store_requested_ = false;
  }
}

void DisplayPrefs::StoreDisplayPrefs() {
  DCHECK(local_state_);
  PrefService* pref_service = local_state_->pref_service();
  if (!pref_service) {
    store_requested_ = true;
    return;
  }

  // Stores the power state regardless of the login status, because the power
  // state respects to the current status (close/open) of the lid which can be
  // changed in any situation. See http://crbug.com/285360
  StoreCurrentDisplayPowerState(pref_service);
  StoreCurrentDisplayRotationLockPrefs(pref_service);

  // Do not store prefs when the confirmation dialog is shown.
  if (!UserCanSaveDisplayPreference() ||
      !Shell::Get()->ShouldSaveDisplaySettings()) {
    return;
  }

  StoreCurrentDisplayLayoutPrefs(pref_service);
  StoreCurrentDisplayProperties(pref_service);
  StoreDisplayTouchAssociations(pref_service);
  StoreExternalDisplayMirrorInfo(pref_service);
  StoreCurrentDisplayMixedMirrorModeParams(pref_service);
}

void DisplayPrefs::LoadDisplayPreferences() {
  LocalState* local_state = local_state_.get();
  LoadDisplayLayouts(local_state);
  LoadDisplayProperties(local_state);
  LoadExternalDisplayMirrorInfo(local_state);
  LoadDisplayMixedMirrorModeParams(local_state);
  LoadDisplayRotationState(local_state);
  LoadDisplayTouchAssociations(local_state);
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          chromeos::switches::kFirstExecAfterBoot)) {
    // Restore DisplayPowerState:
    std::string value =
        local_state->Get(prefs::kDisplayPowerState)->GetString();
    chromeos::DisplayPowerState power_state;
    if (GetDisplayPowerStateFromString(value, &power_state))
      Shell::Get()->display_configurator()->SetInitialDisplayPower(power_state);
  }
}

void DisplayPrefs::StoreDisplayRotationPrefsForTest(
    display::Display::Rotation rotation,
    bool rotation_lock) {
  StoreDisplayRotationPrefs(local_state_->pref_service(), rotation,
                            rotation_lock);
}

void DisplayPrefs::StoreDisplayLayoutPrefForTest(
    const display::DisplayIdList& list,
    const display::DisplayLayout& layout) {
  StoreDisplayLayoutPref(local_state_->pref_service(), list, layout);
}

void DisplayPrefs::StoreDisplayPowerStateForTest(
    DisplayPowerState power_state) {
  StoreDisplayPowerState(local_state_->pref_service(), power_state);
}

void DisplayPrefs::LoadTouchAssociationPreferenceForTest() {
  LoadDisplayTouchAssociations(local_state_.get());
}

void DisplayPrefs::StoreLegacyTouchDataForTest(
    int64_t display_id,
    const display::TouchCalibrationData& data) {
  DictionaryPrefUpdate update(local_state_->pref_service(),
                              prefs::kDisplayProperties);
  base::DictionaryValue* pref_data = update.Get();
  std::unique_ptr<base::DictionaryValue> property_value =
      std::make_unique<base::DictionaryValue>();
  TouchDataToValue(data, property_value.get());
  pref_data->Set(base::Int64ToString(display_id), std::move(property_value));
}

bool DisplayPrefs::ParseTouchCalibrationStringForTest(
    const std::string& str,
    display::TouchCalibrationData::CalibrationPointPairQuad* point_pair_quad) {
  return ParseTouchCalibrationStringValue(str, point_pair_quad);
}

void DisplayPrefs::StoreDisplayMixedMirrorModeParamsForTest(
    const base::Optional<display::MixedMirrorModeParams>& mixed_params) {
  StoreDisplayMixedMirrorModeParams(local_state_->pref_service(), mixed_params);
}

void DisplayPrefs::SetPrefServiceForTest(PrefService* pref_service) {
  DCHECK(local_state_);
  local_state_->set_pref_service(pref_service);
}

}  // namespace ash
