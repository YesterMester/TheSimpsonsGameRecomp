#pragma once

#include "assets.h"

namespace simpsons::modkit {

// An archive/display name is never an actor identity. Reuse the same authored
// GUID, factory class and model relationship as assets.templates.
inline bool LibraryTemplateMatches(const AssetSnapshot& snapshot, size_t asset_index,
                                   std::string_view guid, uint32_t hash,
                                   std::string_view model) {
  if (!snapshot.ready || asset_index >= snapshot.assets.size() || guid.empty() || !hash)
    return false;
  for (const auto index : snapshot.assets[asset_index].objects) {
    if (index >= snapshot.objects.size()) return false;
    const auto& object = snapshot.objects[index];
    if (object.guid == guid && object.hash == hash &&
        (object.model_resource.empty() || object.model_resource == model))
      return true;
  }
  return false;
}

}  // namespace simpsons::modkit
