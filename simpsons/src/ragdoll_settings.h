#pragma once

#include "players.h"
#include "runtime.h"

#include <cmath>

namespace players {

inline simpsons::modkit::Json SaveRagdollSettings(const RagdollSettings& settings) {
  return {{"enabled", settings.enabled}, {"run_impact", settings.run_impact},
          {"punch_impact", settings.punch_impact}, {"push_downed", settings.push_downed},
          {"player_enabled", settings.player_enabled}, {"force_scale", settings.force_scale}};
}

inline RagdollSettings ParseRagdollSettings(const simpsons::modkit::Json& value) {
  using simpsons::modkit::Failure;
  if (!value.is_object() || value.size() != 6)
    throw Failure("invalid_argument", "Knockdown settings need all six named options");
  for (const auto* key : {"enabled", "run_impact", "punch_impact", "push_downed", "player_enabled"})
    if (!value.contains(key) || !value.at(key).is_boolean())
      throw Failure("invalid_argument", "Knockdown switches must be boolean");
  if (!value.contains("force_scale") || !value.at("force_scale").is_number())
    throw Failure("invalid_argument", "Knockdown force must be a number");
  const auto scale = value.at("force_scale").get<double>();
  if (!std::isfinite(scale) || scale < 0.25 || scale > 4.0)
    throw Failure("invalid_argument", "Knockdown force must be between 0.25 and 4");
  return {value.at("enabled").get<bool>(), value.at("run_impact").get<bool>(),
          value.at("punch_impact").get<bool>(), value.at("push_downed").get<bool>(),
          value.at("player_enabled").get<bool>(), float(scale)};
}

}  // namespace players
