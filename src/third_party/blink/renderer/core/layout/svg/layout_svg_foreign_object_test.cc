// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/layout_geometry_map.h"
#include "third_party/blink/renderer/core/testing/core_unit_test_helper.h"

namespace blink {

class LayoutSVGForeignObjectTest : public RenderingTest {
 public:
  LayoutSVGForeignObjectTest()
      : RenderingTest(SingleChildLocalFrameClient::Create()) {}
};

TEST_F(LayoutSVGForeignObjectTest, DivInForeignObject) {
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0 }</style>
    <svg id='svg' style='width: 500px; height: 400px'>
      <foreignObject id='foreign' x='100' y='100' width='300' height='200'>
        <div id='div' style='margin: 50px; width: 200px; height: 100px'>
        </div>
      </foreignObject>
    </svg>
  )HTML");

  const auto& svg = *GetDocument().getElementById("svg");
  const auto& foreign = *GetDocument().getElementById("foreign");
  const auto& foreign_object = *GetLayoutObjectByElementId("foreign");
  const auto& div = *GetLayoutObjectByElementId("div");

  EXPECT_EQ(FloatRect(100, 100, 300, 200), foreign_object.ObjectBoundingBox());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalSVGTransform());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalToSVGParentTransform());

  // mapToVisualRectInAncestorSpace
  LayoutRect div_rect(0, 0, 100, 50);
  EXPECT_TRUE(div.MapToVisualRectInAncestorSpace(&GetLayoutView(), div_rect));
  EXPECT_EQ(LayoutRect(150, 150, 100, 50), div_rect);

  // mapLocalToAncestor
  TransformState transform_state(TransformState::kApplyTransformDirection,
                                 FloatPoint());
  div.MapLocalToAncestor(&GetLayoutView(), transform_state,
                         kTraverseDocumentBoundaries);
  transform_state.Flatten();
  EXPECT_EQ(FloatPoint(150, 150), transform_state.LastPlanarPoint());

  // mapAncestorToLocal
  TransformState transform_state1(
      TransformState::kUnapplyInverseTransformDirection, FloatPoint());
  div.MapAncestorToLocal(&GetLayoutView(), transform_state1,
                         kTraverseDocumentBoundaries);
  transform_state1.Flatten();
  EXPECT_EQ(FloatPoint(-150, -150), transform_state1.LastPlanarPoint());

  // pushMappingToContainer
  LayoutGeometryMap rgm(kTraverseDocumentBoundaries);
  rgm.PushMappingsToAncestor(&div, nullptr);
  EXPECT_EQ(FloatQuad(FloatRect(150, 150, 1, 2)),
            rgm.MapToAncestor(FloatRect(0, 0, 1, 2), nullptr));

  // Hit testing
  EXPECT_EQ(svg, HitTest(1, 1));
  EXPECT_EQ(foreign, HitTest(149, 149));
  EXPECT_EQ(div.GetNode(), HitTest(150, 150));
  EXPECT_EQ(div.GetNode(), HitTest(349, 249));
  EXPECT_EQ(foreign, HitTest(350, 250));
  EXPECT_EQ(svg, HitTest(450, 350));
}

