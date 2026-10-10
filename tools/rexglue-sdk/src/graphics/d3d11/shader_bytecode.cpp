#include <rex/graphics/d3d11/shader_bytecode.h>

#include <array>
#include <cstring>
#include <stdexcept>

#include <rex/graphics/format/dxbc.h>

#include "thirdparty/dxbc/DXBCChecksum.h"

namespace rex::graphics::d3d11 {

namespace {
using dxbc::Opcode;
using Words = std::span<const uint32_t>;

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

constexpr uint32_t kIndexBits = 0x7FF00000;
constexpr uint32_t kInstructionLengthBits = 0x7F000000;

struct Chunk {
  uint32_t fourcc;
  Words words;
};

struct Instruction {
  Words words;
  uint32_t opcode;
  uint32_t prefix_length;
};

uint32_t ResourceOperandType(uint32_t opcode) {
  switch (Opcode(opcode)) {
    case Opcode::kDclResource:
    case Opcode::kDclResourceRaw:
      return uint32_t(ResourceKind::kShaderResource);
    case Opcode::kDclConstantBuffer:
      return uint32_t(ResourceKind::kConstantBuffer);
    case Opcode::kDclSampler:
      return uint32_t(ResourceKind::kSampler);
    case Opcode::kDclUnorderedAccessViewTyped:
    case Opcode::kDclUnorderedAccessViewRaw:
      return uint32_t(ResourceKind::kUnorderedAccess);
    default:
      // Structured buffer declarations. Not emitted by the guest translator,
      // but the same binding conversion applies to native helper shaders.
      return opcode == 158   ? uint32_t(ResourceKind::kUnorderedAccess)
             : opcode == 162 ? uint32_t(ResourceKind::kShaderResource)
                             : 0;
  }
}

uint32_t ResourceDeclarationTailLength(uint32_t opcode) {
  return opcode == 88 || opcode == 89 || opcode == 156 || opcode == 158 || opcode == 162 ? 2 : 1;
}

// Operand counts for instructions emitted by the DXBC translator, plus the
// ordinary SM 5.0 sampling instructions used by native helper shaders. Literal
// declaration payloads are handled separately, never interpreted as operands.
int OperandCount(uint32_t opcode) {
  switch (opcode) {
    case 2:
    case 7:
    case 9:
    case 10:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 48:
    case 58:
    case 62:
      return 0;
    case 3:
    case 4:
    case 6:
    case 8:
    case 13:
    case 31:
    case 44:
    case 63:
    case 76:
    case 117:
    case 118:
    case 119:
      return 1;
    case 5:
    case 11:
    case 12:
    case 25:
    case 26:
    case 27:
    case 28:
    case 43:
    case 40:
    case 47:
    case 54:
    case 59:
    case 64:
    case 65:
    case 66:
    case 67:
    case 68:
    case 75:
    case 86:
    case 122:
    case 123:
    case 124:
    case 125:
    case 129:
    case 130:
    case 131:
    case 135:
    case 136:
    case 141:
    case 205:
      return 2;
    case 0:
    case 1:
    case 14:
    case 15:
    case 16:
    case 17:
    case 24:
    case 29:
    case 30:
    case 32:
    case 33:
    case 34:
    case 36:
    case 37:
    case 39:
    case 41:
    case 42:
    case 45:
    case 49:
    case 51:
    case 52:
    case 56:
    case 57:
    case 60:
    case 61:
    case 77:
    case 79:
    case 80:
    case 83:
    case 84:
    case 85:
    case 87:
    case 110:
    case 163:
    case 164:
    case 165:
    case 166:
    case 169:
    case 170:
    case 204:
      return 3;
    case 35:
    case 38:
    case 46:
    case 50:
    case 55:
    case 69:
    case 78:
    case 81:
    case 82:
    case 108:
    case 109:
    case 167:
    case 168:
    case 138:
    case 139:
      return 4;
    case 70:
    case 71:
    case 72:
    case 74:
    case 140:
      return 5;
    case 73:
      return 6;
    default:
      return -1;
  }
}

class ProgramConverter {
 public:
  ProgramConverter(Words source, const ShaderBytecodeOptions& options, ShaderBytecode& output)
      : source_(source), options_(options), output_(output) {}

