#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace rex::graphics {

// Merge shader programs from the loaded, owned executable into local storage,
// retaining validated records and recovering interrupted trailing writes.
void SeedShaderStorageFromExecutable(const std::filesystem::path& storage_path,
                                     const uint8_t* image, size_t image_size,
                                     const std::filesystem::path& vertex_recipes = {});

}  // namespace rex::graphics