TEST_F(LayoutSVGForeignObjectTest, IframeInForeignObject) {
  SetBodyInnerHTML(R"HTML(
    <style>body { margin: 0 }</style>
    <svg id='svg' style='width: 500px; height: 450px'>
      <foreignObject id='foreign' x='100' y='100' width='300' height='250'>
        <iframe style='border: none; margin: 30px;
             width: 240px; height: 190px'></iframe>
      </foreignObject>
    </svg>
  )HTML");
  SetChildFrameHTML(R"HTML(
    <style>
      body { margin: 0 }
      * { background: white; }
    </style>
    <div id='div' style='margin: 70px; width: 100px; height: 50px'></div>
  )HTML");
  GetDocument().View()->UpdateAllLifecyclePhases();

  const auto& svg = *GetDocument().getElementById("svg");
  const auto& foreign = *GetDocument().getElementById("foreign");
  const auto& foreign_object = *GetLayoutObjectByElementId("foreign");
  const auto& div = *ChildDocument().getElementById("div")->GetLayoutObject();

  EXPECT_EQ(FloatRect(100, 100, 300, 250), foreign_object.ObjectBoundingBox());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalSVGTransform());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalToSVGParentTransform());

  // mapToVisualRectInAncestorSpace
  LayoutRect div_rect(0, 0, 100, 50);
  EXPECT_TRUE(div.MapToVisualRectInAncestorSpace(&GetLayoutView(), div_rect));
  EXPECT_EQ(LayoutRect(200, 200, 100, 50), div_rect);

  // mapLocalToAncestor
  TransformState transform_state(TransformState::kApplyTransformDirection,
                                 FloatPoint());
  div.MapLocalToAncestor(&GetLayoutView(), transform_state,
                         kTraverseDocumentBoundaries);
  transform_state.Flatten();
  EXPECT_EQ(FloatPoint(200, 200), transform_state.LastPlanarPoint());

  // mapAncestorToLocal
  TransformState transform_state1(
      TransformState::kUnapplyInverseTransformDirection, FloatPoint());
  div.MapAncestorToLocal(&GetLayoutView(), transform_state1,
                         kTraverseDocumentBoundaries);
  transform_state1.Flatten();
  EXPECT_EQ(FloatPoint(-200, -200), transform_state1.LastPlanarPoint());

  // pushMappingToContainer
  LayoutGeometryMap rgm(kTraverseDocumentBoundaries);
  rgm.PushMappingsToAncestor(&div, nullptr);
  EXPECT_EQ(FloatQuad(FloatRect(200, 200, 1, 2)),
            rgm.MapToAncestor(FloatRect(0, 0, 1, 2), nullptr));

  // Hit testing
  EXPECT_EQ(svg, HitTest(90, 90));
  EXPECT_EQ(foreign, HitTest(129, 129));
  EXPECT_EQ(ChildDocument().documentElement(), HitTest(130, 130));
  EXPECT_EQ(ChildDocument().documentElement(), HitTest(199, 199));
  EXPECT_EQ(div.GetNode(), HitTest(200, 200));
  EXPECT_EQ(div.GetNode(), HitTest(299, 249));
  EXPECT_EQ(ChildDocument().documentElement(), HitTest(300, 250));
  EXPECT_EQ(ChildDocument().documentElement(), HitTest(369, 319));
  EXPECT_EQ(foreign, HitTest(370, 320));
  EXPECT_EQ(svg, HitTest(450, 400));
}

TEST_F(LayoutSVGForeignObjectTest, HitTestZoomedForeignObject) {
  SetBodyInnerHTML(R"HTML(
    <style>* { margin: 0; zoom: 150% }</style>
    <svg id='svg' style='width: 200px; height: 200px'>
      <foreignObject id='foreign' x='10' y='10' width='100' height='150'>
        <div id='div' style='margin: 50px; width: 50px; height: 50px'>
        </div>
      </foreignObject>
    </svg>
  )HTML");

  const auto& svg = *GetDocument().getElementById("svg");
  const auto& foreign = *GetDocument().getElementById("foreign");
  const auto& foreign_object = *GetLayoutObjectByElementId("foreign");
  const auto& div = *GetDocument().getElementById("div");

  EXPECT_EQ(FloatRect(10, 10, 100, 150), foreign_object.ObjectBoundingBox());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalSVGTransform());
  EXPECT_EQ(AffineTransform(), foreign_object.LocalToSVGParentTransform());

  // mapToVisualRectInAncestorSpace
  LayoutRect div_rect(0, 0, 100, 50);
  EXPECT_TRUE(div.GetLayoutObject()->MapToVisualRectInAncestorSpace(
      &GetLayoutView(), div_rect));
  EXPECT_EQ(LayoutRect(286, 286, 339, 170), div_rect);

  // mapLocalToAncestor
  TransformState transform_state(TransformState::kApplyTransformDirection,
                                 FloatPoint());
  div.GetLayoutObject()->MapLocalToAncestor(&GetLayoutView(), transform_state,
                                            kTraverseDocumentBoundaries);
  transform_state.Flatten();
  EXPECT_EQ(FloatPoint(286.875, 286.875), transform_state.LastPlanarPoint());

  // mapAncestorToLocal
  TransformState transform_state1(
      TransformState::kUnapplyInverseTransformDirection,
      FloatPoint(286.875, 286.875));
  div.GetLayoutObject()->MapAncestorToLocal(&GetLayoutView(), transform_state1,
                                            kTraverseDocumentBoundaries);
  transform_state1.Flatten();
  EXPECT_EQ(FloatPoint(), transform_state1.LastPlanarPoint());

  EXPECT_EQ(svg, HitTest(20, 20));
  EXPECT_EQ(foreign, HitTest(280, 280));
  EXPECT_EQ(div, HitTest(290, 290));
}

