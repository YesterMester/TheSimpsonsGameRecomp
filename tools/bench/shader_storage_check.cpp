// Exercise executable shader seeding and recovery of interrupted cache writes.
#undef NDEBUG

#include <rex/graphics/shader_storage.h>
#include <rex/hash.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
void Be32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    bytes[offset + i] = uint8_t(value >> (24 - i * 8));
}

void Container(std::vector<uint8_t>& bytes, size_t offset, bool vertex) {
  Be32(bytes, offset, vertex ? 0x102A1101 : 0x102A1100);
  Be32(bytes, offset + 4, 44);
  Be32(bytes, offset + 8, 12);
  Be32(bytes, offset + 24, 36);
  Be32(bytes, offset + 36, 0);
  Be32(bytes, offset + 40, 12);
  for (size_t i = 0; i < 12; ++i)
    bytes[offset + 44 + i] = uint8_t(i + (vertex ? 1 : 32));
}

std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint64_t Le(const std::vector<uint8_t>& bytes, size_t offset, size_t count) {
  uint64_t result = 0;
  for (size_t i = 0; i < count; ++i)
    result |= uint64_t(bytes.at(offset + i)) << (i * 8);
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path path(argv[1]);
  assert(!std::filesystem::exists(path));
  std::vector<uint8_t> image(192);
  Container(image, 16, true);
  Container(image, 96, false);
  auto seed = [&]() {
    rex::graphics::SeedShaderStorageFromExecutable(path, image.data(), image.size());
  };
  seed();
  const auto complete = Read(path);
  assert(complete.size() == 8 + 2 * (12 + 12));
  assert(Le(complete, 0, 4) == 0x48534558 && Le(complete, 4, 4) == 0x19122020);
  assert(Le(complete, 16, 4) == 3 && Le(complete, 40, 4) == (0x80000000u | 3));
  assert(Le(complete, 8, 8) == XXH3_64bits(image.data() + 60, 12));
  assert(Le(complete, 32, 8) == XXH3_64bits(image.data() + 140, 12));
  seed();
  assert(Read(path) == complete);  // A second launch never duplicates records.
  std::filesystem::resize_file(path, complete.size() - 7);
  seed();
  assert(Read(path) == complete);  // Keep the first record and repair the second.
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(8);
    file.put(char(complete[8] ^ 1));
  }
  seed();
  assert(Read(path) == complete);  // Rebuild from the first invalid hash.
  Be32(image, 16 + 40, UINT32_MAX);
  Be32(image, 96 + 4, UINT32_MAX);
  seed();
  assert(Read(path) == complete);  // Invalid container sizes cannot extend a cache.
  Container(image, 16, true);
  Container(image, 96, false);
  std::vector<uint8_t> variant(image.begin() + 60, image.begin() + 72);
  variant[7] ^= 0x40;
  const uint64_t target_hash = XXH3_64bits(variant.data(), variant.size());
  auto recipe_path = path;
  recipe_path += ".recipes";
  {
    std::ofstream recipes(recipe_path);
    recipes << "REX_VERTEX_PATCHES 1\n"
            << std::hex << Le(complete, 8, 8) << ' ' << target_hash << " 3 1:00000040\n";
    // A bounds violation with a fabricated target must never add a record.
    recipes << Le(complete, 8, 8) << " 1 3 100:00000040\n";
  }
  rex::graphics::SeedShaderStorageFromExecutable(path, image.data(), image.size(), recipe_path);
  const auto patched = Read(path);
  assert(patched.size() == complete.size() + 24);
  assert(Le(patched, complete.size(), 8) == target_hash);
  assert(std::equal(variant.begin(), variant.end(), patched.end() - variant.size()));
}
