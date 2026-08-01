/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "PetAI.h"
#include "AIException.h"
#include "CharmInfo.h"
#include "CombatManager.h"
#include "Creature.h"
#include "Errors.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"


namespace
{
    // ===== DK 永久食尸鬼修复相关常量 =====
    uint32 constexpr NPC_DK_RISEN_GHOUL = 26125;            // DK 永久食尸鬼 creature entry
    uint32 constexpr NPC_DK_HULKING_HORROR = 141244;        // 黑暗突变后使用的模型来源 creature_template

    // 黑暗突变 aura（旧版/12.x/效果 aura 三种 ID 全部纳入检测）
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION_LEGACY = 63560;
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION = 1233448;
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION_EFFECT = 1235391;

    // 食尸鬼宠物栏基础技能 ID（display 与 execution 两套）
    uint32 constexpr SPELL_DK_GHOUL_CLAW_DISPLAY = 47468;
    uint32 constexpr SPELL_DK_GHOUL_GNAW_DISPLAY = 47481;
    uint32 constexpr SPELL_DK_GHOUL_LEAP_DISPLAY = 47482;

    uint32 constexpr SPELL_DK_GHOUL_CLAW_EXECUTION = 91776;
    uint32 constexpr SPELL_DK_GHOUL_GNAW_EXECUTION = 91800;
    uint32 constexpr SPELL_DK_GHOUL_LEAP_EXECUTION = 91809;

    // 黑暗突变后替换的强化技能 ID
    uint32 constexpr SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY = 91778;
    uint32 constexpr SPELL_DK_GHOUL_SWEEPING_CLAWS = 1278150;
    uint32 constexpr SPELL_DK_GHOUL_MONSTROUS_BLOW = 91797;
    uint32 constexpr SPELL_DK_GHOUL_SHAMBLING_RUSH = 91802;

    enum class DkGhoulOverrideCastResult : uint8
    {
        NotApplicable,
        Casted,
        Blocked
    };

    // 判断是否为 DK 永久食尸鬼（限定 Creature Entry 26125 的宠物）
    bool IsDkRisenGhoul(Creature const* creature)
    {
        return creature && creature->IsPet() && creature->GetEntry() == NPC_DK_RISEN_GHOUL;
    }

    // 检测单位（食尸鬼自身或其主人）是否处于黑暗突变 aura 下
    bool HasDkDarkTransformationAura(Unit const* unit)
    {
        return unit &&
            (unit->HasAura(SPELL_DK_DARK_TRANSFORMATION_LEGACY) ||
             unit->HasAura(SPELL_DK_DARK_TRANSFORMATION) ||
             unit->HasAura(SPELL_DK_DARK_TRANSFORMATION_EFFECT));
    }

    // 取黑暗突变期间应当替换使用的强化技能 ID；不替换则返回 0
    uint32 GetDkGhoulDarkTransformationOverrideSpellId(Unit const* caster, uint32 baseSpellId)
    {
        if (!caster ||
            (!HasDkDarkTransformationAura(caster) &&
             !HasDkDarkTransformationAura(caster->GetCharmerOrOwner())))
            return 0;

        Difficulty const difficulty = caster->GetMap()->GetDifficultyID();

        switch (baseSpellId)
        {
            case SPELL_DK_GHOUL_CLAW_DISPLAY:
            case SPELL_DK_GHOUL_CLAW_EXECUTION:
                // 优先使用 12.x 当前spell，保留历史ID作为数据兼容回退
                if (sSpellMgr->GetSpellInfo(SPELL_DK_GHOUL_SWEEPING_CLAWS, difficulty))
                    return SPELL_DK_GHOUL_SWEEPING_CLAWS;
                if (sSpellMgr->GetSpellInfo(SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY, difficulty))
                    return SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY;
                break;
            case SPELL_DK_GHOUL_GNAW_DISPLAY:
            case SPELL_DK_GHOUL_GNAW_EXECUTION:
                if (sSpellMgr->GetSpellInfo(SPELL_DK_GHOUL_MONSTROUS_BLOW, difficulty))
                    return SPELL_DK_GHOUL_MONSTROUS_BLOW;
                break;
            case SPELL_DK_GHOUL_LEAP_DISPLAY:
            case SPELL_DK_GHOUL_LEAP_EXECUTION:
                if (sSpellMgr->GetSpellInfo(SPELL_DK_GHOUL_SHAMBLING_RUSH, difficulty))
                    return SPELL_DK_GHOUL_SHAMBLING_RUSH;
                break;
            default:
                break;
        }

        return 0;
    }

    bool IsDkGhoulClawSpell(uint32 spellId)
    {
        return spellId == SPELL_DK_GHOUL_CLAW_DISPLAY ||
            spellId == SPELL_DK_GHOUL_CLAW_EXECUTION;
    }

    // 取食尸鬼基础技能的能量消耗。横扫爪击固定 40 能量（数据库中可能为 0，不可信）
    int32 GetDkGhoulBaseSpellEnergyCost(Spell const* baseSpell)
    {
        if (!baseSpell)
            return 0;

        if (IsDkGhoulClawSpell(baseSpell->GetSpellInfo()->Id))
            return 40;

        return std::max<int32>(
            0, baseSpell->GetPowerTypeCostAmount(POWER_ENERGY).value_or(0));
    }

    bool HasDkGhoulBaseSpellPower(Unit const* caster, Spell const* baseSpell)
    {
        int32 const energyCost = GetDkGhoulBaseSpellEnergyCost(baseSpell);
        return !energyCost ||
            int32(caster->GetPower(POWER_ENERGY)) >= energyCost;
    }

    void ConsumeDkGhoulBaseSpellPower(Unit* caster, Spell const* baseSpell)
    {
        int32 const energyCost = GetDkGhoulBaseSpellEnergyCost(baseSpell);
        if (energyCost)
            caster->ModifyPower(POWER_ENERGY, -energyCost);
    }

    // 启动强化技能的冷却（同时同步基础技能的 GCD）
    void StartDkGhoulOverrideCooldown(Unit* caster, SpellInfo const* baseSpellInfo,
        SpellInfo const* overrideSpellInfo)
    {
        if (!caster || !baseSpellInfo || !overrideSpellInfo)
            return;

        SpellHistory* spellHistory = caster->GetSpellHistory();

        if (overrideSpellInfo->GetRecoveryTime() ||
            overrideSpellInfo->CategoryRecoveryTime)
        {
            spellHistory->SendCooldownEvent(overrideSpellInfo, 0, nullptr, true);
        }
        else
        {
            // 横扫爪击在部分数据集中没有正常冷却，加 1 秒服务器冷却避免每 tick 触发
            spellHistory->StartCooldown(
                overrideSpellInfo, 0, nullptr, false, Seconds(1));
            spellHistory->SendCooldownEvent(
                overrideSpellInfo, 0, nullptr, false);
        }

        if (baseSpellInfo->StartRecoveryTime)
            spellHistory->AddGlobalCooldown(
                baseSpellInfo, Milliseconds(baseSpellInfo->StartRecoveryTime));
    }

