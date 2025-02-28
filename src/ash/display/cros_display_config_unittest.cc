// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/display/cros_display_config.h"

#include "ash/display/cros_display_config.h"
#include "ash/display/touch_calibrator_controller.h"
#include "ash/public/interfaces/cros_display_config.mojom.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/touch/ash_touch_transform_controller.h"
#include "base/command_line.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "mojo/public/cpp/bindings/associated_binding.h"
#include "services/ui/public/cpp/input_devices/input_device_client_test_api.h"
#include "ui/display/display_switches.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/manager/test/touch_transform_controller_test_api.h"
#include "ui/display/manager/touch_transform_setter.h"
#include "ui/display/test/display_manager_test_api.h"
#include "ui/events/devices/touch_device_transform.h"
#include "ui/events/devices/touchscreen_device.h"

namespace ash {

namespace {

void SetResult(mojom::DisplayConfigResult* result_ptr,
               base::OnceClosure callback,
               mojom::DisplayConfigResult result) {
  *result_ptr = result;
  std::move(callback).Run();
}

void InitExternalTouchDevices(int64_t display_id) {
  ui::TouchscreenDevice touchdevice(
      123, ui::InputDeviceType::INPUT_DEVICE_EXTERNAL,
      std::string("test external touch device"), gfx::Size(1000, 1000), 1);
  ui::InputDeviceClientTestApi().SetTouchscreenDevices({touchdevice});

  std::vector<ui::TouchDeviceTransform> transforms;
  ui::TouchDeviceTransform touch_device_transform;
  touch_device_transform.display_id = display_id;
  touch_device_transform.device_id = touchdevice.id;
  transforms.push_back(touch_device_transform);
  display::test::TouchTransformControllerTestApi(
      ash::Shell::Get()->touch_transformer_controller())
      .touch_transform_setter()
      ->ConfigureTouchDevices(transforms);
}

class TestObserver : public mojom::CrosDisplayConfigObserver {
 public:
  TestObserver() = default;

  // mojom::CrosDisplayConfigObserver:
  void OnDisplayConfigChanged() override { display_changes_++; }

  int display_changes() const { return display_changes_; }
  void reset_display_changes() { display_changes_ = 0; }

 private:
  int display_changes_ = 0;

  DISALLOW_COPY_AND_ASSIGN(TestObserver);
};

class CrosDisplayConfigTest : public AshTestBase {
 public:
  CrosDisplayConfigTest() {}
  ~CrosDisplayConfigTest() override{};

  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        switches::kUseFirstDisplayAsInternal);
    AshTestBase::SetUp();
    CHECK(display::Screen::GetScreen());
    cros_display_config_ = Shell::Get()->cros_display_config();
  }

  mojom::DisplayLayoutInfoPtr GetDisplayLayoutInfo() {
    mojom::DisplayLayoutInfoPtr display_layout_info;
    base::RunLoop run_loop;
    cros_display_config_->GetDisplayLayoutInfo(base::BindOnce(
        [](mojom::DisplayLayoutInfoPtr* result_ptr, base::OnceClosure callback,
           mojom::DisplayLayoutInfoPtr result) {
          *result_ptr = std::move(result);
          std::move(callback).Run();
        },
        &display_layout_info, run_loop.QuitClosure()));
    run_loop.Run();
    return display_layout_info;
  }

  mojom::DisplayConfigResult SetDisplayLayoutInfo(
      mojom::DisplayLayoutInfoPtr display_layout_info) {
    mojom::DisplayConfigResult result;
    base::RunLoop run_loop;
    cros_display_config_->SetDisplayLayoutInfo(
        std::move(display_layout_info),
        base::BindOnce(&SetResult, &result, run_loop.QuitClosure()));
    run_loop.Run();
    return result;
  }

