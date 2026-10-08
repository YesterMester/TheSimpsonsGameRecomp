#include <rex/graphics/d3d11/shader_cache.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using rex::graphics::d3d11::ShaderCache;
using rex::ui::d3d11::D3D11Device;

namespace {
std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '\"')
      result += '\\';
    result += c < 32 ? ' ' : char(c);
  }
  return result + '\"';
}
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    D3D11Device::Options options;
    options.debug = true;
    std::filesystem::path folder;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--fl11-0")
        options.maximum_feature_level = D3D_FEATURE_LEVEL_11_0;
      else if (arg == "--kernel-directory" && i + 1 < argc)
        folder = argv[++i];
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else
        throw std::runtime_error(
            "usage: d3d11_kernel_check --kernel-directory shaders "
            "[--warp] [--fl11-0] [--output report.json]");
    }
    if (folder.empty())
      throw std::runtime_error("The SDK shader directory is required");
    std::string error;
    auto device = D3D11Device::Create(options, error);
    if (!device)
      throw std::runtime_error(error);
    ShaderCache shaders(*device);
    std::vector<std::filesystem::path> inputs;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sm51") {
        inputs.push_back(entry.path());
      }
    }
    std::sort(inputs.begin(), inputs.end());
    if (inputs.empty())
      throw std::runtime_error("The SDK shader directory is empty");
    std::vector<std::string> failures;
    std::array<uint64_t, 6> stages = {};
    for (const auto& path : inputs) {
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      auto size = file.tellg();
      if (size <= 0 || size > 16 * 1024 * 1024) {
        throw std::runtime_error("Invalid SDK shader file: " + path.string());
      }
      file.seekg(0);
      std::vector<uint8_t> code(size_t(size), 0);
      file.read(reinterpret_cast<char*>(code.data()), size);
      if (!file)
        throw std::runtime_error("Could not read SDK shader: " + path.string());
      const auto* program = shaders.GetOrCreate(code, false, error);
      if (!program) {
        failures.push_back(path.filename().string() + ": " + error);
      } else {
        ++stages[program->bytecode().program_type];
        if (shaders.GetOrCreate(code, false, error) != program || !error.empty()) {
          throw std::runtime_error("The SDK shader cache did not retain its native object");
        }
      }
    }
    std::string report = "{\n  \"passed\": " + std::string(failures.empty() ? "true" : "false") +
                         ",\n  \"scope\": \"native DX11 helper shader creation\",\n";
    report += "  \"adapter\": " + Quote(device->features().adapter_name) + ",\n";
    report += "  \"feature_level\": " + std::to_string(device->features().level) + ",\n";
    report +=
        "  \"software\": " + std::string(device->features().software ? "true" : "false") + ",\n";
    report += "  \"input_programs\": " + std::to_string(inputs.size()) + ",\n";
    report += "  \"created_programs\": " + std::to_string(shaders.statistics().creations) + ",\n";
    report += "  \"stages_ps_vs_gs_hs_ds_cs\": [";
    for (size_t i = 0; i < stages.size(); ++i) {
      report += (i ? "," : "") + std::to_string(stages[i]);
    }
    report += "],\n  \"failures\": [";
    for (size_t i = 0; i < failures.size(); ++i)
      report += (i ? "," : "") + Quote(failures[i]);
    report += "]\n}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      if (!file)
        throw std::runtime_error("Could not write the helper shader report");
    }
    return failures.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (!output.empty()) {
      std::ofstream file(output);
      file << "{\"passed\":false,\"error\":" << Quote(error.what()) << "}\n";
    }
    return 1;
  }
}
