#pragma once

#include <filesystem>
#include <vector>

namespace rex::filesystem { class VirtualFileSystem; }

// Called before LoadXexImage, while no guest content handles exist. Builds a
// single directory view so file opens and directory enumeration agree.
void MountModFiles(rex::filesystem::VirtualFileSystem* vfs,
                   const std::filesystem::path& game,
                   const std::vector<std::filesystem::path>& roots);
