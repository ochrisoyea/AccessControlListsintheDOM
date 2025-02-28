// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/accessibility/mojom/ax_assistant_structure_mojom_traits.h"

#include "mojo/public/cpp/base/string16_mojom_traits.h"
#include "ui/gfx/geometry/mojo/geometry_struct_traits.h"
#include "ui/gfx/range/mojo/range_struct_traits.h"

namespace mojo {

// static
bool StructTraits<ax::mojom::AssistantTreeDataView, ui::AssistantTree>::Read(
    ax::mojom::AssistantTreeDataView data,
    ui::AssistantTree* out) {
  if (!data.ReadNodes(&out->nodes))
    return false;
  for (size_t i = 0; i < out->nodes.size(); i++) {
    // Each child's index should be greater than its parent and within the array
    // bounds. This implies that there is no circle in the tree.
    for (size_t child_index : out->nodes[i]->children_indices) {
      if (child_index <= i || child_index >= out->nodes.size())
        return false;
    }
  }
  return true;
}

// static
bool StructTraits<ax::mojom::AssistantNodeDataView,
                  std::unique_ptr<ui::AssistantNode>>::
    Read(ax::mojom::AssistantNodeDataView data,
         std::unique_ptr<ui::AssistantNode>* out) {
  DCHECK(!out->get());
  *out = std::make_unique<ui::AssistantNode>();
  (*out)->bgcolor = data.bgcolor();
  (*out)->bold = data.bold();

  (*out)->color = data.color();
  (*out)->italic = data.italic();
  (*out)->line_through = data.line_through();
  (*out)->underline = data.underline();

  if (!data.ReadRect(&(*out)->rect) || !data.ReadText(&(*out)->text) ||
      !data.ReadRole(&(*out)->role) ||
      !data.ReadSelection(&(*out)->selection) ||
      !data.ReadChildrenIndices(&(*out)->children_indices) ||
      !data.ReadClassName(&(*out)->class_name)) {
    return false;
  }

  return true;
}

}  // namespace mojo
