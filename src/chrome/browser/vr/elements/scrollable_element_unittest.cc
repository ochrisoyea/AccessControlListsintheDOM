// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/vr/elements/scrollable_element.h"

#include "base/stl_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_gesture_event.h"

namespace vr {

namespace {

std::unique_ptr<blink::WebGestureEvent> CreateScrollUpdate(float delta_x,
                                                           float delta_y) {
  auto gesture = std::make_unique<blink::WebGestureEvent>();
  gesture->SetType(blink::WebGestureEvent::kGestureScrollBegin);
  gesture->data.scroll_update.delta_x = delta_x;
  gesture->data.scroll_update.delta_y = delta_y;
  return gesture;
}

}  // namespace

TEST(ScrollableElement, VerticalOnScrollUpdate) {
  auto element =
      std::make_unique<ScrollableElement>(ScrollableElement::kVertical);
  element->SetMaxSpan(1.0f);
  auto child = std::make_unique<UiElement>();
  child->SetSize(1.0f, 2.0f);
  element->AddScrollingChild(std::move(child));
  element->SizeAndLayOut();

  EXPECT_FLOAT_EQ(0.0f, element->scroll_offset());

  struct {
    float delta_x;
    float delta_y;
    float expected;
  } test_cases[] = {
      {1000.0f, 1000.0f, -0.5f}, {-1000.0f, -1000.0f, 0.5f},
  };

  for (size_t i = 0; i < base::size(test_cases); ++i) {
    SCOPED_TRACE(i);
    element->OnScrollUpdate(
        CreateScrollUpdate(test_cases[i].delta_x, test_cases[i].delta_y), {});
    EXPECT_FLOAT_EQ(test_cases[i].expected, element->scroll_offset());
  }
}

TEST(ScrollableElement, VerticalTopOnScrollUpdate) {
  auto element =
      std::make_unique<ScrollableElement>(ScrollableElement::kVertical);
  element->SetMaxSpan(1.0f);
  element->SetScrollAnchoring(TOP);
  auto child = std::make_unique<UiElement>();
  child->SetSize(1.0f, 2.0f);
  element->AddScrollingChild(std::move(child));
  element->SizeAndLayOut();

  EXPECT_FLOAT_EQ(-0.5f, element->scroll_offset());

  struct {
    float delta_x;
    float delta_y;
    float expected;
  } test_cases[] = {{-1000.0f, -1000.0f, 0.5f}, {1000.0f, 1000.0f, -0.5f}};

  for (size_t i = 0; i < base::size(test_cases); ++i) {
    SCOPED_TRACE(i);
    element->OnScrollUpdate(
        CreateScrollUpdate(test_cases[i].delta_x, test_cases[i].delta_y), {});
    EXPECT_FLOAT_EQ(test_cases[i].expected, element->scroll_offset());
  }
}

TEST(ScrollableElement, HorizontalScrollUpdate) {
  auto element =
      std::make_unique<ScrollableElement>(ScrollableElement::kHorizontal);
  element->SetMaxSpan(1.0f);
  auto child = std::make_unique<UiElement>();
  child->SetSize(2.0f, 1.0f);
  element->AddScrollingChild(std::move(child));
  element->SizeAndLayOut();

  EXPECT_FLOAT_EQ(0.0f, element->scroll_offset());

  struct {
    float delta_x;
    float delta_y;
    float expected;
  } test_cases[] = {
      {1000.0f, 1000.0f, -0.5f}, {-1000.0f, -1000.0f, 0.5f},
  };

  for (size_t i = 0; i < base::size(test_cases); ++i) {
    SCOPED_TRACE(i);
    element->OnScrollUpdate(
        CreateScrollUpdate(test_cases[i].delta_x, test_cases[i].delta_y), {});
    EXPECT_FLOAT_EQ(test_cases[i].expected, element->scroll_offset());
  }
}

}  // namespace vr