  std::vector<uint32_t> Convert() {
    Require(source_.size() >= 2 && source_[1] == source_.size(), "Invalid shader token length");
    Require((source_[0] & 0xFFFF) == 0x51, "DX11 conversion requires shader model 5.1 input");
    output_.program_type = source_[0] >> 16;
    Require(output_.program_type <= uint32_t(dxbc::ProgramType::kComputeShader),
            "Unknown shader program type");
    Require(options_.uav_register_count >= 1 && options_.uav_register_count <= 64,
            "Invalid DX11 UAV register count");
    std::array<bool, 128> mapped_registers = {};
    for (const auto& mapping : options_.texture_swizzles) {
      Require(mapping.srv_register < mapped_registers.size() &&
                  !mapped_registers[mapping.srv_register] && mapping.swizzle <= 0xFFF,
              "Invalid or duplicate DX11 texture channel mapping");
      mapped_registers[mapping.srv_register] = true;
      for (uint32_t component = 0; component < 4; ++component) {
        Require(((mapping.swizzle >> (component * 3)) & 7) <= 5,
                "Invalid DX11 texture channel selector");
      }
    }

    std::vector<Instruction> instructions;
    for (size_t position = 2; position < source_.size();) {
      uint32_t token = source_[position];
      uint32_t opcode = token & 0x7FF;
      bool custom = opcode == uint32_t(Opcode::kCustomData);
      if (custom) {
        Require(source_.size() - position >= 2, "Truncated shader custom-data header");
      }
      uint32_t length = custom ? source_[position + 1] : (token >> 24) & 127;
      Require(length >= (custom ? 2u : 1u) && length <= source_.size() - position,
              "Invalid shader instruction length");
      Words words = source_.subspan(position, length);
      uint32_t prefix = 1;
      if (!custom) {
        for (uint32_t extension = token; extension & 0x80000000;) {
          Require(prefix < length, "Truncated shader opcode extension");
          extension = words[prefix++];
        }
      }
      instructions.push_back({words, opcode, prefix});
      if (ResourceOperandType(opcode)) {
        GatherBinding(instructions.back());
      }
      position += length;
    }
    for (const auto& mapping : options_.texture_swizzles) {
      bool found = false;
      for (const auto& binding : bindings_) {
        if (binding.resource.kind == ResourceKind::kShaderResource &&
            binding.resource.host_register == mapping.srv_register) {
          Require(binding.dimension != 11 && binding.dimension != 12,
                  "Raw and structured buffer reads cannot have texture channel mappings");
          found = true;
        }
      }
      Require(found, "Texture channel mapping has no declared shader resource");
    }
    for (const auto& binding : bindings_) {
      if (!binding.read_only_alias) {
        continue;
      }
      bool found = false;
      for (const auto& srv : bindings_) {
        found |= srv.resource.kind == ResourceKind::kShaderResource && srv.dimension == 11 &&
                 srv.resource.host_register == binding.resource.host_register;
      }
      Require(found, "Read-only UAV alias has no matching declared raw SRV");
    }

    std::vector<uint32_t> result = {source_[0] & ~uint32_t(15), 0};
    for (const auto& instruction : instructions) {
      try {
        ConvertInstruction(instruction, result);
      } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string(error.what()) + " (opcode " +
                                 std::to_string(instruction.opcode) + ")");
      }
    }
    result[1] = uint32_t(result.size());
    return result;
  }

 private:
  struct Binding {
    ResourceBinding resource;
    uint32_t cbuffer_vectors;
    uint32_t dimension;
    uint32_t return_type;
    uint32_t structure_stride;
    bool read_only_alias;
  };

  const Binding& FindBinding(uint32_t type, uint32_t id) const {
    for (const auto& binding : bindings_) {
      if (uint32_t(binding.resource.kind) == type && binding.resource.id == id) {
        return binding;
      }
    }
    throw std::runtime_error("Shader references an undeclared resource binding");
  }