    // 目标的 ThreatManager 中是否含有主人真实威胁记录
    bool TargetHasRealThreatFromOwner(Unit const* target, Unit const* owner)
    {
        return target && owner && target->GetThreatManager().IsThreatenedBy(owner, true);
    }

    // 玩家主人是否真的对 target 开战（玩家在战斗 + 玩家victim==target + 玩家在target的threat表里）
    bool OwnerReallyAttackedTarget(Creature const* pet, Unit const* target)
    {
        Unit const* owner = pet ? pet->GetCharmerOrOwner() : nullptr;
        return owner && target &&
            owner->GetVictim() == target &&
            owner->IsInCombat() &&
            TargetHasRealThreatFromOwner(target, owner);
    }

    // 判断是否可以安全地结束主人战斗代理。
    // 目标已死亡/已从世界移除时返回 true（属于正常的战后清理路径）。
    // 目标仍与食尸鬼或主人实际战斗时返回 false。
    bool DkGhoulCanSafelyEndOwnerCombatProxy(Creature* ghoul, Unit* owner, ObjectGuid const& targetGuid)
    {
        if (!ghoul || !owner)
            return false;

        Unit* target = ObjectAccessor::GetUnit(*ghoul, targetGuid);

        // 目标已消失、已死亡或不可访问时可以移除代理。这是正常的
        // evade/despawn/死亡后清理路径。
        if (!target || !target->IsAlive())
            return true;

        // 怪物仍在与食尸鬼或主人实际战斗时不要清理战斗。这保持 retail 行为：
        // 食尸鬼仍在坦怪时，主人保持战斗状态。
        if (ghoul->IsInCombatWith(target) || target->IsInCombatWith(ghoul))
            return false;

        if (ghoul->GetVictim() == target ||
            target->GetVictim() == ghoul ||
            target->GetVictim() == owner)
            return false;

        // 不要因为主人仍有陈旧威胁或陈旧 victim 指针就阻止清理。
        // 在 Follow/召回后，Trinity 可能保留这些记录，而怪物已经在离开战斗，
        // 这会导致玩家永远卡在战斗中。
        return true;
    }

    // 尝试结束主人的战斗代理（CombatReference）。
    // 重要：当目标已死亡、正在 despawn 或已从世界移除时，PetAI 不要调用
    // CombatReference::EndCombat()。正常的 Unit::Kill/despawn/CombatManager
    // 流程会负责这些清理。在击杀边缘调用 EndCombat() 可能使核心正在处理的
    // 战斗引用失效，这与食尸鬼把怪物打到最后一丝血时发生的崩溃相吻合。
    bool TryEndDkGhoulOwnerCombatProxy(Creature* ghoul, Unit* owner, ObjectGuid const& targetGuid)
    {
        if (!ghoul || !owner)
            return false;

        Unit* target = ObjectAccessor::GetUnit(*ghoul, targetGuid);

        // 非常重要：当目标已死亡、正在 despawn 或已从世界移除时，不要从 PetAI
        // 调用 CombatReference::EndCombat()。正常的 Unit::Kill/despawn/
        // CombatManager 流程会负责那些清理。在击杀边缘调用 EndCombat() 可能使
        // 核心正在处理的战斗引用失效。
        if (!target || !target->IsAlive())
            return true;

        if (!DkGhoulCanSafelyEndOwnerCombatProxy(ghoul, owner, targetGuid))
            return false;

        auto const& pveRefs = owner->GetCombatManager().GetPvECombatRefs();
        if (auto itr = pveRefs.find(targetGuid); itr != pveRefs.end())
            itr->second->EndCombat();

        auto const& pvpRefs = owner->GetCombatManager().GetPvPCombatRefs();
        if (auto itr = pvpRefs.find(targetGuid); itr != pvpRefs.end())
            itr->second->EndCombat();

        // 如果主人战斗引用已不存在，代理跟踪也已完成，可以丢弃。
        return true;
    }

}

int32 PetAI::Permissible(Creature const* creature)
{
    if (creature->HasUnitTypeMask(UNIT_MASK_CONTROLABLE_GUARDIAN))
    {
        if (reinterpret_cast<Guardian const*>(creature)->GetOwner()->GetTypeId() == TYPEID_PLAYER)
            return PERMIT_BASE_PROACTIVE;
        return PERMIT_BASE_REACTIVE;
    }

    return PERMIT_BASE_NO;
}

PetAI::PetAI(Creature* creature, uint32 scriptId) : CreatureAI(creature, scriptId), _tracker(TIME_INTERVAL_LOOK)
{
    if (!me->GetCharmInfo())
        throw InvalidAIException("Creature doesn't have a valid charm info");

    UpdateAllies();
}