TEST_F(LayoutSVGForeignObjectTest, HitTestViewBoxForeignObject) {
  SetBodyInnerHTML(R"HTML(
    <svg id='svg' style='width: 200px; height: 200px' viewBox='0 0 100 100'>
      <foreignObject id='foreign' x='10' y='10' width='100' height='150'>
        <div id='div' style='margin: 50px; width: 50px; height: 50px'>
        </div>
      </foreignObject>
    </svg>
  )HTML");

  const auto& svg = *GetDocument().getElementById("svg");
  const auto& foreign = *GetDocument().getElementById("foreign");
  const auto& div = *GetDocument().getElementById("div");

  // mapLocalToAncestor
  TransformState transform_state(TransformState::kApplyTransformDirection,
                                 FloatPoint());
  div.GetLayoutObject()->MapLocalToAncestor(&GetLayoutView(), transform_state,
                                            kTraverseDocumentBoundaries);
  transform_state.Flatten();
  EXPECT_EQ(FloatPoint(128, 128), transform_state.LastPlanarPoint());

  // mapAncestorToLocal
  TransformState transform_state1(
      TransformState::kUnapplyInverseTransformDirection, FloatPoint(128, 128));
  div.GetLayoutObject()->MapAncestorToLocal(&GetLayoutView(), transform_state1,
                                            kTraverseDocumentBoundaries);
  transform_state1.Flatten();
  EXPECT_EQ(FloatPoint(), transform_state1.LastPlanarPoint());

  EXPECT_EQ(svg, HitTest(20, 20));
  EXPECT_EQ(foreign, HitTest(120, 110));
  EXPECT_EQ(div, HitTest(160, 160));
}

TEST_F(LayoutSVGForeignObjectTest, HitTestUnderClipPath) {
  SetBodyInnerHTML(R"HTML(
    <style>
      * {
        margin: 0
      }
      #target {
         width: 500px;
         height: 500px;
         background-color: blue;
      }
      #target:hover {
        background-color: green;
      }
    </style>
    <svg id="svg" style="width: 500px; height: 500px">
      <clipPath id="c">
        <circle cx="250" cy="250" r="200"/>
      </clipPath>
      <g clip-path="url(#c)">
        <foreignObject id="foreignObject" width="100%" height="100%">
        </foreignObject>
      </g>
    </svg>
  )HTML");

  const auto& svg = *GetDocument().getElementById("svg");
  const auto& foreignObject = *GetDocument().getElementById("foreignObject");

  // The fist and the third return |svg| because the circle clip-path
  // clips out the foreignObject.
  EXPECT_EQ(svg, GetDocument().ElementFromPoint(20, 20));
  EXPECT_EQ(foreignObject, GetDocument().ElementFromPoint(250, 250));
  EXPECT_EQ(svg, GetDocument().ElementFromPoint(400, 400));
}

}  // namespace blink