  void GatherBinding(const Instruction& instruction) {
    uint32_t type = ResourceOperandType(instruction.opcode);
    Words words = instruction.words;
    Require(instruction.prefix_length == 1, "Extended resource declarations are unsupported");
    Require(words.size() == 5 + ResourceDeclarationTailLength(instruction.opcode),
            "Invalid SM 5.1 resource declaration length");
    Require(
        ((words[1] >> 12) & 255) == type && ((words[1] >> 20) & 3) == 3 && !(words[1] & 0xFFC00000),
        "Resource declarations must have three immediate indices");
    Require(words.back() == 0, "DX11 shaders require resource register space zero");
    Require(words[3] == words[4], "DX11 conversion requires single-resource bindings");
    uint32_t host_register = words[3];
    bool read_only_alias = false;
    if (type == uint32_t(ResourceKind::kUnorderedAccess)) {
      for (const auto& alias : options_.read_only_uav_aliases) {
        if (alias.uav_register == words[3]) {
          Require(output_.program_type != 5 && instruction.opcode == 157,
                  "Only graphics-stage raw-buffer UAV reads may alias an SRV");
          host_register = alias.srv_register;
          read_only_alias = true;
          break;
        }
      }
    }
    uint32_t limit = type == 6                      ? 16
                     : type == 7 || read_only_alias ? 128
                     : type == 8                    ? 14
                                                    : options_.uav_register_count;
    if (type == uint32_t(ResourceKind::kUnorderedAccess) && output_.program_type != 5 &&
        !read_only_alias) {
      Require(
          host_register < limit && options_.graphics_uav_register_offset <= limit - host_register,
          "Graphics UAV register offset exceeds the device limit");
      host_register += options_.graphics_uav_register_offset;
    }
    Require(host_register < limit, "Shader resource register exceeds the DX11 device limit");
    if (type == uint32_t(ResourceKind::kUnorderedAccess) && !read_only_alias) {
      bool compute_or_pixel = output_.program_type == 0 || output_.program_type == 5;
      Require(compute_or_pixel ||
                  (options_.supported_feature_flags & dxbc::kShaderFeature0_UAVsAtEveryStage),
              "The device does not support UAVs in this shader stage");
      Require(!(words[0] & dxbc::kUAVFlagRasterizerOrderedAccess) ||
                  (options_.supported_feature_flags & dxbc::kShaderFeature0_ROVs),
              "The device does not support rasterizer-ordered views");
    }
    uint32_t vectors = instruction.opcode == 89 ? words[5] : 0;
    Require(vectors <= 4096, "Constant buffer exceeds the DX11 size limit");
    for (const auto& previous : bindings_) {
      if (uint32_t(previous.resource.kind) == type) {
        Require(previous.resource.id != words[2], "Duplicate shader resource ID");
        Require(previous.resource.host_register != host_register,
                "Overlapping shader resource registers");
      }
    }
    ResourceBinding resource = {ResourceKind(type), words[2], words[3], host_register};
    bool raw = instruction.opcode == 157 || instruction.opcode == 161;
    bool structured = instruction.opcode == 158 || instruction.opcode == 162;
    uint32_t dimension = raw ? 11 : structured ? 12 : (words[0] >> 11) & 31;
    uint32_t return_type = raw || structured ? 0x6666 : words[5];
    bindings_.push_back(
        {resource, vectors, dimension, return_type, structured ? words[5] : 0, read_only_alias});
    if (read_only_alias) {
      output_.aliased_uav_reads.push_back(resource);
    } else {
      output_.bindings.push_back(resource);
    }
  }

