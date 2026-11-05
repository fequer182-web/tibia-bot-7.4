#pragma once
#include "api/CromeApi.hpp"
#include "app/ActionScheduler.hpp"
#include "app/CooldownManager.hpp"
#include "app/GameStateService.hpp"
#include "modules/healing/HealingEngine.hpp"
#include "modules/support/SupportEngine.hpp"
#include "modules/runemaker/RuneMakerEngine.hpp"
#include "modules/targeting/TargetingEngine.hpp"
#include "modules/cavebot/CavebotEngine.hpp"
#include "modules/looter/LooterEngine.hpp"
#include "modules/alarms/AlarmEngine.hpp"
#include "modules/hotkeys/HotkeyEngine.hpp"
namespace crome { class BotController { CromeApi& api_; GameStateService state_; ActionScheduler scheduler_; CooldownManager cooldown_; public: HealingEngine healing; SupportEngine support; RuneMakerEngine runeMaker; TargetingEngine targeting; CavebotEngine cavebot; LooterEngine looter; AlarmEngine alarms; HotkeyEngine hotkeys; explicit BotController(CromeApi&); void tick(); }; }