  std::vector<mojom::DisplayUnitInfoPtr> GetDisplayUnitInfoList() {
    std::vector<mojom::DisplayUnitInfoPtr> display_info_list;
    base::RunLoop run_loop;
    cros_display_config_->GetDisplayUnitInfoList(
        /*single_unified=*/false,
        base::BindOnce(
            [](std::vector<mojom::DisplayUnitInfoPtr>* result_ptr,
               base::OnceClosure callback,
               std::vector<mojom::DisplayUnitInfoPtr> result) {
              *result_ptr = std::move(result);
              std::move(callback).Run();
            },
            &display_info_list, run_loop.QuitClosure()));
    run_loop.Run();
    return display_info_list;
  }

  mojom::DisplayConfigResult SetDisplayProperties(
      const std::string& id,
      mojom::DisplayConfigPropertiesPtr properties) {
    mojom::DisplayConfigResult result;
    base::RunLoop run_loop;
    cros_display_config_->SetDisplayProperties(
        id, std::move(properties),
        base::BindOnce(&SetResult, &result, run_loop.QuitClosure()));
    run_loop.Run();
    return result;
  }

  bool OverscanCalibration(int64_t id,
                           mojom::DisplayConfigOperation op,
                           const base::Optional<gfx::Insets>& delta) {
    mojom::DisplayConfigResult result;
    base::RunLoop run_loop;
    cros_display_config()->OverscanCalibration(
        base::Int64ToString(id), op, delta,
        base::BindOnce(&SetResult, &result, run_loop.QuitClosure()));
    run_loop.Run();
    return result == mojom::DisplayConfigResult::kSuccess;
  }

  bool DisplayExists(int64_t display_id) {
    const display::Display& display =
        display_manager()->GetDisplayForId(display_id);
    return display.id() != display::kInvalidDisplayId;
  }

  mojom::TouchCalibrationPtr GetDefaultCalibration() {
    auto calibration = mojom::TouchCalibration::New();
    for (int i = 0; i < 4; ++i)
      calibration->pairs.emplace_back(mojom::TouchCalibrationPair::New());
    return calibration;
  }

  bool StartTouchCalibration(const std::string& display_id) {
    return CallTouchCalibration(
        display_id, ash::mojom::DisplayConfigOperation::kStart, nullptr);
  }

  bool CompleteCustomTouchCalibration(const std::string& display_id,
                                      mojom::TouchCalibrationPtr calibration) {
    return CallTouchCalibration(display_id,
                                ash::mojom::DisplayConfigOperation::kComplete,
                                std::move(calibration));
  }

  bool CallTouchCalibration(const std::string& id,
                            ash::mojom::DisplayConfigOperation op,
                            ash::mojom::TouchCalibrationPtr calibration) {
    mojom::DisplayConfigResult result;
    base::RunLoop run_loop;
    cros_display_config_->TouchCalibration(
        id, op, std::move(calibration),
        base::BindOnce(&SetResult, &result, run_loop.QuitClosure()));
    run_loop.Run();
    return result == mojom::DisplayConfigResult::kSuccess;
  }

  bool IsTouchCalibrationActive() {
    TouchCalibratorController* touch_calibrator =
        cros_display_config_->touch_calibrator_for_test();
    return touch_calibrator && touch_calibrator->IsCalibrating();
  }

  CrosDisplayConfig* cros_display_config() { return cros_display_config_; }

 private:
  CrosDisplayConfig* cros_display_config_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(CrosDisplayConfigTest);
};

}  // namespace

TEST_F(CrosDisplayConfigTest, OnDisplayConfigChanged) {
  TestObserver observer;
  mojom::CrosDisplayConfigObserverAssociatedPtr observer_ptr;
  mojo::AssociatedBinding<mojom::CrosDisplayConfigObserver> binding(
      &observer, mojo::MakeRequestAssociatedWithDedicatedPipe(&observer_ptr));
  cros_display_config()->AddObserver(observer_ptr.PassInterface());
  base::RunLoop().RunUntilIdle();

  // Adding one display should trigger one notification.
  UpdateDisplay("500x400");
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, observer.display_changes());
  observer.reset_display_changes();

  // Adding two displays should trigger two notification.
  UpdateDisplay("500x400,500x400");
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(2, observer.display_changes());
}