  void ConvertOperand(Words input, size_t& position, std::vector<uint32_t>& result,
                      uint32_t depth = 0, const Binding** accessed_resource = nullptr,
                      bool raw_resource_read = false) {
    Require(depth <= 8 && position < input.size(), "Invalid or truncated shader operand");
    uint32_t token = input[position++];
    uint32_t type = (token >> 12) & 255;
    uint32_t dimensions = (token >> 20) & 3;
    size_t token_position = result.size();
    result.push_back(token);
    for (uint32_t extension = token; extension & 0x80000000;) {
      Require(position < input.size(), "Truncated shader operand extension");
      extension = input[position++];
      Require(!(extension & 0x20000), "Non-uniform resource indexing requires shader model 5.1");
      result.push_back(extension);
    }
    std::array<std::vector<uint32_t>, 3> indices;
    std::array<uint32_t, 3> representations = {};
    for (uint32_t i = 0; i < dimensions; ++i) {
      uint32_t representation = (token >> (22 + 3 * i)) & 7;
      Require(representation <= 4, "Invalid shader operand index representation");
      representations[i] = representation;
      if (representation != 2) {
        uint32_t count = representation == 1 || representation == 4 ? 2 : 1;
        Require(count <= input.size() - position, "Truncated immediate shader index");
        indices[i].insert(indices[i].end(), input.begin() + position,
                          input.begin() + position + count);
        position += count;
      }
      if (representation >= 2) {
        ConvertOperand(input, position, indices[i], depth + 1);
      }
    }
    if (type == 6 || type == 7 || type == 8 || type == 30) {
      Require(depth == 0 && dimensions == (type == 8 ? 3u : 2u) && representations[0] == 0 &&
                  representations[1] == 0,
              "Dynamically indexed resource bindings are unsupported in DX11");
      const Binding& binding = FindBinding(type, indices[0][0]);
      if (binding.read_only_alias) {
        Require(raw_resource_read,
                "A read-only UAV alias is used by a shader write or atomic operation");
      }
      if (accessed_resource && (type == 7 || type == 30)) {
        Require(!*accessed_resource, "Multiple resource operands in a shader instruction");
        *accessed_resource = &binding;
      }
      Require(indices[1][0] == binding.resource.source_register,
              "Shader resource access is outside its declared binding");
      result[token_position] = (token & ~kIndexBits) | ((dimensions - 1) << 20) |
                               (type == 8 ? representations[2] << 25 : 0);
      if (binding.read_only_alias) {
        result[token_position] = (result[token_position] & ~(uint32_t(255) << 12)) | (7 << 12);
      }
      result.push_back(binding.resource.host_register);
      if (type == 8) {
        result.insert(result.end(), indices[2].begin(), indices[2].end());
      }
    } else {
      for (uint32_t i = 0; i < dimensions; ++i) {
        result.insert(result.end(), indices[i].begin(), indices[i].end());
      }
    }
    if (type == 4 || type == 5) {
      uint32_t components = token & 3;
      Require(dimensions == 0 && (components == 1 || components == 2), "Invalid immediate operand");
      uint32_t count = (components == 1 ? 1 : 4) * (type == 5 ? 2 : 1);
      Require(count <= input.size() - position, "Truncated immediate shader data");
      result.insert(result.end(), input.begin() + position, input.begin() + position + count);
      position += count;
    }
  }

