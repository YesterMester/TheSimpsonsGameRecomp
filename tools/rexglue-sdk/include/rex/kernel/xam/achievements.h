/**
 * @file        rex/kernel/xam/achievements.h
 * @brief       Lets the host keep a title's achievements.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace rex::kernel::xam {

// Without a host, achievement writes are accepted and forgotten, enumeration
// reports every achievement as locked, and the Guide's achievements screen is
// not shown, as before.
struct AchievementHost {
  // The title awarded an achievement to a signed-in user (XUserWriteAchievements).
  // Called on the title's thread, once for each achievement written.
  std::function<void(uint32_t user_index, uint32_t achievement_id)> award;
  // Whether the user has the achievement, and when it was unlocked as a FILETIME
  // (0 when unknown). Used when the title enumerates its achievements.
  std::function<bool(uint32_t user_index, uint32_t achievement_id, uint64_t* filetime)> unlocked;
  // The title opened the Guide's achievements screen (XShowAchievementsUI).
  std::function<void(uint32_t user_index)> show;
};

// Set once before the title starts.
void SetAchievementHost(AchievementHost host);
const AchievementHost& GetAchievementHost();

}  // namespace rex::kernel::xam
