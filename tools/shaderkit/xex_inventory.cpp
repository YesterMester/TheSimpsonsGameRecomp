// Extract the shader programs embedded in the game's executable. The cache
// only contains programs seen during play; the executable also has unused
// effects and the original vertex programs before declaration patching.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include <nlohmann/json.hpp>
#include <xxhash.h>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

namespace {

uint32_t ReadBe32(const uint8_t *data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | data[3];
}

template <typename T> T ReadLe(std::istream &file) {
  std::array<uint8_t, sizeof(T)> bytes;
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
    throw std::runtime_error("truncated shader storage");
  }
  T result = 0;
  for (size_t i = 0; i < bytes.size(); ++i)
    result |= T(bytes[i]) << (i * 8);
  return result;
}

template <typename T> void WriteLe(std::ostream &file, T value) {
  for (size_t i = 0; i < sizeof(T); ++i)
    file.put(char(value >> (i * 8)));
}

std::string HashName(uint64_t hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string name(16, '0');
  for (size_t i = 0; i < 16; ++i)
    name[15 - i] = digits[(hash >> (i * 4)) & 15];
  return name;
}

struct Program {
  uint32_t type;
  std::vector<uint8_t> bytes;
};

using Programs = std::map<uint64_t, Program>;

bool AddProgram(Programs &programs, uint64_t hash, uint32_t type,
                std::span<const uint8_t> bytes) {
  auto [it, inserted] = programs.try_emplace(hash, Program{type, {}});
  if (inserted) {
    it->second.bytes.assign(bytes.begin(), bytes.end());
  } else if (it->second.type != type ||
             !std::ranges::equal(it->second.bytes, bytes)) {
    throw std::runtime_error("conflicting shader identity " + HashName(hash));
  }
  return inserted;
}