  std::vector<uint32_t> ApplyTextureSwizzle(uint32_t opcode, const Binding* resource,
                                            size_t destination_begin, size_t destination_end,
                                            size_t resource_begin, std::vector<uint32_t>& result) {
    if (!resource || resource->resource.kind != ResourceKind::kShaderResource)
      return {};
    uint32_t mapping = kIdentityTextureSwizzle;
    for (const auto& swizzle : options_.texture_swizzles) {
      if (swizzle.srv_register == resource->resource.host_register) {
        mapping = swizzle.swizzle;
        break;
      }
    }
    if (mapping == kIdentityTextureSwizzle || opcode == 61 || opcode == 108)
      return {};
    bool color_fetch = opcode == 45 || opcode == 46 || opcode == 69 || opcode == 72 ||
                       opcode == 73 || opcode == 74;
    // Comparison and gather operations do not return one value per channel.
    // The guest translator uses ordinary loads, SampleL and SampleD. Refuse a
    // different operation instead of applying a color mapping to tap results.
    Require(color_fetch, "Texture channel mapping is unsupported for this resource operation");
    uint32_t token = result[resource_begin];
    uint32_t selection = (token >> 2) & 3;
    Require((token & 3) == 2 && (selection == 1 || selection == 2),
            "Invalid texture resource component selection");
    uint32_t destination_token = result[destination_begin];
    Require((destination_token & 15) == 2,
            "Texture channel specialization requires a masked vector destination");
    uint32_t destination_mask = (destination_token >> 4) & 15;
    uint32_t native_swizzle = 0, constant_mask = 0;
    std::array<uint32_t, 4> constants = {};
    for (uint32_t component = 0; component < 4; ++component) {
      uint32_t original = (token >> (selection == 2 ? 4 : 4 + component * 2)) & 3;
      uint32_t native = (mapping >> (original * 3)) & 7;
      if (native >= 4) {
        constant_mask |= destination_mask & (1 << component);
        if (native == 5 && (destination_mask & (1 << component))) {
          uint32_t return_type = (resource->return_type >> (original * 4)) & 15;
          bool integer = return_type == 3 || return_type == 4;
          Require(integer || return_type == 1 || return_type == 2 || return_type == 5,
                  "Texture constant one has an unsupported return type");
          constants[component] = integer ? 1 : 0x3F800000;
        }
        native = 0;
      }
      native_swizzle |= native << (component * 2);
    }
    // Compose the view mapping with the fetch's existing result swizzle. A
    // permutation or replicated color stays within the original instruction.
    result[resource_begin] = (token & ~uint32_t(0xFFC)) | (1 << 2) | (native_swizzle << 4);
    if (!constant_mask)
      return {};
    std::vector<uint32_t> move = {0};
    move.insert(move.end(), result.begin() + destination_begin, result.begin() + destination_end);
    move[1] = (move[1] & ~uint32_t(0xF0)) | (constant_mask << 4);
    move.push_back(2 | (4 << 12));  // Four-component immediate32 source.
    move.insert(move.end(), constants.begin(), constants.end());
    Require(move.size() <= 127, "Texture constant move exceeds the instruction size limit");
    move[0] = uint32_t(Opcode::kMov) | (uint32_t(move.size()) << 24);
    ++output_.texture_swizzle_moves;
    return move;
  }