TEST_F(CrosDisplayConfigTest, GetDisplayLayoutInfo) {
  UpdateDisplay("500x400,500x400,500x400");
  std::vector<display::Display> displays =
      display::Screen::GetScreen()->GetAllDisplays();
  ASSERT_EQ(3u, displays.size());

  mojom::DisplayLayoutInfoPtr display_layout_info = GetDisplayLayoutInfo();

  ASSERT_TRUE(display_layout_info);
  const std::vector<mojom::DisplayLayoutPtr>& layouts =
      *display_layout_info->layouts;
  ASSERT_EQ(2u, layouts.size());

  EXPECT_EQ(base::Int64ToString(displays[1].id()), layouts[0]->id);
  EXPECT_EQ(base::Int64ToString(displays[0].id()), layouts[0]->parent_id);
  EXPECT_EQ(mojom::DisplayLayoutPosition::kRight, layouts[0]->position);
  EXPECT_EQ(0, layouts[0]->offset);

  EXPECT_EQ(base::Int64ToString(displays[2].id()), layouts[1]->id);
  EXPECT_EQ(layouts[0]->id, layouts[1]->parent_id);
  EXPECT_EQ(mojom::DisplayLayoutPosition::kRight, layouts[1]->position);
  EXPECT_EQ(0, layouts[1]->offset);
}

