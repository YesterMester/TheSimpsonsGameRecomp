// xenos_translate - offline Xenos shader -> SPIR-V compiler.
//
// This is the shader half of taking the GPU native: the direct analog of what
// XenonRecomp does for the CPU. Instead of the engine translating each Xenos
// shader to SPIR-V at runtime, this does it ahead of time, once, so the
// runtime cost disappears.
//
// It reuses the engine's own, already-correct shader translator (the
// Xenia-derived code in rexglue-sdk) rather than reimplementing it: it links
// straight against the shipped runtime library and calls the same
// AnalyzeUcode / TranslateAnalyzedShader path the engine uses internally. So
// the output is produced by the exact same code that renders the game today,
// just run offline.
//
// Input:  a raw Xenos microcode blob (as produced by `xsh_inventory extract`),
//         with the shader type taken from the filename (.vs.ucode / .ps.ucode).
// Output: a .spv SPIR-V module.
//
// Build: see CMakeLists.txt in this folder. It compiles against the prebuilt
// SDK headers and links tools/rexglue-bin/.../librexruntimerd.so.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace {

std::vector<uint32_t> read_ucode(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return {};
  }
  auto size = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint32_t> dwords(size / 4);
  f.read(reinterpret_cast<char*>(dwords.data()), static_cast<std::streamsize>(dwords.size() * 4));
  return dwords;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return bool(f);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace rex::graphics;

  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: xenos_translate <shader.(vs|ps).ucode> <out.spv> [--endian big|little]\n"
                 "  translates one Xenos microcode blob to SPIR-V using the engine's own translator.\n");
    return 2;
  }
  std::filesystem::path in_path = argv[1];
  std::filesystem::path out_path = argv[2];
  std::endian src_endian = std::endian::big;
  for (int i = 3; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--endian") {
      src_endian = std::string(argv[i + 1]) == "little" ? std::endian::little : std::endian::big;
    }
  }

  // Shader type from the filename convention xsh_inventory produces.
  std::string name = in_path.filename().string();
  xenos::ShaderType type;
  if (name.find(".vs.") != std::string::npos) {
    type = xenos::ShaderType::kVertex;
  } else if (name.find(".ps.") != std::string::npos) {
    type = xenos::ShaderType::kPixel;
  } else {
    std::fprintf(stderr, "error: can't tell shader type from name '%s' "
                         "(expected .vs.ucode or .ps.ucode)\n", name.c_str());
    return 2;
  }

  std::vector<uint32_t> ucode = read_ucode(in_path);
  if (ucode.empty()) {
    std::fprintf(stderr, "error: couldn't read microcode from %s\n", in_path.c_str());
    return 1;
  }

  // Hash is only an identity key here; the translator doesn't need the real one.
  Shader shader(type, 0, ucode.data(), ucode.size(), src_endian);

  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);

  // A baseline feature set (no live Vulkan device needed): request all features
  // so the translator emits its most capable form. A per-pipeline build would
  // match the host device instead; for a standalone translate this is fine.
  SpirvShaderTranslator::Features features(/*all=*/true);
  SpirvShaderTranslator translator(features,
                                   /*native_2x_msaa_with_attachments=*/true,
                                   /*native_2x_msaa_no_attachments=*/true,
                                   /*edram_fragment_shader_interlock=*/true);

  // The "default" modification for this shader, using its own register bound.
  uint32_t reg_count = shader.GetDynamicAddressableRegisterCount(0);
  uint64_t modification =
      type == xenos::ShaderType::kVertex
          ? translator.GetDefaultVertexShaderModification(reg_count)
          : translator.GetDefaultPixelShaderModification(reg_count);

  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_translated()) {
    std::fprintf(stderr, "error: translation failed for %s\n", name.c_str());
    return 1;
  }

  const std::vector<uint8_t>& spirv = translation->translated_binary();
  if (spirv.size() < 4 || *reinterpret_cast<const uint32_t*>(spirv.data()) != 0x07230203u) {
    std::fprintf(stderr, "warning: output doesn't start with the SPIR-V magic "
                         "(0x07230203) - endianness may be wrong\n");
  }
  if (!write_bytes(out_path, spirv)) {
    std::fprintf(stderr, "error: couldn't write %s\n", out_path.c_str());
    return 1;
  }

  std::printf("%s -> %s  (%s, %zu ucode dwords -> %zu SPIR-V bytes)\n",
              name.c_str(), out_path.filename().string().c_str(),
              type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
              ucode.size(), spirv.size());
  return 0;
}