  void ConvertInstruction(const Instruction& instruction, std::vector<uint32_t>& result) {
    Words input = instruction.words;
    if (instruction.opcode == uint32_t(Opcode::kCustomData)) {
      result.insert(result.end(), input.begin(), input.end());
      return;
    }
    size_t start = result.size();
    std::vector<uint32_t> texture_constant_move;
    result.insert(result.end(), input.begin(), input.begin() + instruction.prefix_length);
    if (uint32_t type = ResourceOperandType(instruction.opcode)) {
      const Binding& binding = FindBinding(type, input[2]);
      if (binding.read_only_alias) {
        result.resize(start);
        return;
      }
      uint32_t token = input[1] & ~kIndexBits;
      if (type == 8) {
        result.push_back(token | (2 << 20));
        result.push_back(binding.resource.host_register);
        result.push_back(binding.cbuffer_vectors);
      } else {
        // FXC emits zero-component declarations for SM 5.0 resources. SM 5.1
        // range declarations instead have a four-component operand.
        result.push_back((token & ~uint32_t(0xFFF)) | (1 << 20));
        result.push_back(binding.resource.host_register);
        if (instruction.opcode == 88 || instruction.opcode == 156 || instruction.opcode == 158 ||
            instruction.opcode == 162) {
          result.push_back(input[5]);
        }
      }
    } else {
      uint32_t trailing = 0;
      int operands = OperandCount(instruction.opcode);
      switch (instruction.opcode) {
        case 91:
        case 96:
        case 97:
        case 99:
        case 100:
        case 102:
        case 103:
          operands = 1;
          trailing = 1;
          break;
        case 95:
        case 98:
        case 101:
        case 143:
          operands = 1;
          break;
        case 159:
          operands = 1;
          trailing = 1;
          break;
        case 160:
          operands = 1;
          trailing = 2;
          break;
        case 92:
        case 93:
        case 106:
        case 147:
        case 148:
        case 149:
        case 150:
        case 151:
        case 113:
        case 114:
        case 115:
        case 116:
        case 190:
          operands = 0;
          if (instruction.opcode == 106) {
            result[start] &= ~uint32_t(dxbc::kGlobalFlagAllResourcesBound);
          }
          break;
        case 94:
        case 104:
        case 152:
        case 153:
        case 154:
          operands = 0;
          trailing = 1;
          break;
        case 105:
        case 155:
          operands = 0;
          trailing = 3;
          break;
      }
      if (operands < 0) {
        throw std::runtime_error("Shader opcode " + std::to_string(instruction.opcode) +
                                 " is unsupported by the DX11 bytecode converter");
      }
      Require(trailing <= input.size() - instruction.prefix_length,
              "Truncated declaration payload");
      Words operand_words = input.first(input.size() - trailing);
      size_t position = instruction.prefix_length;
      const Binding* accessed_resource = nullptr;
      size_t destination_begin = 0, destination_end = 0, resource_begin = 0;
      for (int i = 0; i < operands; ++i) {
        size_t operand_begin = result.size();
        bool had_resource = accessed_resource != nullptr;
        ConvertOperand(operand_words, position, result, 0, &accessed_resource,
                       instruction.opcode == 165);
        if (!i) {
          destination_begin = operand_begin;
          destination_end = result.size();
        }
        if (!had_resource && accessed_resource)
          resource_begin = operand_begin;
      }
      Require(position == operand_words.size(),
              "Shader operand count does not match the instruction");
      result.insert(result.end(), input.end() - trailing, input.end());
      uint32_t opcode = instruction.opcode;
      bool resource_read = opcode == 45 || opcode == 46 || opcode == 61 || opcode == 109 ||
                           (opcode >= 69 && opcode <= 74) || opcode == 108 || opcode == 163 ||
                           opcode == 165;
      if (resource_read && accessed_resource) {
        texture_constant_move = ApplyTextureSwizzle(opcode, accessed_resource, destination_begin,
                                                    destination_end, resource_begin, result);
        // FXC's SM 5.0 loads and samples carry resource dimension and return
        // type extensions. SM 5.1 instructions carry the resource range ID
        // instead. Emit the same extensions as the native compiler.
        bool has_dimension = false, has_return_type = false;
        for (uint32_t i = 1; i < instruction.prefix_length; ++i) {
          has_dimension |= (input[i] & 63) == 2;
          has_return_type |= (input[i] & 63) == 3;
        }
        Require(has_dimension == has_return_type, "Incomplete shader resource opcode extensions");
        if (!has_dimension) {
          Require(accessed_resource->structure_stride <= 4095,
                  "Structured resource stride exceeds shader opcode encoding");
          result[start + instruction.prefix_length - 1] |= 0x80000000;
          std::array<uint32_t, 2> extensions = {0x80000002 | (accessed_resource->dimension << 6) |
                                                    (accessed_resource->structure_stride << 11),
                                                3 | (accessed_resource->return_type << 6)};
          result.insert(result.begin() + start + instruction.prefix_length, extensions.begin(),
                        extensions.end());
        }
      }
    }
    uint32_t length = uint32_t(result.size() - start);
    Require(length <= 127, "Converted shader instruction is too long");
    result[start] = (result[start] & ~kInstructionLengthBits) | (length << 24);
    result.insert(result.end(), texture_constant_move.begin(), texture_constant_move.end());
  }

  Words source_;
  const ShaderBytecodeOptions& options_;
  ShaderBytecode& output_;
  std::vector<Binding> bindings_;
};

