#include "modules/healing/HealingEngine.hpp"
#include <chrono>

namespace crome {

void HealingEngine::tick(const GameState& s, CromeApi& api, ActionScheduler& q, CooldownManager& cd) {
    if (!cfg_.enabled || !s.player.valid) return;

    const double hp = s.player.healthPercent();
    const double mp = s.player.manaPercent();
    const double mana = s.player.mana;
    const auto selfPos = s.player.position;

    // Rifbot reference behavior: UH has highest healing priority and is used on self position.
    if (cfg_.uhEnabled && cfg_.uhItemId != 0 && hp <= cfg_.uhHpPercent) {
        if (!cd.ready("heal_item", std::chrono::milliseconds(650))) return;
        const auto id = cfg_.uhItemId;
        q.submit({110, "healing.uh", [&api, &cd, id, selfPos] {
            api.actions().useWithGround(id, selfPos);
            cd.mark("heal_item");
        }});
        return;
    }

    // Mana Fluid: use on self when mana percent is at/below configured threshold.
    if (cfg_.manaFluidEnabled && cfg_.manaFluidItemId != 0 && mp <= cfg_.manaFluidPercent) {
        if (!cd.ready("heal_fluid", std::chrono::milliseconds(700))) return;
        const auto id = cfg_.manaFluidItemId;
        q.submit({105, "healing.mana_fluid", [&api, &cd, id, selfPos] {
            api.actions().useWithGround(id, selfPos);
            cd.mark("heal_fluid");
        }});
        return;
    }

    // Life Fluid: same self-use model as UH/Mana Fluid, but driven by HP percent.
    if (cfg_.lifeFluidEnabled && cfg_.lifeFluidItemId != 0 && hp <= cfg_.lifeFluidHpPercent) {
        if (!cd.ready("heal_fluid", std::chrono::milliseconds(700))) return;
        const auto id = cfg_.lifeFluidItemId;
        q.submit({100, "healing.life_fluid", [&api, &cd, id, selfPos] {
            api.actions().useWithGround(id, selfPos);
            cd.mark("heal_fluid");
        }});
        return;
    }

    // Spell healing. Rifbot semantics: HP is percentage, Mana is an absolute value.
    for (const auto& r : cfg_.rules) {
        if (!r.enabled || r.spell.empty()) continue;
        if (hp > r.hpPercent) continue;
        if (mana < r.minMana) continue;
        if (!cd.ready("heal_spell", std::chrono::milliseconds(700))) return;

        const auto spell = r.spell;
        q.submit({r.priority, "healing.spell", [&api, &cd, spell] {
            api.actions().say(spell);
            cd.mark("heal_spell");
        }});
        return;
    }
}

} // namespace crome