void MergeStorage(const std::filesystem::path &path, Programs &programs) {
  std::ifstream file(path, std::ios::binary);
  if (!file || ReadLe<uint32_t>(file) != 0x48534558 ||
      ReadLe<uint32_t>(file) != 0x19122020) {
    throw std::runtime_error("unsupported shader storage: " + path.string());
  }
  while (file.peek() != std::char_traits<char>::eof()) {
    uint64_t hash = ReadLe<uint64_t>(file);
    uint32_t packed = ReadLe<uint32_t>(file);
    uint32_t count = packed & 0x7FFFFFFF;
    if (!count || count > 0xFFFF)
      throw std::runtime_error("invalid shader size");
    std::vector<uint8_t> bytes(size_t(count) * 4);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) ||
        XXH3_64bits(bytes.data(), bytes.size()) != hash) {
      throw std::runtime_error("invalid shader record: " + path.string());
    }
    AddProgram(programs, hash, packed >> 31, bytes);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || (argc - 3) % 2) {
    std::cerr << "usage: xex_inventory <default.xex> <output directory> "
                 "[--cache <45410809.xsh>]...\n";
    return 2;
  }
  try {
    auto input = std::filesystem::canonical(argv[1]);
    auto output = std::filesystem::absolute(argv[2]);
    auto storage = output / "cache/shaders/shareable/45410809.xsh";
    Programs programs;
    for (int i = 3; i < argc; i += 2) {
      if (std::string(argv[i]) != "--cache")
        throw std::runtime_error("unknown option");
      if (std::filesystem::weakly_canonical(argv[i + 1]) ==
          std::filesystem::weakly_canonical(storage)) {
        throw std::runtime_error(
            "output must be separate from the input cache");
      }
      MergeStorage(argv[i + 1], programs);
    }
    size_t cached_count = programs.size();
    std::set<uint64_t> cached_hashes;
    for (const auto &[hash, program] : programs)
      cached_hashes.insert(hash);
    std::filesystem::create_directories(output);
    rex::Runtime runtime(input.parent_path(), output / "tool-userdata");
    rex::RuntimeConfig config{};
    config.kernel_init = rex::kernel::InitializeKernel;
    config.tool_mode = true;
    if (runtime.Setup(std::move(config)) != 0 ||
        runtime.LoadXexImage("game:\\" + input.filename().string()) != 0) {
      throw std::runtime_error("could not decode the executable");
    }
    auto module = runtime.kernel_state()->GetExecutableModule();
    if (!module || !module->xex_module())
      throw std::runtime_error("no decoded image");
    auto *xex = module->xex_module();
    std::span<const uint8_t> image(runtime.memory()->virtual_membase() +
                                       xex->base_address(),
                                   xex->image_size());
    nlohmann::json containers = nlohmann::json::array();
    size_t cached_matches = 0;
    for (size_t offset = 0; offset + 36 <= image.size(); offset += 4) {
      const uint8_t *header = image.data() + offset;
      uint32_t signature = ReadBe32(header);
      if (signature != 0x102A1100 && signature != 0x102A1101)
        continue;
      uint32_t metadata_size = ReadBe32(header + 4);
      uint32_t blob_size = ReadBe32(header + 8);
      uint32_t program_header = ReadBe32(header + 24);
      if (metadata_size < 36 || metadata_size > image.size() - offset ||
          !blob_size || blob_size > image.size() - offset - metadata_size ||
          program_header < 36 || program_header > metadata_size - 8) {
        throw std::runtime_error("invalid container at " +
                                 std::to_string(offset));
      }
      uint32_t code_offset = ReadBe32(header + program_header);
      uint32_t code_size = ReadBe32(header + program_header + 4);
      if (code_offset > blob_size || !code_size ||
          code_size > blob_size - code_offset || code_size % 12 ||
          code_size / 4 > 0xFFFF) {
        throw std::runtime_error("invalid program at " +
                                 std::to_string(offset));
      }
      // The profile strings confirm this mapping in the game's 0x102A110x
      // containers. ShaderStoredHeader uses 0 for vertex and 1 for pixel.
      uint32_t type = (signature & 1) ? 0 : 1;
      std::string_view metadata(reinterpret_cast<const char *>(header),
                                metadata_size);
      if (metadata.find(type ? "vs_3_0" : "ps_3_0") != std::string_view::npos) {
        throw std::runtime_error("conflicting shader profile");
      }
      size_t code_address = offset + metadata_size + code_offset;
      auto bytes = image.subspan(code_address, code_size);
      uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
      bool already_cached = cached_hashes.contains(hash);
      AddProgram(programs, hash, type, bytes);
      cached_matches += already_cached;
      containers.push_back({{"address", xex->base_address() + offset},
                            {"offset", offset},
                            {"metadata_size", metadata_size},
                            {"blob_size", blob_size},
                            {"code_offset", code_offset},
                            {"code_size", code_size},
                            {"hash", HashName(hash)},
                            {"type", type ? "ps" : "vs"},
                            {"in_input_cache", already_cached}});
    }
    if (containers.empty())
      throw std::runtime_error("no supported shader containers found");
    std::filesystem::create_directories(storage.parent_path());
    std::filesystem::create_directories(output / "ucode");
    std::ofstream file(storage.string() + ".tmp", std::ios::binary);
    WriteLe(file, uint32_t(0x48534558));
    WriteLe(file, uint32_t(0x19122020));
    size_t vertex_count = 0, pixel_count = 0;
    for (const auto &[hash, program] : programs) {
      std::string name =
          HashName(hash) + (program.type ? ".ps.ucode" : ".vs.ucode");
      std::ofstream raw(output / "ucode" / name, std::ios::binary);
      raw.write(reinterpret_cast<const char *>(program.bytes.data()),
                program.bytes.size());
      if (!raw)
        throw std::runtime_error("could not write " + name);
      WriteLe(file, hash);
      WriteLe(file, uint32_t(program.bytes.size() / 4) | (program.type << 31));
      file.write(reinterpret_cast<const char *>(program.bytes.data()),
                 program.bytes.size());
      program.type ? ++pixel_count : ++vertex_count;
    }
    file.close();
    if (!file)
      throw std::runtime_error("could not write shader storage");
    std::filesystem::rename(storage.string() + ".tmp", storage);
    nlohmann::json report = {{"image_base", xex->base_address()},
                             {"image_size", image.size()},
                             {"input_cache_shaders", cached_count},
                             {"containers", containers},
                             {"cached_matches", cached_matches},
                             {"unique_shaders", programs.size()},
                             {"vertex_shaders", vertex_count},
                             {"pixel_shaders", pixel_count}};
    std::ofstream manifest(output / "inventory.json");
    manifest << report.dump(2) << '\n';
    if (!manifest)
      throw std::runtime_error("could not write inventory");
    std::cout << containers.size() << " executable containers, "
              << cached_matches << " cache matches, " << programs.size()
              << " unique programs (" << vertex_count << " vertex, "
              << pixel_count << " pixel)\n";
    std::cout << "wrote " << storage << '\n';
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
