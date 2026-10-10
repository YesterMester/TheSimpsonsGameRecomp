#include <rex/graphics/pipeline/shader/smooth_tessellation.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics {

namespace {

std::string_view Trim(std::string_view text) {
  size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

bool ParseNumber(std::string_view text, uint32_t& value_out) {
  if (text.empty()) {
    return false;
  }
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value_out);
  return error == std::errc{} && end == text.data() + text.size();
}

// An operand like c12.zxyw, r2 or r0.zyxx. Negated, absolute and relatively
// addressed operands are not parsed.
struct Operand {
  bool is_constant = false;
  uint32_t index = 0;
  std::string swizzle;
};

bool ParseOperand(std::string_view text, Operand& operand_out) {
  text = Trim(text);
  if (text.size() < 2 || (text[0] != 'c' && text[0] != 'r')) {
    return false;
  }
  operand_out.is_constant = text[0] == 'c';
  size_t dot = text.find('.');
  if (!ParseNumber(text.substr(1, dot == std::string_view::npos ? std::string_view::npos : dot - 1),
                   operand_out.index)) {
    return false;
  }
  if (dot == std::string_view::npos) {
    operand_out.swizzle = "xyzw";
  } else {
    operand_out.swizzle = std::string(text.substr(dot + 1));
    if (operand_out.swizzle.size() != 4) {
      return false;
    }
  }
  return true;
}

// A destination like oPos.x___, o2._y__ or r7.xyz_: the register name and the
// written components.
struct Destination {
  std::string name;
  uint32_t mask = 0;
};

bool ParseDestination(std::string_view text, Destination& destination_out) {
  text = Trim(text);
  size_t dot = text.find('.');
  destination_out.name = std::string(text.substr(0, dot));
  if (destination_out.name.empty()) {
    return false;
  }
  if (dot == std::string_view::npos) {
    destination_out.mask = 0b1111;
    return true;
  }
  std::string_view mask = text.substr(dot + 1);
  destination_out.mask = 0;
  for (size_t i = 0; i < mask.size() && i < 4; ++i) {
    if (mask[i] != '_') {
      destination_out.mask |= uint32_t(1) << i;
    }
  }
  return true;
}

struct Instruction {
  size_t line;
  bool conditional;
  std::string opcode;
  Destination destination;
  std::vector<std::string_view> operands;
};

// One component of a dp3 / dp4 with a constant and a temporary register.
struct DotProduct {
  size_t line;
  uint32_t constant;
  std::string constant_swizzle;
  uint32_t reg;
  std::string reg_swizzle;
};

bool ParseDotProduct(const Instruction& instruction, DotProduct& dot_out) {
  if (instruction.conditional || instruction.operands.size() != 2) {
    return false;
  }
  Operand a, b;
  if (!ParseOperand(instruction.operands[0], a) || !ParseOperand(instruction.operands[1], b) ||
      a.is_constant == b.is_constant) {
    return false;
  }
  const Operand& constant = a.is_constant ? a : b;
  const Operand& reg = a.is_constant ? b : a;
  dot_out.line = instruction.line;
  dot_out.constant = constant.index;
  dot_out.constant_swizzle = constant.swizzle;
  dot_out.reg = reg.index;
  dot_out.reg_swizzle = reg.swizzle;
  return true;
}

uint32_t SingleComponent(uint32_t mask) {
  switch (mask) {
    case 0b0001:
      return 0;
    case 0b0010:
      return 1;
    case 0b0100:
      return 2;
    case 0b1000:
      return 3;
    default:
      return UINT32_MAX;
  }
}

// The same dot product for components 0...count-1 of one output: consecutive
// constants with one swizzle, and one register with one swizzle.
bool IsConsecutiveDotProduct(const std::array<std::vector<DotProduct>, 4>& components,
                             uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (components[i].size() != 1) {
      return false;
    }
    const DotProduct& first = components[0][0];
    const DotProduct& dot = components[i][0];
    if (dot.constant != first.constant + i || dot.constant_swizzle != first.constant_swizzle ||
        dot.reg != first.reg || dot.reg_swizzle != first.reg_swizzle) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool FindSmoothTessellationLayout(std::string_view ucode_disassembly,
                                  SmoothTessellationLayout& layout_out) {
  bool skinned = ucode_disassembly.find("c[") != std::string_view::npos;
  std::vector<Instruction> instructions;
  // Jumps as (jump line, target label), and label lines.
  std::vector<std::pair<size_t, std::string>> jumps;
  std::map<std::string, size_t> labels;
  bool exec_conditional = false;
  size_t line_index = 0;
  while (!ucode_disassembly.empty()) {
    size_t line_end = ucode_disassembly.find('\n');
    std::string_view line = Trim(ucode_disassembly.substr(0, line_end));
    ucode_disassembly.remove_prefix(line_end == std::string_view::npos ? ucode_disassembly.size()
                                                                       : line_end + 1);
    ++line_index;
    bool control_flow = false;
    if (line.starts_with("/*")) {
      size_t comment_end = line.find("*/");
      if (comment_end == std::string_view::npos) {
        continue;
      }
      // Control flow instructions are numbered like 1.1, ALU and fetch ones
      // by their slot.
      control_flow = line.substr(2, comment_end - 2).find('.') != std::string_view::npos;
      line = Trim(line.substr(comment_end + 2));
    }
    if (line.starts_with("label ")) {
      labels.emplace(std::string(Trim(line.substr(6))), line_index);
      continue;
    }
    if (line.starts_with("+")) {
      line = Trim(line.substr(1));
    }
    bool predicated = false;
    if (line.starts_with("(p") || line.starts_with("(!p")) {
      size_t predicate_end = line.find(')');
      if (predicate_end == std::string_view::npos) {
        return false;
      }
      predicated = true;
      line = Trim(line.substr(predicate_end + 1));
    }
    size_t comment = line.find("//");
    if (comment != std::string_view::npos) {
      line = Trim(line.substr(0, comment));
    }
    if (line.empty()) {
      continue;
    }
    size_t opcode_end = line.find(' ');
    std::string_view opcode = line.substr(0, opcode_end);
    std::string_view arguments =
        opcode_end == std::string_view::npos ? std::string_view() : Trim(line.substr(opcode_end));
    if (control_flow) {
      if (opcode.find("exec") != std::string_view::npos) {
        exec_conditional = predicated || opcode.starts_with("c");
      } else if (opcode.find("jmp") != std::string_view::npos) {
        size_t label = arguments.rfind('L');
        if (label == std::string_view::npos) {
          return false;
        }
        jumps.emplace_back(line_index, std::string(Trim(arguments.substr(label))));
      } else if (opcode.find("loop") != std::string_view::npos ||
                 opcode.find("call") != std::string_view::npos ||
                 opcode.find("ret") != std::string_view::npos) {
        // Not seen in the shaders this is for; not worth reasoning about.
        return false;
      }
      continue;
    }
    Instruction instruction;
    instruction.line = line_index;
    instruction.conditional = predicated || exec_conditional;
    instruction.opcode = std::string(opcode);
    if (arguments.empty()) {
      continue;
    }
    size_t operand_start = arguments.find(',');
    if (!ParseDestination(arguments.substr(0, operand_start), instruction.destination)) {
      continue;
    }
    while (operand_start != std::string_view::npos) {
      size_t next = arguments.find(',', operand_start + 1);
      instruction.operands.push_back(Trim(arguments.substr(
          operand_start + 1,
          next == std::string_view::npos ? std::string_view::npos : next - operand_start - 1)));
      operand_start = next;
    }
    instructions.push_back(std::move(instruction));
  }

  // Gather the single-component dot products into outputs, and count every
  // write to every output component.
  std::array<std::vector<DotProduct>, 4> position;
  std::map<uint32_t, std::array<std::vector<DotProduct>, 4>> dp4_interpolators;
  std::map<uint32_t, std::array<std::vector<DotProduct>, 4>> dp3_interpolators;
  std::map<std::string, std::array<uint32_t, 4>> output_writes;
  for (const Instruction& instruction : instructions) {
    const Destination& destination = instruction.destination;
    if (destination.name.empty() || destination.name[0] != 'o') {
      continue;
    }
    std::array<uint32_t, 4>& writes = output_writes[destination.name];
    for (uint32_t i = 0; i < 4; ++i) {
      if (destination.mask & (uint32_t(1) << i)) {
        ++writes[i];
      }
    }
    uint32_t component = SingleComponent(destination.mask);
    bool dp4 = instruction.opcode == "dp4";
    bool dp3 = instruction.opcode == "dp3";
    DotProduct dot;
    if (component == UINT32_MAX || (!dp4 && !dp3) || !ParseDotProduct(instruction, dot)) {
      continue;
    }
    if (destination.name == "oPos") {
      if (dp4) {
        position[component].push_back(dot);
      }
      continue;
    }
    uint32_t interpolator;
    if (!ParseNumber(std::string_view(destination.name).substr(1), interpolator)) {
      continue;
    }
    (dp4 ? dp4_interpolators : dp3_interpolators)[interpolator][component].push_back(dot);
  }

  if (!IsConsecutiveDotProduct(position, 4)) {
    return false;
  }
  const DotProduct& clip = position[0][0];
  // The constant swizzle must keep W in place and permute XYZ, so the same
  // model-space vector meets the constants' XYZ and W as stored.
  std::string swizzle_xyz = clip.constant_swizzle.substr(0, 3);
  std::sort(swizzle_xyz.begin(), swizzle_xyz.end());
  if (clip.constant_swizzle[3] != 'w' || swizzle_xyz != "xyz") {
    return false;
  }
  auto written_once = [&](const std::string& name, uint32_t components) {
    auto it = output_writes.find(name);
    if (it == output_writes.end()) {
      return false;
    }
    for (uint32_t i = 0; i < components; ++i) {
      if (it->second[i] != 1) {
        return false;
      }
    }
    return true;
  };
  if (!written_once("oPos", 4)) {
    return false;
  }

  // Lines in [first, last] must not change the register, and no jump may skip
  // only some of them.
  auto range_is_straight = [&](uint32_t reg, size_t first, size_t last) {
    std::string name = "r" + std::to_string(reg);
    for (const Instruction& instruction : instructions) {
      if (instruction.line > first && instruction.line < last &&
          instruction.destination.name == name) {
        return false;
      }
    }
    return true;
  };
  auto no_jump_into = [&](size_t line) {
    for (const auto& [jump_line, label] : jumps) {
      auto label_it = labels.find(label);
      if (label_it == labels.end()) {
        return false;
      }
      size_t low = std::min(jump_line, label_it->second);
      size_t high = std::max(jump_line, label_it->second);
      if (line > low && line < high) {
        return false;
      }
    }
    return true;
  };

  for (const auto& [position_interpolator, world] : dp4_interpolators) {
    if (!IsConsecutiveDotProduct(world, 3)) {
      continue;
    }
    const DotProduct& world_first = world[0][0];
    if (world_first.constant_swizzle != clip.constant_swizzle || world_first.reg != clip.reg ||
        world_first.reg_swizzle != clip.reg_swizzle) {
      continue;
    }
    if (!written_once("o" + std::to_string(position_interpolator), 3)) {
      continue;
    }
    for (const auto& [normal_interpolator, normal] : dp3_interpolators) {
      if (normal_interpolator == position_interpolator || !IsConsecutiveDotProduct(normal, 3)) {
        continue;
      }
      const DotProduct& normal_first = normal[0][0];
      if (normal_first.constant != world_first.constant ||
          normal_first.constant_swizzle.compare(0, 3, clip.constant_swizzle, 0, 3) != 0) {
        continue;
      }
      if (!written_once("o" + std::to_string(normal_interpolator), 3)) {
        continue;
      }
      std::vector<size_t> lines;
      for (uint32_t i = 0; i < 4; ++i) {
        lines.push_back(position[i][0].line);
      }
      for (uint32_t i = 0; i < 3; ++i) {
        lines.push_back(world[i][0].line);
      }
      auto [position_first, position_last] = std::minmax_element(lines.begin(), lines.end());
      size_t normal_first_line =
          std::min({normal[0][0].line, normal[1][0].line, normal[2][0].line});
      size_t normal_last_line = std::max({normal[0][0].line, normal[1][0].line, normal[2][0].line});
      if (!range_is_straight(clip.reg, *position_first, *position_last) ||
          !range_is_straight(normal_first.reg, normal_first_line, normal_last_line)) {
        continue;
      }
      for (uint32_t i = 0; i < 3; ++i) {
        lines.push_back(normal[i][0].line);
      }
      if (!std::all_of(lines.begin(), lines.end(), no_jump_into)) {
        continue;
      }
      layout_out.clip_constant = clip.constant;
      layout_out.world_constant = world_first.constant;
      layout_out.position_interpolator = position_interpolator;
      layout_out.normal_interpolator = normal_interpolator;
      layout_out.skinned = skinned;
      return true;
    }
  }
  return false;
}

}  // namespace rex::graphics
