#pragma once

#include "models.h"

namespace rex {
class Runtime;
}
namespace modkit_models {
void Init(rex::Runtime* runtime, std::shared_ptr<const simpsons::modkit::ModelVariants> variants);
void Shutdown();
// Internal native-loader seam; callers retain ownership of the input buffer.
void Apply(uint8_t* base, uint32_t address, uint32_t size);
}  // namespace modkit_models