void PetAI::UpdateAI(uint32 diff)
{
    if (!me->IsAlive() || !me->GetCharmInfo())
        return;

    Unit* owner = me->GetCharmerOrOwner();

    // 同步 DK 永久食尸鬼的黑暗突变模型。12.x aura 可能应用错误的 Change Model，
    // 因此从 creature_template 141244 解析 retail Hulking Horror 显示 ID。
    UpdateDkGhoulDarkTransformationModel();

    // 核心总是在 ScriptName 查找前为永久宠物分配 PetAI。在此处轮询主人真实威胁，
    // 因为 OwnerAttacked 在右键时触发，远距离近战还未真正命中。
    TryDkGhoulAssistOwnerAfterRealAttack();
    UpdateDkGhoulManualOwnerCombat();

    if (_updateAlliesTimer <= diff)
        // UpdateAllies self set update timer
        UpdateAllies();
    else
        _updateAlliesTimer -= diff;

    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
    {
        // is only necessary to stop casting, the pet must not exit combat
        if (!me->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && // ignore channeled spells (Pin, Seduction)
            me->EnsureVictim()->HasBreakableByDamageCrowdControlAura(me))
        {
            me->InterruptNonMeleeSpells(false);
            return;
        }

        if (NeedToStop())
        {
            TC_LOG_TRACE("scripts.ai.petai", "PetAI::UpdateAI: AI stopped attacking {}", me->GetGUID().ToString());
            StopAttack();
            return;
        }
    }
    else
    {
        if (me->HasReactState(REACT_AGGRESSIVE) || me->GetCharmInfo()->IsAtStay())
        {
            // Every update we need to check targets only in certain cases
            // Aggressive - Allow auto select if owner or pet don't have a target
            // Stay - Only pick from pet or owner targets / attackers so targets won't run by
            //   while chasing our owner. Don't do auto select.
            // All other cases (ie: defensive) - Targets are assigned by DamageTaken(), OwnerAttackedBy(), OwnerAttacked(), etc.
            Unit* nextTarget = SelectNextTarget(me->HasReactState(REACT_AGGRESSIVE));

            if (nextTarget)
                AttackStart(nextTarget);
            else
                HandleReturnMovement();
        }
        else
            HandleReturnMovement();
    }

    // Autocast (cast only in combat or persistent spells in any state)
    if (!me->HasUnitState(UNIT_STATE_CASTING))
    {
        TargetSpellList targetSpellStore;

        for (uint8 i = 0; i < me->GetPetAutoSpellSize(); ++i)
        {
            uint32 spellID = me->GetPetAutoSpellOnPos(i);
            if (!spellID)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID, me->GetMap()->GetDifficultyID());
            if (!spellInfo)
                continue;

            if (me->GetSpellHistory()->HasGlobalCooldown(spellInfo))
                continue;

            // check spell cooldown
            if (!me->GetSpellHistory()->IsReady(spellInfo))
                continue;

            if (spellInfo->IsPositive())
            {
                if (spellInfo->CanBeUsedInCombat(me))
                {
                    // Check if we're in combat or commanded to attack
                    if (!me->IsInCombat() && !me->GetCharmInfo()->IsCommandAttack())
                        continue;
                }

                Spell* spell = new Spell(me, spellInfo, TRIGGERED_NONE);
                bool spellUsed = false;

                // Some spells can target enemy or friendly (DK Ghoul's Leap)
                // Check for enemy first (pet then owner)
                Unit* target = me->getAttackerForHelper();
                if (!target && owner)
                    target = owner->getAttackerForHelper();

                if (target)
                {
                    if (CanAttack(target) && spell->CanAutoCast(target))
                    {
                        targetSpellStore.push_back(std::make_pair(target, spell));
                        spellUsed = true;
                    }
                }

                if (spellInfo->HasEffect(SPELL_EFFECT_JUMP_DEST))
                {
                    if (!spellUsed)
                        delete spell;
                    continue; // Pets must only jump to target
                }

                // No enemy, check friendly
                if (!spellUsed)
                {
                    for (ObjectGuid target : _allySet)
                    {
                        Unit* ally = ObjectAccessor::GetUnit(*me, target);

                        //only buff targets that are in combat, unless the spell can only be cast while out of combat
                        if (!ally)
                            continue;

                        if (spell->CanAutoCast(ally))
                        {
                            targetSpellStore.push_back(std::make_pair(ally, spell));
                            spellUsed = true;
                            break;
                        }
                    }
                }

                // No valid targets at all
                if (!spellUsed)
                    delete spell;
            }
            else if (me->GetVictim() && CanAttack(me->GetVictim()) && spellInfo->CanBeUsedInCombat(me))
            {
                Spell* spell = new Spell(me, spellInfo, TRIGGERED_NONE);
                if (spell->CanAutoCast(me->GetVictim()))
                    targetSpellStore.push_back(std::make_pair(me->GetVictim(), spell));
                else
                    delete spell;
            }
        }

        // found units to cast on to
        if (!targetSpellStore.empty())
        {
            TargetSpellList::iterator it = targetSpellStore.begin();
            std::advance(it, urand(0, targetSpellStore.size() - 1));

            Spell* spell  = (*it).second;
            Unit*  target = (*it).first;

            targetSpellStore.erase(it);

            // DK 食尸鬼黑暗突变期间，将基础技能替换为对应的强化技能
            DkGhoulOverrideCastResult const overrideResult =
                DkGhoulOverrideCastResult(
                    TryDkGhoulDarkTransformationOverride(spell, target));

            if (overrideResult == DkGhoulOverrideCastResult::Casted)
            {
                delete spell;
            }
            else if (overrideResult == DkGhoulOverrideCastResult::Blocked)
            {
                // 已施放强化技能拥有自己的冷却。黑暗突变激活期间不再施放普通版本
                delete spell;
            }
            else
            {
                SpellCastTargets targets;
                targets.SetUnitTarget(target);
                spell->prepare(targets);
            }
        }

        // deleted cached Spell objects
        for (std::pair<Unit*, Spell*> const& unitspellpair : targetSpellStore)
            delete unitspellpair.second;
    }

    // Update speed as needed to prevent dropping too far behind and despawning
    me->UpdateSpeed(MOVE_RUN);
    me->UpdateSpeed(MOVE_WALK);
    me->UpdateSpeed(MOVE_FLIGHT);

}

void PetAI::KilledUnit(Unit* victim)
{
    // Called from Unit::Kill() in case where pet or owner kills something
    // if owner killed this victim, pet may still be attacking something else
    if (me->GetVictim() && me->GetVictim() != victim)
        return;

    // Clear target just in case. May help problem where health / focus / mana
    // regen gets stuck. Also resets attack command.
    // Can't use StopAttack() because that activates movement handlers and ignores
    // next target selection
    me->AttackStop();
    me->InterruptNonMeleeSpells(false);

    // Before returning to owner, see if there are more things to attack
    if (Unit* nextTarget = SelectNextTarget(false))
        AttackStart(nextTarget);
    else
        HandleReturnMovement(); // Return
}

void PetAI::AttackStart(Unit* target)
{
    // Overrides Unit::AttackStart to prevent pet from switching off its assigned target
    if (!target || target == me)
        return;

    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    _AttackStart(target);
}

void PetAI::_AttackStart(Unit* target)
{
    // Check all pet states to decide if we can attack this target
    if (!CanAttack(target))
        return;

    // Only chase if not commanded to stay or if stay but commanded to attack
    DoAttack(target, (!me->GetCharmInfo()->HasCommandState(COMMAND_STAY) || me->GetCharmInfo()->IsCommandAttack()));
}

void PetAI::DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/)
{
    // DK 食尸鬼严格反应规则：
    // 防御模式只在食尸鬼真正受到伤害后才反应。仅仅进入战斗、被选为目标、
    // 产生威胁值或受到 0 伤害命中的情况都不算。
    if (IsDkRisenGhoul(me))
    {
        if (!attacker || !damage)
            return;

        _dkGhoulGhoulDamageTakenTargets.insert(attacker->GetGUID());

        // 食尸鬼真正受到伤害也是有效的防御触发器。让此事件打破旧的 Follow 召回，
        // 以便食尸鬼能够自卫。
        if (me->HasReactState(REACT_DEFENSIVE))
            if (CharmInfo* charmInfo = me->GetCharmInfo())
                charmInfo->SetIsCommandFollow(false);

        AttackStart(attacker);
        return;
    }

    AttackStart(attacker);
}

