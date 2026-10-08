// Export the shared primitive shaders for native Direct3D 11 qualification.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include <rex/graphics/util/dxbc_geometry.h>

int main(int argc, char** argv) {
  using namespace rex::graphics;
  if (argc != 2) {
    std::fprintf(stderr, "usage: dxbc_geometry <output directory>\n");
    return 2;
  }
  std::filesystem::path folder(argv[1]);
  std::filesystem::create_directories(folder);
  std::set<uint32_t> keys;
  for (auto type : {DxbcGeometryShaderType::kPointList, DxbcGeometryShaderType::kRectangleList,
                    DxbcGeometryShaderType::kQuadList}) {
    for (uint32_t interpolators : {0u, 1u, 4u, 8u, 16u}) {
      for (uint32_t clip_planes : {0u, 2u, 6u}) {
        for (uint32_t cull = 0; cull < 2; ++cull) {
          for (uint32_t kill = 0; kill < 2; ++kill) {
            for (uint32_t point_flags = 0; point_flags < 4; ++point_flags) {
              for (uint32_t mode = 0; mode < (type == DxbcGeometryShaderType::kPointList ? 4u : 1u);
                   ++mode) {
                DxbcGeometryShaderKey key;
                key.type = type;
                key.interpolator_count = interpolators;
                key.user_clip_plane_count = clip_planes;
                key.user_clip_plane_cull = cull;
                key.has_vertex_kill_and = kill;
                key.has_point_size =
                    uint32_t(type == DxbcGeometryShaderType::kPointList && (point_flags & 1));
                key.has_point_coordinates = point_flags >> 1;
                key.point_ps_ucp_mode = mode;
                keys.insert(key.key);
              }
            }
          }
        }
      }
    }
  }
  std::vector<uint32_t> code;
  for (uint32_t bits : keys) {
    DxbcGeometryShaderKey key;
    key.key = bits;
    CreateDxbcGeometryShader(key, code);
    char name[64];
    std::snprintf(name, sizeof(name), "geometry_%08X.sm51", bits);
    std::ofstream file(folder / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
    if (!file) {
      std::fprintf(stderr, "Could not write %s\n", name);
      return 1;
    }
  }
  std::printf("Exported %zu primitive shader configurations\n", keys.size());
  return 0;
}