TEST_F(CrosDisplayConfigTest, SetLayoutUnified) {
  UpdateDisplay("500x400,500x400");
  EXPECT_FALSE(display_manager()->IsInUnifiedMode());

  // Enable unified desktop. Enables unified mode.
  cros_display_config()->SetUnifiedDesktopEnabled(true);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(display_manager()->IsInUnifiedMode());

  // Disable unified mode.
  auto properties = mojom::DisplayLayoutInfo::New();
  properties->layout_mode = mojom::DisplayLayoutMode::kNormal;
  mojom::DisplayConfigResult result =
      SetDisplayLayoutInfo(std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_FALSE(display_manager()->IsInUnifiedMode());

  // Enable unified mode.
  properties = mojom::DisplayLayoutInfo::New();
  properties->layout_mode = mojom::DisplayLayoutMode::kUnified;
  result = SetDisplayLayoutInfo(std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_TRUE(display_manager()->IsInUnifiedMode());

  // Restore extended mode.
  cros_display_config()->SetUnifiedDesktopEnabled(false);
  EXPECT_FALSE(display_manager()->IsInUnifiedMode());
}

TEST_F(CrosDisplayConfigTest, SetLayoutMirroredDefault) {
  UpdateDisplay("500x400,500x400,500x400");

  auto properties = mojom::DisplayLayoutInfo::New();
  properties->layout_mode = mojom::DisplayLayoutMode::kMirrored;
  mojom::DisplayConfigResult result =
      SetDisplayLayoutInfo(std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_TRUE(display_manager()->IsInMirrorMode());
  display::DisplayIdList id_list =
      display_manager()->GetMirroringDestinationDisplayIdList();
  ASSERT_EQ(2u, id_list.size());

  properties = mojom::DisplayLayoutInfo::New();
  properties->layout_mode = mojom::DisplayLayoutMode::kNormal;
  result = SetDisplayLayoutInfo(std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_FALSE(display_manager()->IsInMirrorMode());
}

TEST_F(CrosDisplayConfigTest, SetLayoutMirroredMixed) {
  UpdateDisplay("500x400,500x400,500x400,500x400");

  std::vector<display::Display> displays =
      display::Screen::GetScreen()->GetAllDisplays();
  ASSERT_EQ(4u, displays.size());

  auto properties = mojom::DisplayLayoutInfo::New();
  properties->layout_mode = mojom::DisplayLayoutMode::kMirrored;
  properties->mirror_source_id = base::Int64ToString(displays[0].id());
  properties->mirror_destination_ids =
      base::make_optional<std::vector<std::string>>(
          {base::Int64ToString(displays[1].id()),
           base::Int64ToString(displays[3].id())});
  mojom::DisplayConfigResult result =
      SetDisplayLayoutInfo(std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_TRUE(display_manager()->IsInMirrorMode());
  display::DisplayIdList id_list =
      display_manager()->GetMirroringDestinationDisplayIdList();
  ASSERT_EQ(2u, id_list.size());
  EXPECT_TRUE(base::ContainsValue(id_list, displays[1].id()));
  EXPECT_TRUE(base::ContainsValue(id_list, displays[3].id()));
}

TEST_F(CrosDisplayConfigTest, GetDisplayUnitInfoListBasic) {
  UpdateDisplay("500x600,400x520");
  std::vector<mojom::DisplayUnitInfoPtr> result = GetDisplayUnitInfoList();
  ASSERT_EQ(2u, result.size());

  int64_t display_id;
  ASSERT_TRUE(base::StringToInt64(result[0]->id, &display_id));
  ASSERT_TRUE(DisplayExists(display_id));
  const mojom::DisplayUnitInfo& info_0 = *result[0];
  EXPECT_TRUE(info_0.is_primary);
  EXPECT_TRUE(info_0.is_internal);
  EXPECT_TRUE(info_0.is_enabled);
  EXPECT_FALSE(info_0.is_tablet_mode);
  EXPECT_FALSE(info_0.has_touch_support);
  EXPECT_FALSE(info_0.has_accelerometer_support);
  EXPECT_EQ(96, info_0.dpi_x);
  EXPECT_EQ(96, info_0.dpi_y);
  EXPECT_EQ(display::Display::ROTATE_0, info_0.rotation);
  EXPECT_EQ("0,0 500x600", info_0.bounds.ToString());
  EXPECT_EQ("0,0,0,0", info_0.overscan.ToString());

  ASSERT_TRUE(base::StringToInt64(result[1]->id, &display_id));
  ASSERT_TRUE(DisplayExists(display_id));
  const mojom::DisplayUnitInfo& info_1 = *result[1];
  EXPECT_EQ(display_manager()->GetDisplayNameForId(display_id), info_1.name);
  // Second display is left of the primary display whose width 500.
  EXPECT_EQ("500,0 400x520", info_1.bounds.ToString());
  EXPECT_EQ("0,0,0,0", info_1.overscan.ToString());
  EXPECT_EQ(display::Display::ROTATE_0, info_1.rotation);
  EXPECT_FALSE(info_1.is_primary);
  EXPECT_FALSE(info_1.is_internal);
  EXPECT_TRUE(info_1.is_enabled);
  EXPECT_EQ(96, info_1.dpi_x);
  EXPECT_EQ(96, info_1.dpi_y);
}

TEST_F(CrosDisplayConfigTest, GetDisplayUnitInfoListModes) {
  UpdateDisplay("1024x512,1024x512");
  std::vector<mojom::DisplayUnitInfoPtr> result = GetDisplayUnitInfoList();
  ASSERT_EQ(2u, result.size());

  const mojom::DisplayUnitInfo& info_0 = *result[0];
  EXPECT_EQ(3, info_0.selected_display_mode_index);
  ASSERT_EQ(5u, info_0.available_display_modes.size());

  const std::vector<mojom::DisplayModePtr>& modes =
      info_0.available_display_modes;
  // Test native/selected mode.
  EXPECT_EQ("1024x512", modes[3]->size.ToString());
  EXPECT_EQ("1024x512", modes[3]->size_in_native_pixels.ToString());
  EXPECT_EQ(1.0, modes[3]->ui_scale);
  EXPECT_EQ(1.0, modes[3]->device_scale_factor);
  EXPECT_TRUE(modes[3]->is_native);
  EXPECT_EQ("1024x512", modes[3]->size.ToString());

  // Test sizes of other modes.
  EXPECT_EQ("512x256", modes[0]->size.ToString());
  EXPECT_EQ("1024x512", modes[0]->size_in_native_pixels.ToString());
  EXPECT_EQ("640x320", modes[1]->size.ToString());
  EXPECT_EQ("819x409", modes[2]->size.ToString());
  EXPECT_EQ("1152x576", modes[4]->size.ToString());
  EXPECT_EQ("1024x512", modes[4]->size_in_native_pixels.ToString());

  // External display does not have any display modes.
  const mojom::DisplayUnitInfo& info_1 = *result[1];
  EXPECT_EQ(0, info_1.selected_display_mode_index);
  ASSERT_EQ(0u, info_1.available_display_modes.size());
}

TEST_F(CrosDisplayConfigTest, GetDisplayUnitInfoListZoomFactor) {
  UpdateDisplay("1024x512,1024x512");
  std::vector<mojom::DisplayUnitInfoPtr> result = GetDisplayUnitInfoList();
  ASSERT_EQ(2u, result.size());

  const mojom::DisplayUnitInfo& info_0 = *result[0];
  EXPECT_EQ(1.0, info_0.display_zoom_factor);
  const std::vector<double>& zoom_factors =
      info_0.available_display_zoom_factors;
  EXPECT_EQ(9u, zoom_factors.size());
  EXPECT_FLOAT_EQ(0.6, zoom_factors[0]);
  EXPECT_FLOAT_EQ(0.7, zoom_factors[1]);
  EXPECT_FLOAT_EQ(0.8, zoom_factors[2]);
  EXPECT_FLOAT_EQ(0.9, zoom_factors[3]);
  EXPECT_FLOAT_EQ(1.0, zoom_factors[4]);
  EXPECT_FLOAT_EQ(1.1, zoom_factors[5]);
  EXPECT_FLOAT_EQ(1.2, zoom_factors[6]);
  EXPECT_FLOAT_EQ(1.3, zoom_factors[7]);
  EXPECT_FLOAT_EQ(1.4, zoom_factors[8]);
}

TEST_F(CrosDisplayConfigTest, SetDisplayPropertiesPrimary) {
  UpdateDisplay("1200x600,600x1000");
  int64_t primary_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  int64_t secondary_id = display_manager()->GetSecondaryDisplay().id();
  ASSERT_NE(primary_id, secondary_id);

  auto properties = mojom::DisplayConfigProperties::New();
  properties->set_primary = true;
  mojom::DisplayConfigResult result = SetDisplayProperties(
      base::Int64ToString(secondary_id), std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);

  // secondary display should now be primary.
  primary_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  EXPECT_EQ(primary_id, secondary_id);
}

TEST_F(CrosDisplayConfigTest, SetDisplayPropertiesOverscan) {
  UpdateDisplay("1200x600,600x1000*2");
  const display::Display& secondary = display_manager()->GetSecondaryDisplay();

  auto properties = mojom::DisplayConfigProperties::New();
  properties->overscan = gfx::Insets({199, 20, 51, 130});
  mojom::DisplayConfigResult result = SetDisplayProperties(
      base::Int64ToString(secondary.id()), std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("1200,0 150x250", secondary.bounds().ToString());
  const gfx::Insets overscan =
      display_manager()->GetOverscanInsets(secondary.id());
  EXPECT_EQ(199, overscan.top());
  EXPECT_EQ(20, overscan.left());
  EXPECT_EQ(51, overscan.bottom());
  EXPECT_EQ(130, overscan.right());
}

TEST_F(CrosDisplayConfigTest, SetDisplayPropertiesRotation) {
  UpdateDisplay("1200x600,600x1000*2");
  const display::Display& secondary = display_manager()->GetSecondaryDisplay();

  mojom::DisplayConfigResult result;

  auto properties = mojom::DisplayConfigProperties::New();
  properties->rotation =
      mojom::DisplayRotation::New(display::Display::ROTATE_90);
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("1200,0 500x300", secondary.bounds().ToString());
  EXPECT_EQ(display::Display::ROTATE_90, secondary.rotation());

  properties = mojom::DisplayConfigProperties::New();
  properties->rotation =
      mojom::DisplayRotation::New(display::Display::ROTATE_270);
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("1200,0 500x300", secondary.bounds().ToString());
  EXPECT_EQ(display::Display::ROTATE_270, secondary.rotation());

  // Test setting primary and rotating.
  properties = mojom::DisplayConfigProperties::New();
  properties->set_primary = true;
  properties->rotation =
      mojom::DisplayRotation::New(display::Display::ROTATE_180);
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  const display::Display& primary =
      display::Screen::GetScreen()->GetPrimaryDisplay();
  EXPECT_EQ(secondary.id(), primary.id());
  EXPECT_EQ("0,0 300x500", primary.bounds().ToString());
  EXPECT_EQ(display::Display::ROTATE_180, primary.rotation());
}

TEST_F(CrosDisplayConfigTest, SetDisplayPropertiesBoundsOrigin) {
  UpdateDisplay("1200x600,520x400");
  const display::Display& secondary = display_manager()->GetSecondaryDisplay();

  mojom::DisplayConfigResult result;

  auto properties = mojom::DisplayConfigProperties::New();
  properties->bounds_origin = gfx::Point({-520, 50});
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("-520,50 520x400", secondary.bounds().ToString());

  properties = mojom::DisplayConfigProperties::New();
  properties->bounds_origin = gfx::Point({1200, 100});
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("1200,100 520x400", secondary.bounds().ToString());

  properties = mojom::DisplayConfigProperties::New();
  properties->bounds_origin = gfx::Point({1100, -400});
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("1100,-400 520x400", secondary.bounds().ToString());

  properties = mojom::DisplayConfigProperties::New();
  properties->bounds_origin = gfx::Point({-350, 600});
  result = SetDisplayProperties(base::Int64ToString(secondary.id()),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ("-350,600 520x400", secondary.bounds().ToString());
}

TEST_F(CrosDisplayConfigTest, SetDisplayPropertiesDisplayZoomFactor) {
  UpdateDisplay("1200x600, 1600x1000#1600x1000");
  display::DisplayIdList display_id_list =
      display_manager()->GetCurrentDisplayIdList();

  const float zoom_factor_1 = 1.23f;
  const float zoom_factor_2 = 2.34f;

  display_manager()->UpdateZoomFactor(display_id_list[0], zoom_factor_2);
  display_manager()->UpdateZoomFactor(display_id_list[1], zoom_factor_1);

  EXPECT_EQ(
      zoom_factor_2,
      display_manager()->GetDisplayInfo(display_id_list[0]).zoom_factor());
  EXPECT_EQ(
      zoom_factor_1,
      display_manager()->GetDisplayInfo(display_id_list[1]).zoom_factor());

  // Set zoom factor for display 0, should not affect display 1.
  auto properties = mojom::DisplayConfigProperties::New();
  properties->display_zoom_factor = zoom_factor_1;
  mojom::DisplayConfigResult result = SetDisplayProperties(
      base::Int64ToString(display_id_list[0]), std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ(
      zoom_factor_1,
      display_manager()->GetDisplayInfo(display_id_list[0]).zoom_factor());
  EXPECT_EQ(
      zoom_factor_1,
      display_manager()->GetDisplayInfo(display_id_list[1]).zoom_factor());

  // Set zoom factor for display 1.
  properties = mojom::DisplayConfigProperties::New();
  properties->display_zoom_factor = zoom_factor_2;
  result = SetDisplayProperties(base::Int64ToString(display_id_list[1]),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kSuccess, result);
  EXPECT_EQ(
      zoom_factor_1,
      display_manager()->GetDisplayInfo(display_id_list[0]).zoom_factor());
  EXPECT_EQ(
      zoom_factor_2,
      display_manager()->GetDisplayInfo(display_id_list[1]).zoom_factor());

  // Invalid zoom factor should fail.
  const float invalid_zoom_factor = 0.01f;
  properties = mojom::DisplayConfigProperties::New();
  properties->display_zoom_factor = invalid_zoom_factor;
  result = SetDisplayProperties(base::Int64ToString(display_id_list[1]),
                                std::move(properties));
  EXPECT_EQ(mojom::DisplayConfigResult::kPropertyValueOutOfRangeError, result);
  EXPECT_EQ(
      zoom_factor_2,
      display_manager()->GetDisplayInfo(display_id_list[1]).zoom_factor());
}

TEST_F(CrosDisplayConfigTest, SetDisplayMode) {
  UpdateDisplay("1024x512,1024x512");
  std::vector<mojom::DisplayUnitInfoPtr> result = GetDisplayUnitInfoList();
  ASSERT_EQ(2u, result.size());
  EXPECT_EQ(3, result[0]->selected_display_mode_index);
  ASSERT_EQ(5u, result[0]->available_display_modes.size());

  auto properties = mojom::DisplayConfigProperties::New();
  auto display_mode = result[0]->available_display_modes[2].Clone();
  properties->display_mode = std::move(display_mode);
  ASSERT_EQ(mojom::DisplayConfigResult::kSuccess,
            SetDisplayProperties(result[0]->id, std::move(properties)));

  result = GetDisplayUnitInfoList();
  ASSERT_EQ(2u, result.size());
  EXPECT_EQ(2, result[0]->selected_display_mode_index);
}

TEST_F(CrosDisplayConfigTest, OverscanCalibration) {
  UpdateDisplay("1200x600");
  int64_t id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  ASSERT_NE(display::kInvalidDisplayId, id);

  // Test that kAdjust succeeds after kComplete call.
  EXPECT_TRUE(OverscanCalibration(id, mojom::DisplayConfigOperation::kStart,
                                  base::nullopt));
  EXPECT_EQ(gfx::Insets(0, 0, 0, 0), display_manager()->GetOverscanInsets(id));

  gfx::Insets insets(10, 10, 10, 10);
  EXPECT_TRUE(
      OverscanCalibration(id, mojom::DisplayConfigOperation::kAdjust, insets));
  // Adjust has no effect until Complete.
  EXPECT_EQ(gfx::Insets(0, 0, 0, 0), display_manager()->GetOverscanInsets(id));

  EXPECT_TRUE(OverscanCalibration(id, mojom::DisplayConfigOperation::kComplete,
                                  base::nullopt));
  gfx::Insets overscan = display_manager()->GetOverscanInsets(id);
  EXPECT_EQ(insets, overscan)
      << "Overscan: " << overscan.ToString() << " != " << insets.ToString();

  // Test that kReset clears restores previous insets.

  // Start clears any overscan values.
  EXPECT_TRUE(OverscanCalibration(id, mojom::DisplayConfigOperation::kStart,
                                  base::nullopt));
  EXPECT_EQ(gfx::Insets(0, 0, 0, 0), display_manager()->GetOverscanInsets(id));

  // Reset + Complete restores previously set insets.
  EXPECT_TRUE(OverscanCalibration(id, mojom::DisplayConfigOperation::kReset,
                                  base::nullopt));
  EXPECT_EQ(gfx::Insets(0, 0, 0, 0), display_manager()->GetOverscanInsets(id));
  EXPECT_TRUE(OverscanCalibration(id, mojom::DisplayConfigOperation::kComplete,
                                  base::nullopt));
  EXPECT_EQ(insets, display_manager()->GetOverscanInsets(id));

  // Additional complete call should fail.
  EXPECT_FALSE(OverscanCalibration(id, mojom::DisplayConfigOperation::kComplete,
                                   base::nullopt));
}

TEST_F(CrosDisplayConfigTest, CustomTouchCalibrationInternal) {
  UpdateDisplay("1200x600,600x1000*2");
  const int64_t internal_display_id =
      display::test::DisplayManagerTestApi(display_manager())
          .SetFirstDisplayAsInternalDisplay();

  InitExternalTouchDevices(internal_display_id);

  EXPECT_FALSE(StartTouchCalibration(base::Int64ToString(internal_display_id)));
  EXPECT_FALSE(IsTouchCalibrationActive());
}

TEST_F(CrosDisplayConfigTest, CustomTouchCalibrationWithoutStart) {
  UpdateDisplay("1200x600,600x1000*2");
  EXPECT_FALSE(IsTouchCalibrationActive());
}

TEST_F(CrosDisplayConfigTest, CustomTouchCalibrationNonTouchDisplay) {
  UpdateDisplay("1200x600,600x1000*2");

  const int64_t internal_display_id =
      display::test::DisplayManagerTestApi(display_manager())
          .SetFirstDisplayAsInternalDisplay();

  display::DisplayIdList display_id_list =
      display_manager()->GetCurrentDisplayIdList();

  // Pick the non internal display Id.
  const int64_t display_id = display_id_list[0] == internal_display_id
                                 ? display_id_list[1]
                                 : display_id_list[0];

  ui::InputDeviceClientTestApi().SetTouchscreenDevices({});
  std::string id = base::Int64ToString(display_id);

  // Since no external touch devices are present, the calibration should fail.
  EXPECT_FALSE(StartTouchCalibration(id));

  // If an external touch device is present, the calibration should proceed.
  InitExternalTouchDevices(display_id);
  EXPECT_TRUE(StartTouchCalibration(id));
  EXPECT_TRUE(IsTouchCalibrationActive());
}

TEST_F(CrosDisplayConfigTest, CustomTouchCalibrationInvalidPoints) {
  UpdateDisplay("1200x600,600x1000*2");

  const int64_t internal_display_id =
      display::test::DisplayManagerTestApi(display_manager())
          .SetFirstDisplayAsInternalDisplay();

  display::DisplayIdList display_id_list =
      display_manager()->GetCurrentDisplayIdList();

  // Pick the non internal display Id.
  const int64_t display_id = display_id_list[0] == internal_display_id
                                 ? display_id_list[1]
                                 : display_id_list[0];

  InitExternalTouchDevices(display_id);

  std::string id = base::Int64ToString(display_id);

  EXPECT_TRUE(StartTouchCalibration(id));
  mojom::TouchCalibrationPtr calibration = GetDefaultCalibration();
  calibration->pairs[0]->display_point.set_x(-1);
  EXPECT_FALSE(CompleteCustomTouchCalibration(id, std::move(calibration)));

  EXPECT_TRUE(StartTouchCalibration(id));
  calibration = GetDefaultCalibration();
  calibration->bounds.set_width(1);
  calibration->pairs[0]->display_point.set_x(2);
  EXPECT_FALSE(CompleteCustomTouchCalibration(id, std::move(calibration)));
}

TEST_F(CrosDisplayConfigTest, CustomTouchCalibrationSuccess) {
  UpdateDisplay("1200x600,600x1000*2");

  const int64_t internal_display_id =
      display::test::DisplayManagerTestApi(display_manager())
          .SetFirstDisplayAsInternalDisplay();

  display::DisplayIdList display_id_list =
      display_manager()->GetCurrentDisplayIdList();

  // Pick the non internal display Id.
  const int64_t display_id = display_id_list[0] == internal_display_id
                                 ? display_id_list[1]
                                 : display_id_list[0];

  InitExternalTouchDevices(display_id);

  std::string id = base::Int64ToString(display_id);

  EXPECT_TRUE(StartTouchCalibration(id));
  EXPECT_TRUE(IsTouchCalibrationActive());
  mojom::TouchCalibrationPtr calibration = GetDefaultCalibration();
  EXPECT_TRUE(CompleteCustomTouchCalibration(id, std::move(calibration)));
}

}  // namespace ash
