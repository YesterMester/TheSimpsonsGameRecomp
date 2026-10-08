// Export the shared render target transfer programs for native qualification.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/graphics/util/dxbc_render_target_transfer.h>

int main(int argc, char** argv) {
  using Generator = rex::graphics::DxbcRenderTargetTransferShader;
  using namespace rex::graphics;
  if (argc != 2) {
    std::fprintf(stderr, "usage: dxbc_transfer <output directory>\n");
    return 2;
  }
  std::filesystem::path folder(argv[1]);
  std::filesystem::create_directories(folder);
  constexpr std::array<uint32_t, 12> colors = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
  uint32_t count = 0;
  std::vector<uint32_t> code;
  std::string error;
  for (uint32_t scale : {1u, 2u}) {
    for (uint32_t msaa_2x = 0; msaa_2x < 2; ++msaa_2x) {
      Generator::Options options;
      options.resolution_scale_x = options.resolution_scale_y = scale;
      options.msaa_2x_supported = msaa_2x;
      for (uint32_t mode = 0; mode < uint32_t(Generator::TransferMode::kCount); ++mode) {
        auto transfer_mode = Generator::TransferMode(mode);
        const auto& info = Generator::kTransferModes[mode];
        bool source_color = transfer_mode == Generator::TransferMode::kColorToDepth ||
                            transfer_mode == Generator::TransferMode::kColorToColor ||
                            transfer_mode == Generator::TransferMode::kColorToStencilBit ||
                            transfer_mode == Generator::TransferMode::kColorAndHostDepthToDepth;
        bool dest_color = info.output == Generator::TransferOutput::kColor;
        bool host_depth = transfer_mode == Generator::TransferMode::kColorAndHostDepthToDepth ||
                          transfer_mode == Generator::TransferMode::kDepthAndHostDepthToDepth;
        for (uint32_t source = 0; source < (source_color ? colors.size() : 2); ++source) {
          for (uint32_t destination = 0; destination < (dest_color ? colors.size() : 2);
               ++destination) {
            for (uint32_t source_samples = 0; source_samples < 3; ++source_samples) {
              for (uint32_t dest_samples = 0; dest_samples < 3; ++dest_samples) {
                for (uint32_t host = 0; host < (host_depth ? 4u : 1u); ++host) {
                  Generator::TransferShaderKey key;
                  key.mode = transfer_mode;
                  key.source_resource_format = source_color ? colors[source] : source;
                  key.dest_resource_format = dest_color ? colors[destination] : destination;
                  key.source_msaa_samples = xenos::MsaaSamples(source_samples);
                  key.dest_msaa_samples = xenos::MsaaSamples(dest_samples);
                  key.host_depth_source_is_copy = host == 3;
                  key.host_depth_source_msaa_samples = xenos::MsaaSamples(host == 3 ? 0 : host);
                  if (!Generator::Create(key, options, code, error)) {
                    std::fprintf(stderr, "Transfer %08X failed: %s\n", key.key, error.c_str());
                    return 1;
                  }
                  char name[96];
                  std::snprintf(name, sizeof(name), "transfer_%08X_scale%u_msaa2%u.sm51", key.key,
                                scale, msaa_2x);
                  std::ofstream file(folder / name, std::ios::binary);
                  file.write(reinterpret_cast<const char*>(code.data()),
                             std::streamsize(code.size() * 4));
                  if (!file) {
                    std::fprintf(stderr, "Could not write %s\n", name);
                    return 1;
                  }
                  ++count;
                }
              }
            }
          }
        }
      }
    }
  }
  std::printf("Exported %u render target transfer configurations\n", count);
  return 0;
}
