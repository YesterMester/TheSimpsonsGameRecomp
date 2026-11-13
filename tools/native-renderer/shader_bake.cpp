// Export the current translator's shader storage without executing a game or
// replaying a frame. The host GPU still selects the exact device configuration.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/runtime.h>

int main(int argc, char** argv) {
  if ((argc != 5 && argc != 6) || (std::string(argv[3]) != "1" && std::string(argv[3]) != "2")) {
    std::fprintf(
        stderr,
        "usage: shader_bake <cache root> <output directory> <1|2> <modifications> [AOT root]\n");
    return 2;
  }
  const auto cache = std::filesystem::absolute(argv[1]);
  const auto output = std::filesystem::absolute(argv[2]);
  const auto requests = std::filesystem::absolute(argv[4]);
  const auto served = argc == 6 ? std::filesystem::absolute(argv[5]) : output / "none";
  if (!std::filesystem::is_regular_file(cache / "shaders/shareable/45410809.xsh") ||
      !std::filesystem::is_regular_file(requests)) {
    std::fprintf(stderr, "The source shader storage or modification requests are missing\n");
    return 2;
  }
  const auto flag = [](const char* name, const std::string& value) {
    if (rex::cvar::SetFlagByName(name, value))
      return true;
    std::fprintf(stderr, "Could not set shader bake option %s\n", name);
    return false;
  };
  if (!flag("resolution_scale", argv[3]) || !flag("aot_shader_path", served.string()) ||
      !flag("aot_export_path", output.string()) || !flag("aot_export_storage", "true") ||
      !flag("aot_export_modifications", requests.string()) ||
      !flag("vulkan_pipeline_creation_threads", "0"))
    return 2;

  auto runtime = std::make_unique<rex::Runtime>("", "", "");
  auto graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  auto* graphics_system = graphics.get();
  if (XFAILED(graphics_system->SetupPresentation(nullptr))) {
    std::fprintf(stderr, "Could not create the headless Vulkan presenter\n");
    return 1;
  }
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  config.tool_mode = false;
  if (XFAILED(runtime->Setup(std::move(config)))) {
    std::fprintf(stderr, "Could not initialize the shader bake runtime\n");
    return 1;
  }
  graphics_system->InitializeShaderStorage(cache, 0x45410809, true);
  // Shutdown exports every translated module and its real configuration. The
  // archive builder checks error logs, exact variant retention and source hash.
  runtime.reset();
  std::printf("Finished source shader bake at %sx\n", argv[3]);
  return 0;
}