void PetAI::OwnerAttackedBy(Unit* attacker)
{
    // OwnerAttackedBy 由 Unit::DealDamage 在玩家主人真实受到伤害后调用。
    // DK 食尸鬼的防御模式可以对此事件反应，但协助模式不能。
    if (IsDkRisenGhoul(me) && attacker)
    {
        ObjectGuid const attackerGuid = attacker->GetGUID();

        _dkGhoulOwnerDamageTakenTargets.insert(attackerGuid);

        // 主人真实受到伤害是有效的防御触发器。如果玩家之前召回过食尸鬼，
        // 此新的真实事件允许打破旧的召回状态。
        if (me->HasReactState(REACT_DEFENSIVE))
            if (CharmInfo* charmInfo = me->GetCharmInfo())
                charmInfo->SetIsCommandFollow(false);

        _dkGhoulOwnerDirectCombatTargets.insert(attackerGuid);
    }

    // Called when owner takes damage. This function helps keep pets from running off
    //  simply due to owner gaining aggro.

    if (!attacker || !me->IsAlive())
        return;

    // Passive pets don't do anything
    if (me->HasReactState(REACT_PASSIVE))
        return;

    // Retail 风格的 DK 协助模式仅在主人主动攻击后才反应；
    // 防御模式在主人受击时仍会反应。
    if (IsDkRisenGhoul(me) && me->HasReactState(REACT_ASSIST))
        return;

    // Prevent pet from disengaging from current target
    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    // Continue to evaluate and attack if necessary
    AttackStart(attacker);
}

void PetAI::OwnerDealtDamage(Unit* target)
{
    if (!IsDkRisenGhoul(me) || !target)
        return;

    // 此回调只对实际攻击者是主人玩家的伤害触发。食尸鬼的横扫爪击即使记入主人威胁值，
    // 攻击者仍是食尸鬼，不会错误地把宠物代理转成主人战斗。
    ObjectGuid const targetGuid = target->GetGUID();

    _dkGhoulOwnerDamageDealtTargets.insert(targetGuid);

    // 主人亲自对该目标造成了伤害，但只要食尸鬼仍在远程为其战斗，食尸鬼仍可能是
    // 主人必须保持战斗的原因。此处不要擦除宠物拥有的主人战斗代理；召回/leash
    // 应当由战斗/威胁自然解决，而不是通过强制的 EndCombat。
    _dkGhoulOwnerDirectCombatTargets.insert(targetGuid);

    // 官服：玩家召回食尸鬼（点了Follow）后，DOT持续伤害不打破召回命令。
    // 只有玩家手动攻击、手动Attack、或切换协助/防御状态才重新协助。
    CharmInfo* charmInfo = me->GetCharmInfo();
    if (charmInfo && charmInfo->IsCommandFollow())
        return;

    // 新的真实主人伤害事件允许打破旧的显式 Follow 召回。没有新伤害时，
    // 旧的 victim/威胁状态不应当在玩家点击 Follow 后把食尸鬼拉回去。
    if (charmInfo)
        charmInfo->SetIsCommandFollow(false);

    // 真实伤害事件是协助/防御模式的权威触发器。
    // 不要仅依赖 UpdateAI 轮询 owner->GetVictim()；有些攻击和法术能造成伤害
    // 而不留下稳定的 owner victim 给下一次 AI 更新。下方的 CanAttack() 仍然
    // 强制执行协助/防御/被动和命令状态规则。
    if (me->IsAlive() && !me->HasReactState(REACT_PASSIVE))
        AttackStart(target);
}

void PetAI::OwnerAttacked(Unit* target)
{
    // Called when owner attacks something. Allows defensive pets to know
    //  that they need to assist

    // Target might be NULL if called from spell with invalid cast targets
    if (!target || !me->IsAlive())
        return;

    // Passive pets don't do anything
    if (me->HasReactState(REACT_PASSIVE))
        return;

    // Prevent pet from disengaging from current target
    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    // Unit::Attack 在主人右键时立刻通知宠物。远距离右键只是攻击意图，
    // 必须等到主人产生真实威胁后才让食尸鬼介入。
    if (IsDkRisenGhoul(me) && !OwnerReallyAttackedTarget(me, target))
        return;

    // 官服：召回状态下玩家右键打怪，食尸鬼不协助。
    // 必须由玩家手动点Attack命令、切协助/防御反应状态、或重新召唤才打破召回。
    if (IsDkRisenGhoul(me) && me->GetCharmInfo() && me->GetCharmInfo()->IsCommandFollow())
        return;

    // Continue to evaluate and attack if necessary
    AttackStart(target);
}

Unit* PetAI::SelectNextTarget(bool allowAutoSelect) const
{
    // Provides next target selection after current target death.
    // This function should only be called internally by the AI
    // Targets are not evaluated here for being valid targets, that is done in _CanAttack()
    // The parameter: allowAutoSelect lets us disable aggressive pet auto targeting for certain situations

    // Passive pets don't do next target selection
    if (me->HasReactState(REACT_PASSIVE))
        return nullptr;

    // Check pet attackers first so we don't drag a bunch of targets to the owner
    if (Unit* myAttacker = me->getAttackerForHelper())
        if (!myAttacker->HasBreakableByDamageCrowdControlAura())
            return myAttacker;

    // Not sure why we wouldn't have an owner but just in case...
    if (!me->GetCharmerOrOwner())
        return nullptr;

    // Check owner attackers
    if (Unit* ownerAttacker = me->GetCharmerOrOwner()->getAttackerForHelper())
        if (!ownerAttacker->HasBreakableByDamageCrowdControlAura())
            return ownerAttacker;

    // Check owner victim
    // 3.0.2 - Pets now start attacking their owners victim in defensive mode as soon as the hunter does
    if (Unit* ownerVictim = me->GetCharmerOrOwner()->GetVictim())
    {
        // owner->GetVictim() 在远距离右键时即被设置。在主人尚未产生威胁之前，
        // 不要将其作为 DK 食尸鬼的真实协助触发目标。
        if (!IsDkRisenGhoul(me) || OwnerReallyAttackedTarget(me, ownerVictim))
            return ownerVictim;
    }

    // Neither pet or owner had a target and aggressive pets can pick any target
    // To prevent aggressive pets from chain selecting targets and running off, we
    //  only select a random target if certain conditions are met.
    if (me->HasReactState(REACT_AGGRESSIVE) && allowAutoSelect)
    {
        if (!me->GetCharmInfo()->IsReturning() || me->GetCharmInfo()->IsFollowing() || me->GetCharmInfo()->IsAtStay())
            if (Unit* nearTarget = me->SelectNearestHostileUnitInAggroRange(true, true))
                return nearTarget;
    }

    // Default - no valid targets
    return nullptr;
}

