#pragma once

#include "meshes.h"
#include "textures.h"

namespace rex {
class Runtime;
}
namespace modkit_meshes {
void Init(rex::Runtime* runtime, std::shared_ptr<const simpsons::modkit::MeshVariants> variants,
          std::shared_ptr<const simpsons::modkit::TextureVariants> textures = {});
void Shutdown();
// The named-resource reader calls this before consuming its header. Only a
// selected, source-matched mesh or texture is edited; the game keeps ownership.
void Apply(uint8_t* base, uint32_t stream);
}  // namespace modkit_meshes
