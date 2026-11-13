#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rex::graphics;

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

class AnalyzedShader final : public Shader {
 public:
  using Shader::Shader;

 private:
  Translation* CreateTranslationInstance(uint64_t) override { return nullptr; }
};

std::vector<uint32_t> Program(bool register_factor) {
  using namespace ucode;
  std::vector<uint32_t> code = {1u | (2u << 12), uint32_t(ControlFlowOpcode::kExecEnd) << 12, 0};
  auto mad = [&](ExportRegister target, uint32_t scale, uint32_t offset) {
    code.push_back(uint32_t(target) | (1u << 15) | (15u << 16) |
                   (uint32_t(AluScalarOpcode::kRetainPrev) << 26));
    code.push_back(0x6Cu << 16);
    code.push_back(offset | (scale << 8) | (uint32_t(AluVectorOpcode::kMad) << 24) | (1u << 31));
  };
  mad(ExportRegister::kExportAddress, 0, 1);
  if (register_factor)
    code[5] |= 1u << 30;
  mad(ExportRegister::kExportData0, 2, 3);
  return code;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: memexport_bounds_check report.json");
    rex::InitLogging("memexport-bounds-runtime.log", spdlog::level::info);
    uint32_t accepted = 0, rejected = 0, scalar_checks = 0;
    for (auto stage : {xenos::ShaderType::kVertex, xenos::ShaderType::kPixel}) {
      RegisterFile registers;
      for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i)
        registers[i] = 0;
      uint32_t bank = stage == xenos::ShaderType::kVertex ? 0 : 256;
      uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + bank * 4;
      uint32_t address = stage == xenos::ShaderType::kVertex ? 0x40000 : 0x180000;
      const std::array<uint32_t, 4> stream = {
          0x40000000 | (address >> 2), 0x4B000000,
          0x4B000000 | (uint32_t(xenos::ColorFormat::k_32_32_32_32_FLOAT) << 8), 0x4B000040};
      for (uint32_t i = 0; i < 4; ++i)
        registers[base + 4 + i] = stream[i];
      registers[base + 1] = std::bit_cast<uint32_t>(1.0f);
      auto code = Program(false);
      AnalyzedShader shader(stage, 1, code.data(), code.size(), std::endian::little);
      rex::string::StringBuffer disassembly;
      shader.AnalyzeUcode(disassembly);
      Require(shader.memexport_eM_written() && shader.has_static_memexport_addresses(),
              "The analyzed address MAD was not recognized");
      for (float zero : {0.0f, -0.0f}) {
        for (uint32_t component : {0u, 2u, 3u})
          registers[base + component] = std::bit_cast<uint32_t>(zero);
        Require(draw_util::CanUseNativeMemExports(registers, shader),
                "A stream with unchanged descriptor fields was rejected");
        ++accepted;
        // Independent floating-point evaluation of the original MAD. Every
        // admitted index must retain the descriptor's address, format and count.
        for (uint32_t index = 0; index <= 64; ++index)
          for (uint32_t component : {0u, 2u, 3u}) {
            float result = std::fma(float(index), zero, std::bit_cast<float>(stream[component]));
            Require(std::bit_cast<uint32_t>(result) == stream[component],
                    "An admitted MAD changes the native resource descriptor");
            ++scalar_checks;
          }
      }
      for (uint32_t component : {0u, 2u, 3u}) {
        for (float value : {0.25f, -1.0f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
          registers[base + component] = std::bit_cast<uint32_t>(value);
          Require(!draw_util::CanUseNativeMemExports(registers, shader),
                  "A modified or non-finite stream descriptor was admitted");
          ++rejected;
        }
        registers[base + component] = 0;
      }
      std::vector<draw_util::MemExportRange> ranges;
      draw_util::AddMemExportRanges(registers, shader, ranges);
      Require(ranges.size() == 1 && ranges[0].base_address_dwords == address / 4 &&
                  ranges[0].size_bytes == 64 * 16,
              "The default shader constant bank selects the wrong export range");
      auto dynamic_code = Program(true);
      AnalyzedShader dynamic(stage, 2, dynamic_code.data(), dynamic_code.size(),
                             std::endian::little);
      dynamic.AnalyzeUcode(disassembly);
      Require(!draw_util::CanUseNativeMemExports(registers, dynamic),
              "Unproved register factors were admitted");
      ++rejected;
    }
    std::ofstream file(argv[1]);
    file << "{\"passed\":true,\"accepted\":" << accepted << ",\"rejected\":" << rejected
         << ",\"independent_scalar_checks\":" << scalar_checks << "}\n";
    Require(bool(file), "Could not write the address proof report");
    return 0;
  } catch (const std::exception& error) {
    if (argc > 1)
      std::ofstream(std::string(argv[1]) + ".error.txt") << error.what() << '\n';
    return 1;
  }
}
