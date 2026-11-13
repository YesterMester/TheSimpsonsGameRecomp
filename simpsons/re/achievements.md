# Achievements

The game has 44 Xbox 360 achievements worth 1000 gamerscore. Their names, descriptions,
gamerscore and pictures are in the title data (XDBF) of `default.xex`, read at run time through
`KernelState::title_xdbf()`; no game content is copied into the project.

## How the game awards them

Almost every achievement is an unlock watcher in `gamedata/simpsons_scores.lua`:

    score:AddUnlockWatcher(ACHIEVEMENT_AWARD, threshold, ACHIEVEMENT_X)

When the score reaches the threshold, the game writes the achievement with
`XUserWriteAchievements` (`sub_824316A8`, XGI message `0xB0008` with one
`XUSER_ACHIEVEMENT {user index, achievement id}`). Press START to Play (32) is the only one that is
not a score watcher. The `ACHIEVEMENT_*` Lua constants are registered by `sub_82A11CC8` with the
XDBF ids as their values.

| Id | Achievement | Score watched | Threshold |
|---|---|---|---|
| 33–48 | Finish each episode | `SCORE_<episode>_COMPLETE` | 1 |
| 61–77 | Win each time challenge | `SCORE_<episode>_CHALLENGE_COMPLETE` | 1 |
| 49 | Goes to 11 | `SCORE_SPR_HUB_TROPHIES_AWARDED` | 4 |
| 50 | Dufftacular Finish | `SCORE_TOTAL_BEERCAP_COLLECTED` | 154 |
| 51 | Hairs to You | `SCORE_TOTAL_PRODUCT_COLLECTED` | 110 |
| 52 | Doll Crazy | `SCORE_TOTAL_DOLL_COLLECTED` | 124 |
| 54 | Poster Paster | `SCORE_TOTAL_KOUPON_COLLECTED` | 151 |
| 55 | Worst Cliché Ever | `SCORE_CLICHE_31` | 1 |
| 56 | Complete Package | `SCORE_TASKS_COMPLETED` | 3 |
| 57 | Fast Times | `SCORE_TOTAL_EPISODE_TARGET_TIMES_BEATEN` | 16 |
| 58 | Heavenly Score | `SCORE_MEET_THY_PLAYER_GOD_BATTLE_PERFECT` | 1 |
| 59 | Pwnd | `SCORE_TOTAL_PLAYER_DEATHS` | 10 |
| 60 | Challenger | `SCORE_ALL_CHALLENGES_COMPLETE` | 1 |

The episode pairs follow `EpisodeHelper` in the script, for example Bartman Begins: Heist Hijinx
(34) and Up and Atom (62); The Day of the Dolphin: A Passive Fish (40) and Nice Cans! (69).
Heavenly Score's score has no session storage, so it only exists while the final battle runs.

## Reading the scores

The ScoreKeeper pointer is at `0x82D08BB8`; its sorted score array is at +16 and the count at +20.
A score's name is hashed by `sub_827451C0` (ASCII lowercased, `h = h * 65599 + c`). The natives
under the Lua bindings take the hash:

| Binding | Native | Arguments |
|---|---|---|
| `sk:GetScore(name)` | `sub_828A3100` | keeper, hash, context → f1 (0 for a missing score) |
| `sk:SetScore(name, value)` | `sub_828A31A8` | keeper, hash, f1 = value, r6 = context |
| `sk:IssueScoreEvent(name)` | `sub_828A3420` | keeper, hash, context |

Setting a score runs its watchers, as the game's scripts do. The global context is 0.

## The recomp's achievement system

`simpsons/src/achievements.cpp` keeps the unlocks next to the profile's save slots, in
`<user data>/<XUID>/45410809/achievements.txt` (achievement id and Unix unlock time per line), so
the launcher's save backups include them. The runtime's `AchievementHost`
(`rex/kernel/xam/achievements.h`) receives `XUserWriteAchievements`, reports the unlocks with
their times to `XamUserCreateAchievementEnumerator`, and opens the achievements screen for
`XamShowAchievementsUI`, which the game calls when it gets `iMsgShowAchievements`.

Watchers fire when a score changes, never when a save is loaded. Twice a second the loaded
save's scores are read with the game's own GetScore and compared with the thresholds above, so
progress made before achievements were kept unlocks them too, and the counted achievements show
their progress. Front-end defaults are all zero and unlock nothing.

On Linux, pressing START at the title makes the game write Press START to Play through
XUserWriteAchievements, and loading an older Springfield save unlocked Chocolate Victory from its
`SCORE_LAND_OF_CHOCOLATE_COMPLETE`; both showed their notification and appear on the
achievements screen, which the pause menu opens with Y.
