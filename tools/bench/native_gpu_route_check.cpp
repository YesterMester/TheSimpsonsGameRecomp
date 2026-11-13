// Counter-contract checks only. These do not execute or qualify a GPU backend.
#include <rex/cvar.h>
#include <rex/graphics/native_gpu_diagnostics.h>
#include <rex/logging.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

using Diagnostics = rex::graphics::NativeGpuDiagnostics;
using System = Diagnostics::RouteSystem;
using Scope = Diagnostics::RouteScope;

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Flag(const char* name, const std::string& value) {
  Require(rex::cvar::SetFlagByName(name, value), name);
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_route_check output_directory");
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    const auto log_path = (output / "check.log").string();
    rex::InitLogging(log_path.c_str(), spdlog::level::info);
    Flag("native_gpu_diagnostics", "true");
    Flag("native_gpu_strict", "false");
    Flag("native_gpu_report_path", (output / "main.json").string());
    Diagnostics first;
    first.Initialize("vulkan");
    int first_registers, second_registers, temporary_registers, disabled_registers;
    first.BindRegisterFile(&first_registers);
    Require(Diagnostics::ForRegisterFile(&first_registers) == &first, "first owner binding");
    Flag("native_gpu_report_path", (output / "other.json").string());
    Diagnostics second;
    second.Initialize("d3d12");
    second.BindRegisterFile(&second_registers);

    for (size_t index = 0; index < size_t(System::kCount); ++index) {
      Scope scope(&first, System(index));
      scope.Native();
      scope.Finish(true);
    }
    {
      Scope scope(&first, System::kGeometry);
      scope.Retained();
      scope.Finish(true);
    }
    {
      Scope scope(&first, System::kTextures);
      scope.Native();
      first.RecordDependency(Diagnostics::Dependency::kTextureFallback, 16, "unit counter probe");
      first.RecordDependency(Diagnostics::Dependency::kMemoryMirror, 32, "same attempted upload");
      scope.Finish(true);
    }
    {
      Scope scope(&first, System::kResolves);
      scope.Native();
      first.RecordAllocation(Diagnostics::Allocation::kEdram, 64, 64);
      scope.Finish(true);
    }
    {
      Scope scope(&first, System::kShaderCommands);
      scope.Native();
      // An early return or exception must remain a failed attempt.
    }
    {
      Scope scope(&first, System::kSynchronization);
      scope.Native();
      scope.Skip();
    }
    {
      Scope scope(&first, System::kSubmission);
      scope.Finish(true);
    }
    {
      Scope scope(&first, System::kDrawDispatch);
      scope.Native();
      scope.Finish(true);
      scope.Finish(true);
    }
    {
      Scope parent(&first, System::kResolves);
      parent.Native();
      {
        Scope child(&first, System::kTextures);
        child.Native();
        first.RecordDependency(Diagnostics::Dependency::kMemoryMirror, 8, "nested dependency");
        child.Finish(true);
      }
      parent.Finish(true);
    }
    {
      Scope parent(&first, System::kGeometry);
      parent.Native();
      {
        Scope child(&second, System::kTextures);
        child.Native();
        second.RecordDependency(Diagnostics::Dependency::kTextureFallback, 4, "second owner only");
        child.Finish(true);
      }
      parent.Finish(true);
    }
    {
      Scope parent(&first, System::kTextures);
      parent.Native();
      {
        Diagnostics::ReadbackScope readback(first);
        Scope excluded(&first, System::kTextures);
        excluded.Native();
        first.RecordDependency(Diagnostics::Dependency::kMemoryMirror, 64, "diagnostic readback");
        first.RecordAllocation(Diagnostics::Allocation::kMemoryMirror, 32, 32);
        first.RecordOperation(Diagnostics::Operation::kCpuTextureUpload);
        excluded.Finish(true);
      }
      parent.Finish(true);
    }
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread)
      workers.emplace_back([&] {
        for (unsigned index = 0; index < 1000; ++index) {
          Scope scope(&first, System::kGeometry);
          scope.Native();
          scope.Finish(true);
        }
      });
    for (auto& worker : workers)
      worker.join();
    {
      Diagnostics temporary;
      temporary.Initialize("d3d11");
      temporary.BindRegisterFile(&temporary_registers);
      Require(Diagnostics::ForRegisterFile(&temporary_registers) == &temporary, "temporary binding");
    }
    Require(!Diagnostics::ForRegisterFile(&temporary_registers), "destroyed owner remained bound");
    {
      Scope scope(&first, System::kGeometry);
      scope.Native();
      first.WriteReport(false);
      std::filesystem::copy_file(output / "main.json", output / "in-flight.json");
      scope.Finish(true);
    }
    first.WriteReport(true);
    second.WriteReport(true);

    Flag("native_gpu_diagnostics", "false");
    Flag("native_gpu_report_path", "");
    Diagnostics disabled;
    disabled.Initialize("vulkan");
    disabled.BindRegisterFile(&disabled_registers);
    Require(!Diagnostics::ForRegisterFile(&disabled_registers), "disabled owner binding");
    {
      Scope scope(&disabled, System::kGeometry);
      scope.Native();
      scope.Finish(true);
    }
    std::printf("{\"passed\":true,\"counter_contract_only\":true,"
                "\"hardware_gpu_executed\":false,\"concurrent_attempts\":4000}\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
