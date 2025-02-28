// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/chromeos/camera_metadata_utils.h"

#include <algorithm>
#include <unordered_set>

namespace media {

cros::mojom::CameraMetadataEntryPtr* GetMetadataEntry(
    const cros::mojom::CameraMetadataPtr& camera_metadata,
    cros::mojom::CameraMetadataTag tag) {
  if (!camera_metadata || !camera_metadata->entries.has_value()) {
    return nullptr;
  }
  // We assume the metadata entries are sorted.
  auto iter = std::find_if(camera_metadata->entries.value().begin(),
                           camera_metadata->entries.value().end(),
                           [tag](const cros::mojom::CameraMetadataEntryPtr& e) {
                             return e->tag == tag;
                           });
  if (iter == camera_metadata->entries.value().end()) {
    return nullptr;
  }
  return &(camera_metadata->entries.value()[(*iter)->index]);
}

void SortCameraMetadata(cros::mojom::CameraMetadataPtr* camera_metadata) {
  if (!camera_metadata || !(*camera_metadata) ||
      !(*camera_metadata)->entries.has_value()) {
    return;
  }
  std::sort((*camera_metadata)->entries.value().begin(),
            (*camera_metadata)->entries.value().end(),
            [](const cros::mojom::CameraMetadataEntryPtr& a,
               const cros::mojom::CameraMetadataEntryPtr& b) {
              return a->tag < b->tag;
            });
  for (size_t i = 0; i < (*camera_metadata)->entries.value().size(); ++i) {
    (*camera_metadata)->entries.value()[i]->index = i;
  }
}

void MergeMetadata(cros::mojom::CameraMetadataPtr* to,
                   const cros::mojom::CameraMetadataPtr& from) {
  DCHECK(to);
  (*to)->entry_count += from->entry_count;
  (*to)->entry_capacity += from->entry_count;
  (*to)->data_count += from->data_count;
  (*to)->data_capacity += from->data_count;

  if (!from->entries) {
    return;
  }

  std::unordered_set<cros::mojom::CameraMetadataTag> tags;
  if ((*to)->entries) {
    for (const auto& entry : (*to)->entries.value()) {
      tags.insert(entry->tag);
    }
  } else {
    (*to)->entries = std::vector<cros::mojom::CameraMetadataEntryPtr>();
  }
  for (const auto& entry : from->entries.value()) {
    if (tags.find(entry->tag) != tags.end()) {
      LOG(ERROR) << "Found duplicated entries for tag " << entry->tag;
      continue;
    }
    tags.insert(entry->tag);
    (*to)->entries->push_back(entry->Clone());
  }
}

}  // namespace media
