// Offline Xenos shader compilation for the native Direct3D 11 renderer.
// Inputs come from the user's own shader inventory; no game data is included.

#include <algorithm>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/string/buffer.h>

namespace {
bool Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(file);
}
}  // namespace

int main(int argc, char** argv) {
  using namespace rex::graphics;
  if (argc != 3 && argc != 5) {
    std::fprintf(stderr,
                 "usage: dxbc_translate <ucode directory> <output directory> "
                 "[--endian big|little]\n");
    return 2;
  }
  std::endian endian = std::endian::big;
  if (argc == 5) {
    if (std::string(argv[3]) != "--endian" ||
        (std::string(argv[4]) != "big" && std::string(argv[4]) != "little")) {
      std::fprintf(stderr, "Choose big or little source byte order\n");
      return 2;
    }
    endian = std::string(argv[4]) == "big" ? std::endian::big : std::endian::little;
  }
  std::filesystem::path output_directory(argv[2]);
  std::filesystem::create_directories(output_directory);
  std::vector<std::filesystem::path> inputs;
  for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
    if (entry.is_regular_file() && entry.path().extension() == ".ucode") {
      inputs.push_back(entry.path());
    }
  }
  std::sort(inputs.begin(), inputs.end());
  std::ofstream report(output_directory / "compilation.csv");
  report << "shader,scale,modification,stage,source_bytes,compiled_bytes,textures,samplers,"
            "memexports,passed\n";
  size_t passed = 0, failed = 0;
  for (uint32_t scale : {1u, 2u}) {
    // Bindful resources and host render targets work on FL 11_0 without ROVs.
    DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kMicrosoft, false,
                                    false, false, true, scale, scale);
    for (const auto& path : inputs) {
      std::string name = path.filename().string();
      bool vertex = name.ends_with(".vs.ucode");
      if (!vertex && !name.ends_with(".ps.ucode")) {
        continue;
      }
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      std::streamoff size = file.tellg();
      if (size <= 0 || size % 4 != 0 || size > 1024 * 1024) {
        std::fprintf(stderr, "Invalid shader input: %s\n", path.string().c_str());
        ++failed;
        continue;
      }
      file.seekg(0);
      std::vector<uint32_t> ucode(size_t(size) / 4);
      file.read(reinterpret_cast<char*>(ucode.data()), size);
      if (!file) {
        ++failed;
        continue;
      }
      auto type = vertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel;
      DxbcShader shader(type, 0, ucode.data(), ucode.size(), endian);
      rex::string::StringBuffer disassembly;
      shader.AnalyzeUcode(disassembly);
      uint32_t registers = shader.GetDynamicAddressableRegisterCount(63);
      // DXBC uses a geometry shader for rectangle primitives. The Vulkan
      // rectangle-list vertex modification is not a DXBC modification.
      uint64_t modification = vertex ? translator.GetDefaultVertexShaderModification(registers)
                                     : translator.GetDefaultPixelShaderModification(registers);
      auto* translation = shader.GetOrCreateTranslation(modification);
      bool valid = translation && translator.TranslateAnalyzedShader(*translation) &&
                   translation->is_valid();
      d3d11::ShaderBytecode bytecode;
      std::string error;
      d3d11::ShaderBytecodeOptions options;
      if (vertex && !shader.memexport_eM_written()) {
        options.read_only_uav_aliases.push_back(
            {uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory),
             uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory)});
      }
      if (valid) {
        valid = d3d11::ConvertShaderBytecode(translation->translated_binary(), options, bytecode,
                                             error);
      }
      char suffix[80];
      std::snprintf(suffix, sizeof(suffix), ".%ux.%016llX", scale,
                    static_cast<unsigned long long>(modification));
      std::string stem = name.substr(0, name.size() - 6) + suffix;
      if (valid) {
        valid = Write(output_directory / (stem + ".dxbc"), bytecode.data) &&
                Write(output_directory / (stem + ".sm51"), translation->translated_binary());
      }
      size_t textures = shader.GetTextureBindingsAfterTranslation().size();
      size_t samplers = shader.GetSamplerBindingsAfterTranslation().size();
      report << name << ',' << scale << ',' << modification << ',' << (vertex ? "vs" : "ps") << ','
             << size << ',' << bytecode.data.size() << ',' << textures << ',' << samplers << ','
             << unsigned(shader.memexport_eM_written()) << ',' << (valid ? 1 : 0) << '\n';
      if (valid) {
        ++passed;
      } else {
        ++failed;
        std::fprintf(stderr, "%s: %s\n", stem.c_str(),
                     error.empty() ? "translation or output failed" : error.c_str());
      }
    }
  }
  report.close();
  std::printf("DX11 shader variants: %zu compiled, %zu failed\n", passed, failed);
  return passed && !failed && report ? 0 : 1;
}