void PetAI::HandleReturnMovement()
{
    // Handles moving the pet back to stay or owner

    // Prevent activating movement when under control of spells
    // such as "Eyes of the Beast"
    if (me->IsCharmed())
        return;

    if (!me->GetCharmInfo())
    {
        TC_LOG_WARN("scripts.ai.petai", "me->GetCharmInfo() is NULL in PetAI::HandleReturnMovement(). Debug info: {}", GetDebugInfo());
        return;
    }

    if (me->GetCharmInfo()->HasCommandState(COMMAND_STAY) ||
        me->GetCharmInfo()->HasCommandState(COMMAND_MOVE_TO))
    {
        if (!me->GetCharmInfo()->IsAtStay() && !me->GetCharmInfo()->IsReturning())
        {
            // 返回 Stay 或 Move To 点击时保存的守卫点位置。
            float x, y, z;

            me->GetCharmInfo()->GetStayPosition(x, y, z);
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsReturning(true);

            if (me->HasUnitState(UNIT_STATE_CHASE))
                me->GetMotionMaster()->Remove(CHASE_MOTION_TYPE);

            me->GetMotionMaster()->MovePoint(me->GetGUID().GetCounter(), x, y, z);
        }
    }
    else // COMMAND_FOLLOW
    {
        if (!me->GetCharmInfo()->IsFollowing() && !me->GetCharmInfo()->IsReturning())
        {
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsReturning(true);

            if (me->HasUnitState(UNIT_STATE_CHASE))
                me->GetMotionMaster()->Remove(CHASE_MOTION_TYPE);

            me->GetMotionMaster()->MoveFollow(me->GetCharmerOrOwner(), PET_FOLLOW_DIST, me->GetFollowAngle());
        }
    }
    me->RemoveUnitFlag(UNIT_FLAG_PET_IN_COMBAT); // on player pets, this flag indicates that we're actively going after a target - we're returning, so remove it
}

void PetAI::DoAttack(Unit* target, bool chase)
{
    // Handles attack with or without chase and also resets flags
    // for next update / creature kill

    if (me->Attack(target, true))
    {
        me->SetUnitFlag(UNIT_FLAG_PET_IN_COMBAT); // on player pets, this flag indicates we're actively going after a target - that's what we're doing, so set it
        // Play sound to let the player know the pet is attacking something it picked on its own
        if (me->HasReactState(REACT_AGGRESSIVE) && !me->GetCharmInfo()->IsCommandAttack())
            me->SendPetAIReaction(me->GetGUID());

        if (chase)
        {
            bool oldCmdAttack = me->GetCharmInfo()->IsCommandAttack(); // This needs to be reset after other flags are cleared
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsCommandAttack(oldCmdAttack); // For passive pets commanded to attack so they will use spells

            if (me->HasUnitState(UNIT_STATE_FOLLOW))
                me->GetMotionMaster()->Remove(FOLLOW_MOTION_TYPE);

            // Pets with ranged attacks should not care about the chase angle at all.
            float chaseDistance = me->GetPetChaseDistance();
            float angle = chaseDistance == 0.f ? float(M_PI) : 0.f;
            float tolerance = chaseDistance == 0.f ? float(M_PI_4) : float(M_PI * 2);
            me->GetMotionMaster()->MoveChase(target, ChaseRange(0.f, chaseDistance), ChaseAngle(angle, tolerance));
        }
        else // (Stay && ((Aggressive || Defensive) && In Melee Range)))
        {
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsAtStay(true);

            if (me->HasUnitState(UNIT_STATE_FOLLOW))
                me->GetMotionMaster()->Remove(FOLLOW_MOTION_TYPE);

            me->GetMotionMaster()->MoveIdle();
        }
    }
}

void PetAI::MovementInform(uint32 type, uint32 id)
{
    // Receives notification when pet reaches stay or follow owner
    switch (type)
    {
        case POINT_MOTION_TYPE:
        {
            // Pet is returning to where stay was clicked. data should be
            // pet's GUIDLow since we set that as the waypoint ID
            if (id == me->GetGUID().GetCounter() && me->GetCharmInfo()->IsReturning())
            {
                // 对 Move To 而言，保存实际到达的位置。点击处理器在食尸鬼抵达前
                // 就启动了移动，若在点击时保存会记录旧位置。
                me->GetCharmInfo()->SaveStayPosition();
                ClearCharmInfoFlags();
                me->GetCharmInfo()->SetIsAtStay(true);
                me->GetMotionMaster()->MoveIdle();
            }
            break;
        }
        case FOLLOW_MOTION_TYPE:
        {
            // If data is owner's GUIDLow then we've reached follow point,
            // otherwise we're probably chasing a creature
            if (me->GetCharmerOrOwner() && me->GetCharmInfo() && id == me->GetCharmerOrOwner()->GetGUID().GetCounter() && me->GetCharmInfo()->IsReturning())
            {
                bool const wasExplicitFollowCommand = me->GetCharmInfo()->IsCommandFollow();

                ClearCharmInfoFlags();
                me->GetCharmInfo()->SetIsFollowing(true);

                // 在食尸鬼抵达主人后保持显式的玩家 Follow 召回粘滞状态。
                // 否则旧的 owner victim/威胁状态可能立刻让
                // TryDkGhoulAssistOwnerAfterRealAttack() 把食尸鬼送回同一只怪物。
                if (wasExplicitFollowCommand)
                    me->GetCharmInfo()->SetIsCommandFollow(true);
            }
            break;
        }
        default:
            break;
    }
}