std::vector<uint32_t> ConvertReflection(Words source, const ShaderBytecode& program) {
  Require(
      source.size() >= 15 && (source[4] & 0xFFFF) == 0x501 && source[8] == 60 && source[10] == 40,
      "Invalid SM 5.1 reflection header");
  uint32_t count = source[2];
  uint32_t start = source[3];
  Require(!(start & 3) && start / 4 <= source.size() && count <= (source.size() - start / 4) / 10,
          "Invalid shader reflection binding table");
  std::vector<uint32_t> result(source.begin(), source.end());
  result[3] = uint32_t(result.size() * 4);
  result[4] &= ~uint32_t(255);
  result[5] &= ~uint32_t(dxbc::kCompileFlagEnableUnboundedDescriptorTables |
                         dxbc::kCompileFlagAllResourcesBound);
  result[7] = uint32_t(dxbc::RdefHeader::FourCC::k5_0);
  result[10] = 32;
  result[2] = 0;
  for (uint32_t i = 0; i < count; ++i) {
    Words record = source.subspan(start / 4 + i * 10, 10);
    Require(record[8] == 0 && record[6] == 1,
            "DX11 reflection requires single resources in space zero");
    uint32_t type = record[1];
    uint32_t operand_type = type == 0                                          ? 8
                            : type == 3                                        ? 6
                            : type == 1 || type == 2 || type == 5 || type == 7 ? 7
                                                                               : 30;
    bool alias = false;
    for (const auto& resource : program.aliased_uav_reads) {
      if (operand_type == 30 && resource.id == record[9]) {
        Require(resource.source_register == record[5],
                "Reflection UAV alias differs from its declaration");
        alias = true;
        break;
      }
    }
    if (alias) {
      continue;
    }
    const ResourceBinding* binding = nullptr;
    for (const auto& resource : program.bindings) {
      if (uint32_t(resource.kind) == operand_type && resource.id == record[9]) {
        binding = &resource;
        break;
      }
    }
    Require(binding && binding->source_register == record[5],
            "Reflection and shader binding declarations differ");
    size_t position = result.size();
    result.insert(result.end(), record.begin(), record.begin() + 8);
    result[position + 5] = binding->host_register;
    ++result[2];
  }
  return result;
}
}  // namespace

