#include <rex/graphics/shader_storage.h>

#include <cstdio>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>

namespace rex::graphics {
// Xbox 360 shader programs in the executable sit in D3D containers marked
// 0x102A1100 (pixel) or 0x102A1101 (vertex); tools/shaderkit/xex_inventory.cpp
// reads the same layout. The storage gets the programs it doesn't have yet, in
// its own record format, before the pipeline cache opens it.
void SeedShaderStorageFromExecutable(const std::filesystem::path& storage_path,
                                     const uint8_t* image, size_t image_size,
                                     const std::filesystem::path& vertex_recipes) {
  auto read_be32 = [](const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) |
           data[3];
  };
  constexpr uint32_t kMagic = 0x48534558;           // 'XESH'
  constexpr uint32_t kVersionSwapped = 0x19122020;  // ShaderStoredHeader::kVersion
  std::set<uint64_t> stored;
  std::error_code error;
  std::filesystem::create_directories(storage_path.parent_path(), error);
  bool fresh = true;
  uint64_t valid_bytes = 0;
  if (FILE* file = rex::filesystem::OpenFile(storage_path, "rb")) {
    uint32_t header[2] = {};
    if (fread(header, sizeof(header), 1, file) == 1 && header[0] == kMagic &&
        header[1] == kVersionSwapped) {
      fresh = false;
      valid_bytes = sizeof(header);
      std::vector<uint8_t> code;
      while (true) {
        uint64_t hash;
        uint32_t packed;
        if (fread(&hash, sizeof(hash), 1, file) != 1 ||
            fread(&packed, sizeof(packed), 1, file) != 1) {
          break;
        }
        uint32_t dwords = packed & 0x7FFFFFFF;
        if (!dwords || dwords > 0xFFFF) {
          break;
        }
        code.resize(size_t(dwords) * 4);
        if (fread(code.data(), 1, code.size(), file) != code.size() ||
            XXH3_64bits(code.data(), code.size()) != hash) {
          break;
        }
        stored.insert(hash);
        valid_bytes += sizeof(hash) + sizeof(packed) + code.size();
      }
    }
    fclose(file);
  }
  FILE* file = rex::filesystem::OpenFile(storage_path, fresh ? "wb" : "a+b");
  if (!file) {
    return;
  }
  if (fresh) {
    const uint32_t header[2] = {kMagic, kVersionSwapped};
    fwrite(header, sizeof(header), 1, file);
  } else {
    // Recover an interrupted final record before appending. Otherwise the
    // loader would stop at that record and discard every newly seeded shader.
    rex::filesystem::TruncateStdioFile(file, valid_bytes);
  }
  size_t added = 0;
  bool write_failed = false;
  std::map<uint64_t, std::vector<uint8_t>> source_vertices;
  for (size_t offset = 0; offset + 36 <= image_size; offset += 4) {
    const uint8_t* header = image + offset;
    uint32_t signature = read_be32(header);
    if (signature != 0x102A1100 && signature != 0x102A1101) {
      continue;
    }
    uint32_t metadata_size = read_be32(header + 4);
    uint32_t blob_size = read_be32(header + 8);
    uint32_t program_header = read_be32(header + 24);
    if (metadata_size < 36 || metadata_size > image_size - offset || !blob_size ||
        blob_size > image_size - offset - metadata_size || program_header < 36 ||
        program_header > metadata_size - 8) {
      continue;
    }
    uint32_t code_offset = read_be32(header + program_header);
    uint32_t code_size = read_be32(header + program_header + 4);
    if (code_offset > blob_size || !code_size || code_size > blob_size - code_offset ||
        code_size % 12 || code_size / 4 > 0xFFFF) {
      continue;
    }
    const uint8_t* code = image + offset + metadata_size + code_offset;
    uint64_t hash = XXH3_64bits(code, code_size);
    if (!vertex_recipes.empty() && (signature & 1)) {
      source_vertices.try_emplace(hash, code, code + code_size);
    }
    if (!stored.insert(hash).second) {
      continue;
    }
    // ShaderStoredHeader: the hash, then the dword count with the type (0
    // vertex, 1 pixel) in the top bit, then the program as stored.
    uint32_t packed = (code_size / 4) | ((signature & 1) ? 0u : 0x80000000u);
    if (fwrite(&hash, sizeof(hash), 1, file) != 1 ||
        fwrite(&packed, sizeof(packed), 1, file) != 1 ||
        fwrite(code, 1, code_size, file) != code_size) {
      REXLOG_WARN("Shader storage: could not finish seeding the executable's shaders");
      write_failed = true;
      break;
    }
    ++added;
  }
  if (!write_failed && !vertex_recipes.empty()) {
    std::ifstream recipes(vertex_recipes);
    std::string magic;
    uint32_t version = 0;
    if (recipes >> magic >> version; magic == "REX_VERTEX_PATCHES" && version == 1) {
      std::string line;
      while (std::getline(recipes, line)) {
        if (line.empty())
          continue;
        std::istringstream fields(line);
        uint64_t source_hash, target_hash;
        uint32_t words;
        if (!(fields >> std::hex >> source_hash >> target_hash >> std::dec >> words) ||
            stored.contains(target_hash))
          continue;
        const auto source = source_vertices.find(source_hash);
        if (source == source_vertices.end() || words != source->second.size() / 4)
          continue;
        std::vector<uint8_t> code = source->second;
        std::set<uint32_t> changed;
        bool valid = true;
        std::string patch;
        while (fields >> patch) {
          const size_t colon = patch.find(':');
          if (colon == std::string::npos) {
            valid = false;
            break;
          }
          uint32_t index = 0, mask = 0;
          auto index_result = std::from_chars(patch.data(), patch.data() + colon, index);
          auto mask_result =
              std::from_chars(patch.data() + colon + 1, patch.data() + patch.size(), mask, 16);
          if (index_result.ec != std::errc() || index_result.ptr != patch.data() + colon ||
              mask_result.ec != std::errc() || mask_result.ptr != patch.data() + patch.size() ||
              index >= words || !mask || !changed.insert(index).second) {
            valid = false;
            break;
          }
          for (size_t byte = 0; byte < 4; ++byte) {
            code[size_t(index) * 4 + byte] ^= uint8_t(mask >> (24 - byte * 8));
          }
        }
        if (!valid || changed.empty() || XXH3_64bits(code.data(), code.size()) != target_hash) {
          continue;
        }
        if (fwrite(&target_hash, sizeof(target_hash), 1, file) != 1 ||
            fwrite(&words, sizeof(words), 1, file) != 1 ||
            fwrite(code.data(), 1, code.size(), file) != code.size())
          break;
        stored.insert(target_hash);
        ++added;
      }
    }
  }
  fclose(file);
  if (added) {
    REXLOG_INFO("Shader storage: added {} programs from the executable", added);
  }
}
}  // namespace rex::graphics
