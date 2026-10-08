// Export native render target encoding programs for resolve qualification.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/graphics/util/dxbc_render_target_dump.h>

int main(int argc, char** argv) {
  using Generator = rex::graphics::DxbcRenderTargetDumpShader;
  using namespace rex::graphics;
  if (argc != 2) {
    std::fprintf(stderr, "usage: dxbc_dump <output directory>\n");
    return 2;
  }
  std::filesystem::path folder(argv[1]);
  std::filesystem::create_directories(folder);
  constexpr std::array<uint32_t, 12> colors = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
  uint32_t count = 0;
  std::vector<uint32_t> code;
  std::string error;
  for (uint32_t scale : {1u, 2u})
    for (uint32_t native_2x : {0u, 1u})
      for (uint32_t portable : {0u, 1u})
        for (uint32_t depth_convert : {0u, 1u})
          for (uint32_t depth_round : {0u, 1u}) {
            Generator::Options options;
            options.resolution_scale_x = options.resolution_scale_y = scale;
            options.msaa_2x_supported = native_2x;
            options.portable_integer_division = portable;
            options.depth_float24_convert_in_pixel_shader = depth_convert;
            options.depth_float24_round = depth_round;
            for (uint32_t depth : {0u, 1u})
              for (uint32_t format = 0; format < (depth ? 2u : uint32_t(colors.size())); ++format)
                for (uint32_t samples = 0; samples < 3; ++samples) {
                  Generator::Key key;
                  key.is_depth = depth;
                  key.resource_format = depth ? format : colors[format];
                  key.msaa_samples = xenos::MsaaSamples(samples);
                  if (!Generator::Create(key, options, code, error)) {
                    std::fprintf(stderr, "Resolve encoding %08X failed: %s\n", key.key, error.c_str());
                    return 1;
                  }
                  char name[128];
                  std::snprintf(name, sizeof(name), "dump_%08X_scale%u_msaa2%u_portable%u_convert%u_round%u.sm51",
                                key.key, scale, native_2x, portable, depth_convert, depth_round);
                  std::ofstream file(folder / name, std::ios::binary);
                  file.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
                  if (!file) {
                    std::fprintf(stderr, "Could not write %s\n", name);
                    return 1;
                  }
                  ++count;
                }
          }
  std::printf("Exported %u render target resolve encoding configurations\n", count);
  return 0;
}