bool ConvertShaderBytecode(std::span<const uint8_t> source, const ShaderBytecodeOptions& options,
                           ShaderBytecode& output, std::string& error) {
  output = {};
  error.clear();
  try {
    Require(source.size() >= 32 && source.size() % 4 == 0 && source.size() <= UINT32_MAX,
            "Invalid DXBC container size");
    // Read through an aligned copy. Callers may supply mapped file data or a
    // byte vector without uint32_t alignment.
    std::vector<uint32_t> words(source.size() / 4);
    std::memcpy(words.data(), source.data(), source.size());
    Require(
        words[0] == dxbc::ContainerHeader::kFourCC && words[5] == 1 && words[6] == source.size(),
        "Invalid DXBC container header");
    uint32_t chunk_count = words[7];
    Require(chunk_count >= 1 && chunk_count <= 32 && chunk_count <= words.size() - 8,
            "Invalid DXBC chunk count");
    std::array<unsigned int, 4> checksum;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(words.data()), uint32_t(source.size()),
                          checksum.data());
    Require(std::memcmp(checksum.data(), words.data() + 1, 16) == 0,
            "DXBC checksum does not match");
    std::vector<Chunk> chunks;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    size_t program_index = SIZE_MAX;
    for (uint32_t i = 0; i < chunk_count; ++i) {
      uint32_t offset = words[8 + i];
      Require(!(offset & 3) && offset / 4 >= 8 + chunk_count && offset / 4 <= words.size() - 2,
              "Invalid DXBC chunk offset");
      uint32_t size = words[offset / 4 + 1];
      Require(!(size & 3) && size / 4 <= words.size() - offset / 4 - 2, "Invalid DXBC chunk size");
      uint32_t end = offset + 8 + size;
      for (const auto& range : ranges) {
        Require(offset >= range.second || end <= range.first, "Overlapping DXBC chunks");
      }
      ranges.emplace_back(offset, end);
      uint32_t fourcc = words[offset / 4];
      chunks.push_back({fourcc, Words(words).subspan(offset / 4 + 2, size / 4)});
      if (fourcc == uint32_t(dxbc::BlobHeader::FourCC::kShaderEx)) {
        Require(program_index == SIZE_MAX, "Duplicate DXBC shader program");
        program_index = i;
      } else if (fourcc == uint32_t(dxbc::BlobHeader::FourCC::kShaderFeatureInfo)) {
        Require(size == 8, "Invalid shader feature information");
      } else if (fourcc != uint32_t(dxbc::BlobHeader::FourCC::kResourceDefinition) &&
                 fourcc != uint32_t(dxbc::BlobHeader::FourCC::kInputSignature) &&
                 fourcc != uint32_t(dxbc::BlobHeader::FourCC::kOutputSignature) &&
                 fourcc != uint32_t(dxbc::BlobHeader::FourCC::kOutputSignatureForGS) &&
                 fourcc != uint32_t(dxbc::BlobHeader::FourCC::kPatchConstantSignature) &&
                 fourcc != uint32_t(dxbc::BlobHeader::FourCC::kStatistics)) {
        char name[5] = {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24),
                        0};
        for (char& c : name) {
          if (c && (c < 0x20 || c > 0x7E)) {
            c = '?';
          }
        }
        throw std::runtime_error(std::string("Unsupported DXBC chunk ") + name +
                                 " in the DX11 shader input");
      }
    }
    Require(program_index != SIZE_MAX, "DXBC container has no shader program");
    ProgramConverter converter(chunks[program_index].words, options, output);
    auto program = converter.Convert();
    std::vector<uint32_t> result(8 + chunk_count);
    result[0] = dxbc::ContainerHeader::kFourCC;
    result[5] = 1;
    result[7] = chunk_count;
    for (uint32_t i = 0; i < chunk_count; ++i) {
      const Chunk& chunk = chunks[i];
      std::vector<uint32_t> reflection;
      Words payload = chunk.words;
      if (i == program_index) {
        payload = program;
      } else if (chunk.fourcc == uint32_t(dxbc::BlobHeader::FourCC::kResourceDefinition)) {
        reflection = ConvertReflection(chunk.words, output);
        payload = reflection;
      } else if (chunk.fourcc == uint32_t(dxbc::BlobHeader::FourCC::kShaderFeatureInfo)) {
        uint64_t features = chunk.words[0] | (uint64_t(chunk.words[1]) << 32);
        if (!output.aliased_uav_reads.empty()) {
          bool has_uav = false;
          for (const auto& resource : output.bindings) {
            has_uav |= resource.kind == ResourceKind::kUnorderedAccess;
          }
          if (!has_uav) {
            features &= ~uint64_t(dxbc::kShaderFeature0_UAVsAtEveryStage);
          }
        }
        Require(!(features & ~options.supported_feature_flags),
                "Shader requires features unsupported by the DX11 device");
        reflection = {uint32_t(features), uint32_t(features >> 32)};
        payload = reflection;
      } else if (chunk.fourcc == uint32_t(dxbc::BlobHeader::FourCC::kStatistics) &&
                 output.texture_swizzle_moves) {
        Require(payload.size() >= 20, "Truncated shader statistics");
        reflection.assign(payload.begin(), payload.end());
        Require(reflection[0] <= UINT32_MAX - output.texture_swizzle_moves &&
                    reflection[19] <= UINT32_MAX - output.texture_swizzle_moves,
                "Shader statistics overflow after texture channel specialization");
        reflection[0] += output.texture_swizzle_moves;
        reflection[19] += output.texture_swizzle_moves;
        payload = reflection;
      }
      result[8 + i] = uint32_t(result.size() * 4);
      result.push_back(chunk.fourcc);
      result.push_back(uint32_t(payload.size() * 4));
      result.insert(result.end(), payload.begin(), payload.end());
    }
    result[6] = uint32_t(result.size() * 4);
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(result.data()), result[6],
                          checksum.data());
    std::memcpy(result.data() + 1, checksum.data(), 16);
    output.data.resize(result.size() * 4);
    std::memcpy(output.data.data(), result.data(), output.data.size());
    return true;
  } catch (const std::exception& exception) {
    output = {};
    error = exception.what();
    return false;
  }
}

}  // namespace rex::graphics::d3d11