bool PetAI::CanAttack(Unit* target)
{
    // Evaluates wether a pet can attack a specific target based on CommandState, ReactState and other flags
    // IMPORTANT: The order in which things are checked is important, be careful if you add or remove checks

    // Hmmm...
    if (!target)
        return false;

    if (!target->IsAlive())
    {
        // if target is invalid, pet should evade automaticly
        // Clear target to prevent getting stuck on dead targets
        //me->AttackStop();
        //me->InterruptNonMeleeSpells(false);
        return false;
    }

    if (!me->GetCharmInfo())
    {
        TC_LOG_WARN("scripts.ai.petai", "me->GetCharmInfo() is NULL in PetAI::CanAttack(). Debug info: {}", GetDebugInfo());
        return false;
    }

    // Passive - passive pets can attack if told to
    if (me->HasReactState(REACT_PASSIVE))
        return me->GetCharmInfo()->IsCommandAttack();

    bool dkGhoulAcceptedRealAutoTarget = false;

    if (IsDkRisenGhoul(me) && !me->GetCharmInfo()->IsCommandAttack())
    {
        ObjectGuid const targetGuid = target->GetGUID();

        // 协助模式刻意严格：必须由玩家主人亲自对该目标造成真实伤害。
        // 仅右键、主人 victim、食尸鬼伤害产生的威胁值或目标选择都不够。
        bool const ownerReallyDealtDamage =
            _dkGhoulOwnerDamageDealtTargets.find(targetGuid) !=
            _dkGhoulOwnerDamageDealtTargets.end();

        // 防御模式还可以在主人真正被该目标伤害，或食尸鬼自身真正被该目标伤害时反应。
        // 这些只从真实伤害回调中记录。
        bool const ownerReallyTookDamage =
            _dkGhoulOwnerDamageTakenTargets.find(targetGuid) !=
            _dkGhoulOwnerDamageTakenTargets.end();

        bool const ghoulReallyTookDamage =
            _dkGhoulGhoulDamageTakenTargets.find(targetGuid) !=
            _dkGhoulGhoulDamageTakenTargets.end();

        if (me->HasReactState(REACT_ASSIST))
        {
            if (!ownerReallyDealtDamage)
                return false;

            dkGhoulAcceptedRealAutoTarget = true;
        }
        else if (me->HasReactState(REACT_DEFENSIVE))
        {
            if (!ownerReallyDealtDamage && !ownerReallyTookDamage && !ghoulReallyTookDamage)
                return false;

            dkGhoulAcceptedRealAutoTarget = true;
        }
    }

    // CC - mobs under crowd control can be attacked if owner commanded
    if (target->HasBreakableByDamageCrowdControlAura())
        return me->GetCharmInfo()->IsCommandAttack();

    // 手动宠物栏 Attack 拥有最高优先级。它必须取消之前 Move To/Stay 命令的实际效果，
    // 否则被派到某点的食尸鬼会拒绝追击主人选中的目标。
    if (me->GetCharmInfo()->IsCommandAttack())
        return true;

    // 普通 Follow 移动中的 DK 食尸鬼可能在它仅仅是追赶主人时被标记为 Returning。
    // 一旦真实的 owner/食尸鬼伤害事件接受了此目标，该 follow-return 标志就不应
    // 再阻止协助或防御。Stay 和 Move To 在下方处理，保持各自的规则。
    if (dkGhoulAcceptedRealAutoTarget &&
        me->GetCharmInfo()->HasCommandState(COMMAND_FOLLOW) &&
        !me->GetCharmInfo()->IsCommandFollow())
        return true;

    // Returning - pets ignore attacks only if owner clicked follow
    if (me->GetCharmInfo()->IsReturning())
        return !me->GetCharmInfo()->IsCommandFollow();

    // Stay - 仅当目标已在近战范围内才能攻击。手动 Attack 已在上方处理，允许追击。
    if (me->GetCharmInfo()->HasCommandState(COMMAND_STAY))
        return me->IsWithinMeleeRange(target);

    // DK 食尸鬼的 Move To 是保存的守卫点，不是永久阻止攻击。
    // 若上方的协助/防御检查接受了该目标，允许食尸鬼离开守卫点去帮忙；
    // 战斗结束后 HandleReturnMovement 会把它送回，因为 COMMAND_MOVE_TO 仍是活动命令状态。
    if (me->GetCharmInfo()->HasCommandState(COMMAND_MOVE_TO))
        return IsDkRisenGhoul(me);

    //  Pets attacking something (or chasing) should only switch targets if owner tells them to
    if (me->GetVictim() && me->GetVictim() != target)
    {
        // Check if our owner selected this target and clicked "attack"
        Unit* ownerTarget = nullptr;
        if (Player* owner = me->GetCharmerOrOwner()->ToPlayer())
            ownerTarget = owner->GetSelectedUnit();
        else
            ownerTarget = me->GetCharmerOrOwner()->GetVictim();

        if (ownerTarget && me->GetCharmInfo()->IsCommandAttack())
            return (target->GetGUID() == ownerTarget->GetGUID());
    }

    // Follow
    if (me->GetCharmInfo()->HasCommandState(COMMAND_FOLLOW))
        return !me->GetCharmInfo()->IsReturning();

    // default, though we shouldn't ever get here
    return false;
}


void PetAI::JustEnteredCombat(Unit* who)
{
    EngagementStart(who);

    // DK 食尸鬼真实接敌后，建立主人战斗代理（CombatReference）。
    // 不能在玩家右键时立即让主人进战斗。
    if (IsDkRisenGhoul(me))
        StartDkGhoulManualOwnerCombat(who);
}

void PetAI::JustExitedCombat()
{
    // 食尸鬼退出战斗时，清理由宠物拥有的玩家战斗代理
    if (IsDkRisenGhoul(me))
        ClearDkGhoulManualOwnerCombat();

    EngagementOver();
}

void PetAI::OnDespawn()
{
    // 食尸鬼被 despawn 时，清理由宠物拥有的玩家战斗代理
    if (IsDkRisenGhoul(me))
        ClearDkGhoulManualOwnerCombat();
}

