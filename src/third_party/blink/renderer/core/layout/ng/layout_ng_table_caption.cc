// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/layout_ng_table_caption.h"

#include "third_party/blink/renderer/core/layout/layout_analyzer.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space.h"

namespace blink {

LayoutNGTableCaption::LayoutNGTableCaption(Element* element)
    : LayoutNGMixin<LayoutTableCaption>(element) {}

void LayoutNGTableCaption::UpdateBlockLayout(bool relayout_children) {
  LayoutAnalyzer::BlockScope analyzer(*this);

  DCHECK(!IsOutOfFlowPositioned()) << "Out of flow captions are blockified.";

  scoped_refptr<NGConstraintSpace> constraint_space =
      NGConstraintSpace::CreateFromLayoutObject(*this);

  scoped_refptr<NGLayoutResult> result =
      NGBlockNode(this).Layout(*constraint_space);

  // Tell legacy layout there were abspos descendents we couldn't place. We know
  // we have to pass up to legacy here because this method is legacy's entry
  // point to LayoutNG. If our parent were LayoutNG, it wouldn't have called
  // UpdateBlockLayout, it would have packaged this LayoutObject into
  // NGBlockNode and called Layout on that.
  for (NGOutOfFlowPositionedDescendant descendant :
       result->OutOfFlowPositionedDescendants())
    descendant.node.UseOldOutOfFlowPositioning();

  // The parent table sometimes changes the caption's position after laying it
  // out. So there's no point in setting the fragment's offset here;
  // NGBoxFragmentPainter::Paint will have to handle it until table layout is
  // implemented in NG, in which case that algorithm will set each child's
  // offsets. See https://crbug.com/788590 for more info.
  DCHECK(!result->PhysicalFragment()->IsPlacedByLayoutNG())
      << "Only a table should be placing table caption fragments and the ng "
         "table algorithm doesn't exist yet!";
}

}  // namespace blink