// ===== DK 食尸鬼黑暗突变：自动施法时把基础技能替换为对应的强化技能 =====
uint8 PetAI::TryDkGhoulDarkTransformationOverride(
    Spell* baseSpell, Unit* target)
{
    if (!IsDkRisenGhoul(me) || !baseSpell || !target)
        return uint8(DkGhoulOverrideCastResult::NotApplicable);

    SpellInfo const* baseSpellInfo = baseSpell->GetSpellInfo();
    uint32 const overrideSpellId =
        GetDkGhoulDarkTransformationOverrideSpellId(me, baseSpellInfo->Id);
    if (!overrideSpellId)
        return uint8(DkGhoulOverrideCastResult::NotApplicable);

    SpellInfo const* overrideSpellInfo = sSpellMgr->GetSpellInfo(
        overrideSpellId, me->GetMap()->GetDifficultyID());
    if (!overrideSpellInfo)
        return uint8(DkGhoulOverrideCastResult::NotApplicable);

    // 不要因为强化版在冷却或食尸鬼能量不足就回退到普通版本。
    // triggered 的强化法术会绕过核心正常的 TakePower 流程。
    if (!me->GetSpellHistory()->IsReady(overrideSpellInfo) ||
        !HasDkGhoulBaseSpellPower(me, baseSpell))
        return uint8(DkGhoulOverrideCastResult::Blocked);

    Spell* overrideSpell =
        new Spell(me, overrideSpellInfo, TRIGGERED_FULL_MASK);

    SpellCastResult const result = overrideSpell->CheckPetCast(target);
    if (result != SPELL_CAST_OK)
    {
        delete overrideSpell;
        return uint8(DkGhoulOverrideCastResult::NotApplicable);
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(target);
    overrideSpell->prepare(targets);

    ConsumeDkGhoulBaseSpellPower(me, baseSpell);
    StartDkGhoulOverrideCooldown(me, baseSpellInfo, overrideSpellInfo);
    return uint8(DkGhoulOverrideCastResult::Casted);
}

// ===== DK 食尸鬼黑暗突变：模型同步 =====
void PetAI::UpdateDkGhoulDarkTransformationModel()
{
    if (!IsDkRisenGhoul(me))
        return;

    Unit* owner = me->GetCharmerOrOwner();
    bool const transformed =
        HasDkDarkTransformationAura(me) ||
        HasDkDarkTransformationAura(owner);

    if (transformed)
    {
        // 从 world.creature_template 解析模型，而不是硬编码 display ID。
        // 这样修复可以兼容当前服务器构建加载的 DB2 与 creature 模型数据。
        if (!_dkGhoulDarkTransformationDisplayId)
        {
            if (CreatureTemplate const* transformationTemplate =
                sObjectMgr->GetCreatureTemplate(NPC_DK_HULKING_HORROR))
            {
                if (CreatureModel const* model =
                    ObjectMgr::ChooseDisplayId(transformationTemplate))
                {
                    _dkGhoulDarkTransformationDisplayId =
                        model->CreatureDisplayID;
                }
            }
        }

        // 仅在另一个 aura 效果已覆盖 display 时重新应用。
        if (_dkGhoulDarkTransformationDisplayId &&
            me->GetDisplayId() != _dkGhoulDarkTransformationDisplayId)
        {
            // 不修改 NativeDisplayID。它包含食尸鬼原始随机外观，
            // aura 过期后还需要它来还原。
            me->SetDisplayId(_dkGhoulDarkTransformationDisplayId);
        }

        return;
    }

    // 只还原由本修复所拥有的模型。如果其他变形已改变模型，则保持不动。
    if (_dkGhoulDarkTransformationDisplayId)
    {
        if (me->GetDisplayId() == _dkGhoulDarkTransformationDisplayId)
            me->RestoreDisplayId();

        _dkGhoulDarkTransformationDisplayId = 0;
    }
}

// ===== DK 食尸鬼：检测主人真实接敌后协助攻击 =====
void PetAI::TryDkGhoulAssistOwnerAfterRealAttack()
{
    if (!IsDkRisenGhoul(me) || me->GetVictim() || me->HasReactState(REACT_PASSIVE))
        return;

    CharmInfo* charmInfo = me->GetCharmInfo();
    if (!charmInfo || charmInfo->IsCommandAttack())
        return;

    // 玩家点击了 Follow/召回。旧记录不应立刻把食尸鬼拉回同一只怪物。
    // 新的真实事件会直接调用 OwnerDealtDamage()、OwnerAttackedBy() 或 DamageTaken()。
    if (charmInfo->IsCommandFollow())
        return;

    Unit* owner = me->GetCharmerOrOwner();
    Unit* target = owner ? owner->GetVictim() : nullptr;
    if (!target)
        return;

    ObjectGuid const targetGuid = target->GetGUID();

    bool const ownerReallyDealtDamage =
        _dkGhoulOwnerDamageDealtTargets.find(targetGuid) !=
        _dkGhoulOwnerDamageDealtTargets.end();

    bool const ownerReallyTookDamage =
        _dkGhoulOwnerDamageTakenTargets.find(targetGuid) !=
        _dkGhoulOwnerDamageTakenTargets.end();

    bool const ghoulReallyTookDamage =
        _dkGhoulGhoulDamageTakenTargets.find(targetGuid) !=
        _dkGhoulGhoulDamageTakenTargets.end();

    // 不要用 owner->GetVictim()、战斗状态或威胁值来替代伤害。
    // 在任何人真正受到或造成伤害之前，这些都可能由右键/aggro/战斗姿态创建。
    if (me->HasReactState(REACT_ASSIST))
    {
        if (!ownerReallyDealtDamage)
            return;
    }
    else if (me->HasReactState(REACT_DEFENSIVE))
    {
        if (!ownerReallyDealtDamage && !ownerReallyTookDamage && !ghoulReallyTookDamage)
            return;
    }
    else
        return;

    AttackStart(target);
}

// ===== DK 食尸鬼：食尸鬼真实接敌后建立主人战斗代理 =====
void PetAI::StartDkGhoulManualOwnerCombat(Unit* target)
{
    if (!IsDkRisenGhoul(me) || !target)
        return;

    Unit* owner = me->GetCharmerOrOwner();
    if (!owner || !me->IsInCombatWith(target))
        return;

    ObjectGuid const targetGuid = target->GetGUID();
    _dkGhoulCombatTargets.insert(targetGuid);

    // 当食尸鬼仍在与某目标真实战斗时，主人也必须保持战斗状态。
    // 即使主人之前对该目标造成过伤害然后走开也是如此：只要永久食尸鬼仍在
    // 为主人战斗，retail 就让主人保持战斗状态。
    if (!owner->IsInCombatWith(target))
        owner->SetInCombatWith(target, true);

    if (owner->IsInCombatWith(target))
        _dkGhoulOwnedOwnerCombatRefs.insert(targetGuid);
}

// ===== DK 食尸鬼：每 tick 更新战斗代理归属关系，清理失效的战斗关系 =====
void PetAI::UpdateDkGhoulManualOwnerCombat()
{
    if (!IsDkRisenGhoul(me))
        return;

    Unit* owner = me->GetCharmerOrOwner();
    if (!owner)
    {
        ClearDkGhoulManualOwnerCombat();
        return;
    }

    GuidSet currentCombatTargets;

    auto collectCombatTargets = [&](auto const& combatRefs)
    {
        for (auto const& [guid, combatRef] : combatRefs)
        {
            Unit* target = combatRef ? combatRef->GetOther(me) : nullptr;
            if (!target || target == owner || !target->IsAlive() ||
                me->IsFriendlyTo(target))
                continue;

            currentCombatTargets.insert(guid);
            StartDkGhoulManualOwnerCombat(target);
        }
    };

    // 横扫爪击与其他变形攻击可能创建多个战斗引用，即使 me->GetVictim() 只暴露主目标。
    collectCombatTargets(me->GetCombatManager().GetPvECombatRefs());
    collectCombatTargets(me->GetCombatManager().GetPvPCombatRefs());

    // 食尸鬼不再与这些目标有真实战斗引用。
    // 此处不要在主人身上强制 EndCombat。如果怪物仍有有效威胁或仍在追击，
    // 战斗应当保持。如果没人再与之战斗，正常的 evade/leash/威胁系统会结束战斗。
    GuidSet endedTargets;
    for (ObjectGuid const& guid : _dkGhoulCombatTargets)
        if (currentCombatTargets.find(guid) == currentCombatTargets.end())
            endedTargets.insert(guid);

    for (ObjectGuid const& guid : endedTargets)
    {
        bool cleanupComplete = true;

        if (_dkGhoulOwnedOwnerCombatRefs.find(guid) !=
            _dkGhoulOwnedOwnerCombatRefs.end())
            cleanupComplete = TryEndDkGhoulOwnerCombatProxy(me, owner, guid);

        // 如果当前还不能安全清理，保留跟踪记录。下一次 UpdateAI tick 会重试。
        // 旧版本即使在清理失败时也擦除了这些记录，会让主人战斗代理卡死。
        if (!cleanupComplete)
            continue;

        _dkGhoulCombatTargets.erase(guid);
        _dkGhoulOwnedOwnerCombatRefs.erase(guid);
        _dkGhoulOwnerDirectCombatTargets.erase(guid);
        _dkGhoulOwnerDamageDealtTargets.erase(guid);
        _dkGhoulOwnerDamageTakenTargets.erase(guid);
        _dkGhoulGhoulDamageTakenTargets.erase(guid);
    }
}

// ===== DK 食尸鬼：清理所有由宠物拥有的主人战斗代理（退出战斗/despawn/死亡时调用） =====
void PetAI::ClearDkGhoulManualOwnerCombat()
{
    Unit* owner = me->GetCharmerOrOwner();

    // 召回/被动不能立刻强制怪物 evade，但永久留下主人代理会让主人卡在战斗中。
    // 只移除已完成的代理引用。如果怪物仍在与食尸鬼或主人战斗/追击，保留记录，
    // 以便 UpdateAI 在稍后重试清理。
    if (!owner)
    {
        _dkGhoulCombatTargets.clear();
        _dkGhoulOwnedOwnerCombatRefs.clear();
        _dkGhoulOwnerDirectCombatTargets.clear();
        _dkGhoulOwnerDamageDealtTargets.clear();
        _dkGhoulOwnerDamageTakenTargets.clear();
        _dkGhoulGhoulDamageTakenTargets.clear();
        return;
    }

    GuidSet trackedRefs = _dkGhoulCombatTargets;
    trackedRefs.insert(_dkGhoulOwnedOwnerCombatRefs.begin(),
        _dkGhoulOwnedOwnerCombatRefs.end());

    for (ObjectGuid const& guid : trackedRefs)
    {
        bool cleanupComplete = true;

        if (_dkGhoulOwnedOwnerCombatRefs.find(guid) !=
            _dkGhoulOwnedOwnerCombatRefs.end())
            cleanupComplete = TryEndDkGhoulOwnerCombatProxy(me, owner, guid);

        if (!cleanupComplete)
            continue;

        _dkGhoulCombatTargets.erase(guid);
        _dkGhoulOwnedOwnerCombatRefs.erase(guid);
        _dkGhoulOwnerDirectCombatTargets.erase(guid);
        _dkGhoulOwnerDamageDealtTargets.erase(guid);
        _dkGhoulOwnerDamageTakenTargets.erase(guid);
        _dkGhoulGhoulDamageTakenTargets.erase(guid);
    }
}

void PetAI::ReceiveEmote(Player* player, uint32 emote)
{
    if (me->GetOwnerGUID() != player->GetGUID())
        return;

    switch (emote)
    {
        case TEXT_EMOTE_COWER:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(/*EMOTE_ONESHOT_ROAR*/EMOTE_ONESHOT_OMNICAST_GHOUL);
            break;
        case TEXT_EMOTE_ANGRY:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(/*EMOTE_ONESHOT_COWER*/EMOTE_STATE_STUN);
            break;
        case TEXT_EMOTE_GLARE:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(EMOTE_STATE_STUN);
            break;
        case TEXT_EMOTE_SOOTHE:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(EMOTE_ONESHOT_OMNICAST_GHOUL);
            break;
    }
}

bool PetAI::NeedToStop()
{
    // This is needed for charmed creatures, as once their target was reset other effects can trigger threat
    if (me->IsCharmed() && me->GetVictim() == me->GetCharmer())
        return true;

    // dont allow pets to follow targets far away from owner
    if (Unit* owner = me->GetCharmerOrOwner())
        if (owner->GetExactDist(me) >= (owner->GetVisibilityRange() - 10.0f))
            return true;

    return !me->IsValidAttackTarget(me->GetVictim());
}

void PetAI::StopAttack()
{
    if (!me->IsAlive())
    {
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveIdle();
        me->CombatStop();
        return;
    }

    me->AttackStop();
    me->InterruptNonMeleeSpells(false);
    me->GetCharmInfo()->SetIsCommandAttack(false);
    ClearCharmInfoFlags();
    HandleReturnMovement();
}

void PetAI::UpdateAllies()
{
    _updateAlliesTimer = 10 * IN_MILLISECONDS; // update friendly targets every 10 seconds, lesser checks increase performance

    Unit* owner = me->GetCharmerOrOwner();
    if (!owner)
        return;

    Group* group = nullptr;
    if (Player* player = owner->ToPlayer())
        group = player->GetGroup();

    // only pet and owner/not in group->ok
    if (_allySet.size() == 2 && !group)
        return;

    // owner is in group; group members filled in already (no raid -> subgroupcount = whole count)
    if (group && !group->isRaidGroup() && _allySet.size() == (group->GetMembersCount() + 2))
        return;

    _allySet.clear();
    _allySet.insert(me->GetGUID());
    if (group) // add group
    {
        for (GroupReference const& itr : group->GetMembers())
        {
            Player* Target = itr.GetSource();
            if (!Target->IsInMap(owner) || !group->SameSubGroup(owner->ToPlayer(), Target))
                continue;

            if (Target->GetGUID() == owner->GetGUID())
                continue;

            _allySet.insert(Target->GetGUID());
        }
    }
    else // remove group
        _allySet.insert(owner->GetGUID());
}

void PetAI::OnCharmed(bool isNew)
{
    if (!me->isPossessedByPlayer() && me->IsCharmed())
        me->GetMotionMaster()->MoveFollow(me->GetCharmer(), PET_FOLLOW_DIST, me->GetFollowAngle());

    CreatureAI::OnCharmed(isNew);
}

void PetAI::ClearCharmInfoFlags()
{
    CharmInfo* ci = me->GetCharmInfo();
    if (ci)
    {
        ci->SetIsAtStay(false);
        ci->SetIsCommandAttack(false);
        ci->SetIsCommandFollow(false);
        ci->SetIsFollowing(false);
        ci->SetIsReturning(false);
    }
}
