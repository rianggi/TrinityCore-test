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

#include "WorldSession.h"
#include "CharmInfo.h"
#include "Common.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PetPackets.h"
#include "Player.h"
#include "QueryPackets.h"
#include "Spell.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellPackets.h"
#include "PetAI.h"
#include "Util.h"


namespace
{
    // DK 永久食尸鬼（Raise Dead 永久宠物）的 Creature Entry
    uint32 constexpr NPC_DK_RISEN_GHOUL = 26125;

    // 黑暗突变识别 aura：
    //   63560    旧版黑暗突变
    //   1233448  12.x 黑暗突变
    //   1235391  黑暗突变效果 aura
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION_LEGACY = 63560;
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION = 1233448;
    uint32 constexpr SPELL_DK_DARK_TRANSFORMATION_EFFECT = 1235391;

    // 客户端显示的食尸鬼基础技能 ID（宠物动作条图标）
    uint32 constexpr SPELL_DK_GHOUL_CLAW_DISPLAY = 47468;
    uint32 constexpr SPELL_DK_GHOUL_GNAW_DISPLAY = 47481;
    uint32 constexpr SPELL_DK_GHOUL_LEAP_DISPLAY = 47482;
    uint32 constexpr SPELL_DK_GHOUL_HUDDLE_DISPLAY = 47484;

    // 服务端实际执行的食尸鬼基础技能 ID
    uint32 constexpr SPELL_DK_GHOUL_CLAW_EXECUTION = 91776;
    uint32 constexpr SPELL_DK_GHOUL_GNAW_EXECUTION = 91800;
    uint32 constexpr SPELL_DK_GHOUL_LEAP_EXECUTION = 91809;
    uint32 constexpr SPELL_DK_GHOUL_HUDDLE_EXECUTION = 91838;

    // 黑暗突变后的强化技能 ID
    uint32 constexpr SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY = 91778;
    uint32 constexpr SPELL_DK_GHOUL_SWEEPING_CLAWS = 1278150;
    uint32 constexpr SPELL_DK_GHOUL_MONSTROUS_BLOW = 91797;
    uint32 constexpr SPELL_DK_GHOUL_SHAMBLING_RUSH = 91802;

    // 判断单位是否为 DK 永久食尸鬼（Creature Entry 26125）
    bool IsRisenGhoul(Unit const* unit)
    {
        return unit && unit->GetTypeId() == TYPEID_UNIT && unit->GetEntry() == NPC_DK_RISEN_GHOUL;
    }

    // 获取食尸鬼自动释放技能对应的服务端执行技能 ID
    // 仅返回允许自动释放的三个技能：爪击、撕扯、跳跃
    uint32 GetDkGhoulAutocastExecutionSpellId(uint32 spellId)
    {
        switch (spellId)
        {
            case SPELL_DK_GHOUL_CLAW_DISPLAY:
            case SPELL_DK_GHOUL_CLAW_EXECUTION:
                return SPELL_DK_GHOUL_CLAW_EXECUTION;
            case SPELL_DK_GHOUL_GNAW_DISPLAY:
            case SPELL_DK_GHOUL_GNAW_EXECUTION:
                return SPELL_DK_GHOUL_GNAW_EXECUTION;
            case SPELL_DK_GHOUL_LEAP_DISPLAY:
            case SPELL_DK_GHOUL_LEAP_EXECUTION:
                return SPELL_DK_GHOUL_LEAP_EXECUTION;
            default:
                break;
        }

        return 0;
    }

    // 获取食尸鬼技能对应的服务端执行技能 ID
    // 包含所有四个技能：爪击、撕扯、跳跃、蜷缩
    uint32 GetDkGhoulExecutionSpellId(uint32 spellId)
    {
        switch (spellId)
        {
            case SPELL_DK_GHOUL_CLAW_DISPLAY:
                return SPELL_DK_GHOUL_CLAW_EXECUTION;
            case SPELL_DK_GHOUL_GNAW_DISPLAY:
                return SPELL_DK_GHOUL_GNAW_EXECUTION;
            case SPELL_DK_GHOUL_LEAP_DISPLAY:
                return SPELL_DK_GHOUL_LEAP_EXECUTION;
            case SPELL_DK_GHOUL_HUDDLE_DISPLAY:
                return SPELL_DK_GHOUL_HUDDLE_EXECUTION;
            default:
                break;
        }

        return spellId;
    }

    // 获取宠物动作条上存储的技能 ID
    // 旧的 474xx ID 仅作为输入别名接受，最终存储为 9xxxx
    uint32 GetDkGhoulActionBarSpellId(uint32 spellId)
    {
        return GetDkGhoulExecutionSpellId(spellId);
    }

    // 判断技能是否为蜷缩技能
    bool IsDkGhoulHuddleSpell(uint32 spellId)
    {
        return spellId == SPELL_DK_GHOUL_HUDDLE_DISPLAY ||
            spellId == SPELL_DK_GHOUL_HUDDLE_EXECUTION;
    }

    // 判断单位是否拥有任意版本的黑暗突变 aura
    bool HasDkDarkTransformationAura(Unit const* unit)
    {
        return unit &&
            (unit->HasAura(SPELL_DK_DARK_TRANSFORMATION_LEGACY) ||
             unit->HasAura(SPELL_DK_DARK_TRANSFORMATION) ||
             unit->HasAura(SPELL_DK_DARK_TRANSFORMATION_EFFECT));
    }

    // 获取黑暗突变状态下对应基础技能的强化技能 ID。
    // 仅对 DK 永久食尸鬼且处于黑暗突变状态下生效。
    // 返回 0 表示无需替换为基础技能执行。
    uint32 GetDkGhoulDarkTransformationOverrideSpellId(
        Unit const* caster, uint32 baseSpellId)
    {
        if (!IsRisenGhoul(caster) ||
            (!HasDkDarkTransformationAura(caster) &&
             !HasDkDarkTransformationAura(caster->GetCharmerOrOwner())))
            return 0;

        Difficulty const difficulty = caster->GetMap()->GetDifficultyID();

        switch (baseSpellId)
        {
            case SPELL_DK_GHOUL_CLAW_DISPLAY:
            case SPELL_DK_GHOUL_CLAW_EXECUTION:
                // 爪击 -> 横扫爪击（优先 1278150，缺失时回退旧版 91778）
                if (sSpellMgr->GetSpellInfo(
                    SPELL_DK_GHOUL_SWEEPING_CLAWS, difficulty))
                    return SPELL_DK_GHOUL_SWEEPING_CLAWS;
                if (sSpellMgr->GetSpellInfo(
                    SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY, difficulty))
                    return SPELL_DK_GHOUL_SWEEPING_CLAWS_LEGACY;
                break;
            case SPELL_DK_GHOUL_GNAW_DISPLAY:
            case SPELL_DK_GHOUL_GNAW_EXECUTION:
                // Gnaw -> 巨兽猛击
                if (sSpellMgr->GetSpellInfo(
                    SPELL_DK_GHOUL_MONSTROUS_BLOW, difficulty))
                    return SPELL_DK_GHOUL_MONSTROUS_BLOW;
                break;
            case SPELL_DK_GHOUL_LEAP_DISPLAY:
            case SPELL_DK_GHOUL_LEAP_EXECUTION:
                // Leap -> 蹒跚突袭
                if (sSpellMgr->GetSpellInfo(
                    SPELL_DK_GHOUL_SHAMBLING_RUSH, difficulty))
                    return SPELL_DK_GHOUL_SHAMBLING_RUSH;
                break;
            default:
                break;
        }

        return 0;
    }

    // 判断 spellId 是否为食尸鬼爪击（基础技能）
    bool IsDkGhoulClawSpell(uint32 spellId)
    {
        return spellId == SPELL_DK_GHOUL_CLAW_DISPLAY ||
            spellId == SPELL_DK_GHOUL_CLAW_EXECUTION;
    }

    // 获取基础技能的能量消耗。
    // 黑暗突变不会让爪击免费。横扫爪击保留原本的 40 点能量需求，
    // 即使存在法力消耗条目但报告为 0 也按 40 点处理。
    int32 GetDkGhoulBaseSpellEnergyCost(Spell const* baseSpell)
    {
        if (!baseSpell)
            return 0;

        if (IsDkGhoulClawSpell(baseSpell->GetSpellInfo()->Id))
            return 40;

        return std::max<int32>(
            0, baseSpell->GetPowerTypeCostAmount(POWER_ENERGY).value_or(0));
    }

    // 判断食尸鬼是否有足够能量施放基础技能
    bool HasDkGhoulBaseSpellPower(Unit const* caster, Spell const* baseSpell)
    {
        int32 const energyCost = GetDkGhoulBaseSpellEnergyCost(baseSpell);
        return !energyCost ||
            int32(caster->GetPower(POWER_ENERGY)) >= energyCost;
    }

    // 手动消耗基础技能的能量（黑暗突变的强化技能会绕过正常能量处理）
    void ConsumeDkGhoulBaseSpellPower(Unit* caster, Spell const* baseSpell)
    {
        int32 const energyCost = GetDkGhoulBaseSpellEnergyCost(baseSpell);
        if (energyCost)
            caster->ModifyPower(POWER_ENERGY, -energyCost);
    }

    // 为强化技能启动冷却：
    //   - 若强化技能本身有恢复时间，则正常 SendCooldownEvent
    //   - 否则强制启动 1 秒冷却避免连发
    //   - 同时为基础技能添加 GCD
    void StartDkGhoulOverrideCooldown(Unit* caster,
        SpellInfo const* baseSpellInfo, SpellInfo const* overrideSpellInfo)
    {
        SpellHistory* spellHistory = caster->GetSpellHistory();

        if (overrideSpellInfo->GetRecoveryTime() ||
            overrideSpellInfo->CategoryRecoveryTime)
        {
            spellHistory->SendCooldownEvent(
                overrideSpellInfo, 0, nullptr, true);
        }
        else
        {
            spellHistory->StartCooldown(
                overrideSpellInfo, 0, nullptr, false, Seconds(1));
            spellHistory->SendCooldownEvent(
                overrideSpellInfo, 0, nullptr, false);
        }

        if (baseSpellInfo->StartRecoveryTime)
            spellHistory->AddGlobalCooldown(
                baseSpellInfo, Milliseconds(baseSpellInfo->StartRecoveryTime));
    }
}

void WorldSession::HandleDismissCritter(WorldPackets::Pet::DismissCritter& packet)
{
    Unit* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, packet.CritterGUID);

    if (!pet)
    {
        TC_LOG_DEBUG("entities.pet", "Critter ({}) does not exist - player '{}' ({} / account: {}) attempted to dismiss it (possibly lagged out)",
            packet.CritterGUID.ToString(), GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString(), GetAccountId());
        return;
    }

    if (_player->GetCritterGUID() == pet->GetGUID())
    {
        if (pet->GetTypeId() == TYPEID_UNIT && pet->IsSummon())
        {
            if (!_player->GetSummonedBattlePetGUID().IsEmpty() && _player->GetSummonedBattlePetGUID() == pet->GetBattlePetCompanionGUID())
                _player->SetBattlePetData(nullptr);

            pet->ToTempSummon()->UnSummon();
        }
    }
}

void WorldSession::HandlePetAction(WorldPackets::Pet::PetAction& packet)
{
    if (_player->IsMounted())
        return;

    ObjectGuid guid1 = packet.PetGUID; //pet guid
    ObjectGuid guid2 = packet.TargetGUID; //tag guid

    uint32 spellid = UNIT_ACTION_BUTTON_ACTION(packet.Action);
    uint8 flag = UNIT_ACTION_BUTTON_TYPE(packet.Action); //delete = 0x07 CastSpell = C1

    // used also for charmed creature
    Unit* pet = ObjectAccessor::GetUnit(*_player, guid1);
    TC_LOG_DEBUG("entities.pet", "HandlePetAction: {} - flag: {}, spellid: {}, target: {}.", guid1.ToString(), uint32(flag), spellid, guid2.ToString());

    if (!pet)
    {
        TC_LOG_DEBUG("entities.pet", "HandlePetAction: {} doesn't exist for {} {}", guid1.ToString(), GetPlayer()->GetGUID().ToString(), GetPlayer()->GetName());
        return;
    }

    if (pet != GetPlayer()->GetFirstControlled())
    {
        TC_LOG_DEBUG("entities.pet", "HandlePetAction: {} does not belong to {} {}", guid1.ToString(), GetPlayer()->GetGUID().ToString(), GetPlayer()->GetName());
        return;
    }

    if (!pet->IsAlive())
    {
        SpellInfo const* spell = (flag == ACT_ENABLED || flag == ACT_PASSIVE) ? sSpellMgr->GetSpellInfo(spellid, pet->GetMap()->GetDifficultyID()) : nullptr;
        if (!spell)
            return;
        if (!spell->HasAttribute(SPELL_ATTR0_ALLOW_CAST_WHILE_DEAD))
            return;
    }

    /// @todo allow control charmed player?
    if (pet->GetTypeId() == TYPEID_PLAYER && !(flag == ACT_COMMAND && spellid == COMMAND_ATTACK))
        return;

    if (GetPlayer()->m_Controlled.size() == 1)
        HandlePetActionHelper(pet, guid1, spellid, flag, guid2, packet.ActionPosition);
    else
    {
        // If a pet is dismissed, m_Controlled will change
        std::vector<Unit*> controlled;
        for (Unit::ControlList::iterator itr = GetPlayer()->m_Controlled.begin(); itr != GetPlayer()->m_Controlled.end(); ++itr)
            if ((*itr)->GetEntry() == pet->GetEntry() && (*itr)->IsAlive())
                controlled.push_back(*itr);
        for (std::vector<Unit*>::iterator itr = controlled.begin(); itr != controlled.end(); ++itr)
            HandlePetActionHelper(*itr, guid1, spellid, flag, guid2, packet.ActionPosition);
    }
}

void WorldSession::HandlePetStopAttack(WorldPackets::Pet::PetStopAttack& packet)
{
    Unit* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, packet.PetGUID);

    if (!pet)
    {
        TC_LOG_ERROR("entities.pet", "HandlePetStopAttack: {} does not exist", packet.PetGUID.ToString());
        return;
    }

    if (pet != GetPlayer()->GetPet() && pet != GetPlayer()->GetCharmed())
    {
        TC_LOG_ERROR("entities.pet", "HandlePetStopAttack: {} isn't a pet or charmed creature of player {}",
            packet.PetGUID.ToString(), GetPlayer()->GetName());
        return;
    }

    if (!pet->IsAlive())
        return;

    pet->AttackStop();
}

void WorldSession::HandlePetActionHelper(Unit* pet, ObjectGuid guid1, uint32 spellid, uint16 flag, ObjectGuid guid2, Position const& pos)
{
    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        TC_LOG_DEBUG("entities.pet", "WorldSession::HandlePetAction(petGuid: {}, tagGuid: {}, spellId: {}, flag: {}): object {} is considered pet-like but doesn't have a charminfo!",
            guid1.ToString(), guid2.ToString(), spellid, flag, pet->GetGUID().ToString());
        return;
    }

    switch (flag)
    {
        case ACT_COMMAND: // 0x07
            switch (spellid)
            {
                case COMMAND_STAY: // flat = 1792 - STAY
                    pet->GetMotionMaster()->Clear(MOTION_PRIORITY_NORMAL);
                    pet->GetMotionMaster()->MoveIdle();

                    charmInfo->SetCommandState(COMMAND_STAY);
                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsAtStay(true);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsReturning(false);
                    charmInfo->SaveStayPosition();
                    break;
                case COMMAND_FOLLOW: // spellid = 1792 - FOLLOW
                    if (IsRisenGhoul(pet))
                    {
                        // 跟随只是一个宠物命令，必须停止食尸鬼当前的攻击动作并把它
                        // 召回，但不应强制调用 CombatStop。若在此处结束战斗，目标
                        // 可能立刻脱战，而零售端会保持战斗状态直到正常的距离/leash
                        // 规则使其结束。
                        charmInfo->SetIsCommandAttack(false);
                        pet->AttackStop();
                    }
                    else
                        pet->AttackStop();

                    pet->InterruptSpell(CURRENT_GENERIC_SPELL, false, false);
                    if (Spell const* channeledSpell = pet->GetCurrentSpell(CURRENT_CHANNELED_SPELL); channeledSpell && !channeledSpell->GetSpellInfo()->HasAttribute(SPELL_ATTR9_CHANNEL_PERSISTS_ON_PET_FOLLOW))
                        pet->InterruptSpell(CURRENT_CHANNELED_SPELL, true, true);
                    pet->GetMotionMaster()->MoveFollow(_player, PET_FOLLOW_DIST, pet->GetFollowAngle());

                    charmInfo->SetCommandState(COMMAND_FOLLOW);
                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsReturning(true);
                    charmInfo->SetIsCommandFollow(true);
                    charmInfo->SetIsFollowing(false);
                    break;
                case COMMAND_ATTACK: // spellid = 1792 - ATTACK
                {
                    // Can't attack if owner is pacified
                    if (_player->HasAuraType(SPELL_AURA_MOD_PACIFY))
                    {
                        // pet->SendPetCastFail(spellid, SPELL_FAILED_PACIFIED);
                        /// @todo Send proper error message to client
                        return;
                    }

                    // only place where pet can be player
                    Unit* TargetUnit = ObjectAccessor::GetUnit(*_player, guid2);
                    if (!TargetUnit)
                        return;

                    if (Unit* owner = pet->GetOwner())
                        if (!owner->IsValidAttackTarget(TargetUnit))
                            return;

                    // This is true if pet has no target or has target but targets differs.
                    if (pet->GetVictim() != TargetUnit || !pet->GetCharmInfo()->IsCommandAttack())
                    {
                        if (pet->GetVictim())
                            pet->AttackStop();

                        if (pet->GetTypeId() != TYPEID_PLAYER && pet->ToCreature()->IsAIEnabled())
                        {
                            // 手动宠物条 Attack 必须覆盖之前的守卫点命令（如 Move To 或 Stay）。
                            // 若 COMMAND_MOVE_TO 仍处于激活状态，PetAI 可能拒绝攻击，
                            // 导致食尸鬼卡在守卫点无法攻击。
                            charmInfo->SetCommandState(COMMAND_ATTACK);
                            charmInfo->SetIsCommandAttack(true);
                            charmInfo->SetIsAtStay(false);
                            charmInfo->SetIsFollowing(false);
                            charmInfo->SetIsCommandFollow(false);
                            charmInfo->SetIsReturning(false);

                            CreatureAI* AI = pet->ToCreature()->AI();
                            if (PetAI* petAI = dynamic_cast<PetAI*>(AI))
                                petAI->_AttackStart(TargetUnit); // force target switch
                            else
                                AI->AttackStart(TargetUnit);

                            // 10% chance to play special pet attack talk, else growl
                            if (pet->IsPet() && ((Pet*)pet)->getPetType() == SUMMON_PET && pet != TargetUnit && urand(0, 100) < 10)
                                pet->SendPetTalk((uint32)PET_TALK_ATTACK);
                            else
                            {
                                // 90% chance for pet and 100% chance for charmed creature
                                pet->SendPetAIReaction(guid1);
                            }
                        }
                        else // charmed player
                        {
                            // 同样为被控制/被附身的玩家宠物覆盖之前的守卫点命令。
                            charmInfo->SetCommandState(COMMAND_ATTACK);
                            charmInfo->SetIsCommandAttack(true);
                            charmInfo->SetIsAtStay(false);
                            charmInfo->SetIsFollowing(false);
                            charmInfo->SetIsCommandFollow(false);
                            charmInfo->SetIsReturning(false);

                            pet->Attack(TargetUnit, true);
                            pet->SendPetAIReaction(guid1);
                        }
                    }
                    break;
                }
                case COMMAND_ABANDON: // abandon (hunter pet) or dismiss (summoned pet)
                    if (pet->GetCharmerGUID() == GetPlayer()->GetGUID())
                        _player->StopCastingCharm();
                    else if (pet->GetOwnerGUID() == GetPlayer()->GetGUID())
                    {
                        ASSERT(pet->GetTypeId() == TYPEID_UNIT);
                        if (pet->IsPet())
                        {
                            if (((Pet*)pet)->getPetType() == HUNTER_PET)
                                GetPlayer()->RemovePet((Pet*)pet, PET_SAVE_AS_DELETED);
                            else
                                GetPlayer()->RemovePet((Pet*)pet, PET_SAVE_NOT_IN_SLOT);
                        }
                        else if (pet->HasUnitTypeMask(UNIT_MASK_MINION))
                        {
                            ((Minion*)pet)->UnSummon();
                        }
                    }
                    break;
                case COMMAND_MOVE_TO:
                    pet->StopMoving();
                    pet->GetMotionMaster()->Clear();

                    // Move To 是一个守卫点命令。使用宠物 GUID 作为点 id，
                    // 这样 PetAI::MovementInform 可以标记到达事件并保存实际
                    // 到达的位置。若在此处保存，会存储宠物旧位置而非点击点。
                    pet->GetMotionMaster()->MovePoint(pet->GetGUID().GetCounter(), pos);
                    charmInfo->SetCommandState(COMMAND_MOVE_TO);

                    charmInfo->SetIsCommandAttack(false);
                    charmInfo->SetIsAtStay(false);
                    charmInfo->SetIsCommandFollow(false);
                    charmInfo->SetIsFollowing(false);
                    charmInfo->SetIsReturning(true);
                    break;
                default:
                    TC_LOG_ERROR("entities.pet", "WORLD: unknown PET flag Action {} and spellid {}.", uint32(flag), spellid);
            }
            break;
        case ACT_REACTION: // 0x6
            switch (spellid)
            {
                case REACT_PASSIVE: // passive
                    if (IsRisenGhoul(pet))
                    {
                        // 被动只取消食尸鬼的攻击意图。此处不应调用 CombatStop：
                        // 若食尸鬼或其主人仍在怪物的威胁/战斗列表中，怪物应保持
                        // 战斗状态直到正常的距离/leash 规则使其结束。
                        charmInfo->SetIsCommandAttack(false);
                        pet->AttackStop();
                    }
                    else
                        pet->AttackStop();
                    [[fallthrough]];
                case REACT_DEFENSIVE: // recovery
                case REACT_AGGRESSIVE: // activete
                case REACT_ASSIST:     // assist
                    if (pet->GetTypeId() == TYPEID_UNIT)
                    {
                        pet->ToCreature()->SetReactState(ReactStates(spellid));
                        // 官服：玩家在食尸鬼召回状态下切换协助/防御/攻击反应时，
                        // 召回命令被打破，食尸鬼重新协助玩家。
                        if (IsRisenGhoul(pet))
                        {
                            charmInfo->SetIsCommandFollow(false);
                            charmInfo->SetIsReturning(false);
                            charmInfo->SetIsFollowing(true);
                            charmInfo->SetCommandState(COMMAND_FOLLOW);
                        }
                    }
                    break;
            }
            break;
        case ACT_DISABLED: // 0x81 spell (disabled), ignore
        case ACT_PASSIVE: // 0x01
        case ACT_ENABLED: // 0xC1 spell
        {
            Unit* unit_target = nullptr;

            if (!guid2.IsEmpty())
                unit_target = ObjectAccessor::GetUnit(*_player, guid2);

            // do not cast unknown spells
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid, pet->GetMap()->GetDifficultyID());
            if (!spellInfo)
            {
                TC_LOG_ERROR("spells.pet", "WORLD: unknown PET spell id {}", spellid);
                return;
            }

            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            {
                if (spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY || spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY || spellEffectInfo.TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                    return;
            }

            // do not cast not learned spells
            if (!pet->HasSpell(spellid) || spellInfo->IsPassive())
                return;

            // Clear the flags as if owner clicked 'attack'. AI will reset them
            // after AttackStart, even if spell failed
            if (pet->GetCharmInfo())
            {
                pet->GetCharmInfo()->SetIsAtStay(false);
                pet->GetCharmInfo()->SetIsCommandAttack(true);
                pet->GetCharmInfo()->SetIsReturning(false);
                pet->GetCharmInfo()->SetIsFollowing(false);
            }

            Spell* spell = new Spell(pet, spellInfo, TRIGGERED_NONE);

            SpellCastResult result = spell->CheckPetCast(unit_target);

            // auto turn to target unless possessed
            if (result == SPELL_FAILED_UNIT_NOT_INFRONT && !pet->isPossessed() && !pet->IsVehicle())
            {
                if (unit_target)
                {
                    if (!pet->HasSpellFocus())
                        pet->SetInFront(unit_target);
                    if (Player* player = unit_target->ToPlayer())
                        pet->SendUpdateToPlayer(player);
                }
                else if (Unit* unit_target2 = spell->m_targets.GetUnitTarget())
                {
                    if (!pet->HasSpellFocus())
                        pet->SetInFront(unit_target2);
                    if (Player* player = unit_target2->ToPlayer())
                        pet->SendUpdateToPlayer(player);
                }

                if (Unit* powner = pet->GetCharmerOrOwner())
                    if (Player* player = powner->ToPlayer())
                        pet->SendUpdateToPlayer(player);

                result = SPELL_CAST_OK;
            }

            if (result == SPELL_CAST_OK)
            {
                unit_target = spell->m_targets.GetUnitTarget();

                // 10% chance to play special pet attack talk, else growl
                // actually this only seems to happen on special spells, fire shield for imp, torment for voidwalker, but it's stupid to check every spell
                if (pet->IsPet() && (((Pet*)pet)->getPetType() == SUMMON_PET) && (pet != unit_target) && (urand(0, 100) < 10))
                    pet->SendPetTalk((uint32)PET_TALK_SPECIAL_SPELL);
                else
                {
                    pet->SendPetAIReaction(guid1);
                }

                if (unit_target && !GetPlayer()->IsFriendlyTo(unit_target) && !pet->isPossessed() && !pet->IsVehicle())
                {
                    // This is true if pet has no target or has target but targets differs.
                    if (pet->GetVictim() != unit_target)
                    {
                        if (CreatureAI* AI = pet->ToCreature()->AI())
                        {
                            if (PetAI* petAI = dynamic_cast<PetAI*>(AI))
                                petAI->_AttackStart(unit_target); // force victim switch
                            else
                                AI->AttackStart(unit_target);
                        }
                    }
                }

                spell->prepare(spell->m_targets);
            }
            else
            {
                if (pet->isPossessed() || pet->IsVehicle()) /// @todo: confirm this check
                    Spell::SendCastResult(GetPlayer(), spellInfo, spell->m_SpellVisual, spell->m_castId, result);
                else
                    spell->SendPetCastResult(result);

                if (!pet->GetSpellHistory()->HasCooldown(spellid))
                    pet->GetSpellHistory()->ResetCooldown(spellid, true);

                spell->finish(result);
                delete spell;

                // reset specific flags in case of spell fail. AI will reset other flags
                if (pet->GetCharmInfo())
                    pet->GetCharmInfo()->SetIsCommandAttack(false);
            }
            break;
        }
        default:
            TC_LOG_ERROR("entities.pet", "WORLD: unknown PET flag Action {} and spellid {}.", uint32(flag), spellid);
    }
}

void WorldSession::HandleQueryPetName(WorldPackets::Query::QueryPetName& packet)
{
    SendQueryPetNameResponse(packet.UnitGUID);
}

void WorldSession::SendQueryPetNameResponse(ObjectGuid guid)
{
    WorldPackets::Query::QueryPetNameResponse response;

    response.UnitGUID = guid;

    if (Creature* unit = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, guid))
    {
        response.Allow = true;
        response.Timestamp = *unit->m_unitData->PetNameTimestamp;
        response.Name = unit->GetName();

        if (Pet* pet = unit->ToPet())
        {
            if (DeclinedName const* names = pet->GetDeclinedNames())
            {
                response.HasDeclined = true;
                response.DeclinedNames = *names;
            }
        }
    }

    _player->SendDirectMessage(response.Write());
}

bool WorldSession::CheckStableMaster(ObjectGuid guid)
{
    // spell case or GM
    if (guid == GetPlayer()->GetGUID())
    {
        if (!GetPlayer()->IsGameMaster() && !GetPlayer()->HasAuraType(SPELL_AURA_OPEN_STABLE))
        {
            TC_LOG_DEBUG("entities.player.cheat", "{} attempt open stable in cheating way.", guid.ToString());
            return false;
        }
    }
    // stable master case
    else
    {
        if (!GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_STABLEMASTER, UNIT_NPC_FLAG_2_NONE))
        {
            TC_LOG_DEBUG("entities.player", "Stablemaster {} not found or you can't interact with him.", guid.ToString());
            return false;
        }
    }
    return true;
}

void WorldSession::HandlePetSetAction(WorldPackets::Pet::PetSetAction& packet)
{
    ObjectGuid petguid = packet.PetGUID;
    Unit* pet = ObjectAccessor::GetUnit(*_player, petguid);

    if (!pet || pet != _player->GetFirstControlled())
    {
        TC_LOG_ERROR("entities.pet", "HandlePetSetAction: Unknown {} or owner ({})", petguid.ToString(), _player->GetGUID().ToString());
        return;
    }

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (!charmInfo)
    {
        TC_LOG_ERROR("entities.pet", "WorldSession::HandlePetSetAction: object {} is considered pet-like but doesn't have a charminfo!", pet->GetGUID().ToString());
        return;
    }

    std::vector<Unit*> pets;
    for (Unit* controlled : _player->m_Controlled)
        if (controlled->GetEntry() == pet->GetEntry() && controlled->IsAlive())
            pets.push_back(controlled);

    uint32 position = packet.Index;
    uint32 actionData = packet.Action;

    uint32 spell_id = UNIT_ACTION_BUTTON_ACTION(actionData);
    uint8 act_state = UNIT_ACTION_BUTTON_TYPE(actionData);

    TC_LOG_DEBUG("entities.pet", "Player {} has changed pet spell action. Position: {}, Spell: {}, State: 0x{:X}",
        _player->GetName(), position, spell_id, uint32(act_state));

    for (Unit* petControlled : pets)
    {
        ActiveStates finalActState = ActiveStates(act_state);
        uint32 actionBarSpellId = spell_id;
        uint32 autocastSpellId = spell_id;

        if (IsRisenGhoul(petControlled))
        {
            // DK 食尸鬼：宠物栏存储 9xxxx 执行技能 ID
            actionBarSpellId = GetDkGhoulActionBarSpellId(spell_id);

            // 自动释放使用对应的执行技能 ID
            if (uint32 ghoulAutocastSpellId = GetDkGhoulAutocastExecutionSpellId(spell_id))
                autocastSpellId = ghoulAutocastSpellId;
            else
                autocastSpellId = actionBarSpellId;

            // 蜷缩是手动技能，不允许自动释放，强制保持 ACT_PASSIVE
            if (IsDkGhoulHuddleSpell(actionBarSpellId) && (act_state == ACT_ENABLED || act_state == ACT_DISABLED))
                finalActState = ACT_PASSIVE;
        }

        // 如果是技能动作（开启/关闭/被动）且有技能 ID 但宠物未学习该技能，则不添加
        uint32 knownSpellId = IsRisenGhoul(petControlled) ? actionBarSpellId : spell_id;
        if (!((act_state == ACT_ENABLED || act_state == ACT_DISABLED || act_state == ACT_PASSIVE) && spell_id && !petControlled->HasSpell(knownSpellId)))
        {
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(autocastSpellId, petControlled->GetMap()->GetDifficultyID()))
            {
                // 开启自动释放
                if (finalActState == ACT_ENABLED)
                {
                    if (petControlled->GetTypeId() == TYPEID_UNIT && petControlled->IsPet())
                        ((Pet*)petControlled)->ToggleAutocast(spellInfo, true);
                    else
                        for (Unit::ControlList::iterator itr = GetPlayer()->m_Controlled.begin(); itr != GetPlayer()->m_Controlled.end(); ++itr)
                            if ((*itr)->GetEntry() == petControlled->GetEntry())
                                (*itr)->GetCharmInfo()->ToggleCreatureAutocast(spellInfo, true);
                }
                // 关闭自动释放
                else if (finalActState == ACT_DISABLED)
                {
                    if (petControlled->GetTypeId() == TYPEID_UNIT && petControlled->IsPet())
                        ((Pet*)petControlled)->ToggleAutocast(spellInfo, false);
                    else
                        for (Unit::ControlList::iterator itr = GetPlayer()->m_Controlled.begin(); itr != GetPlayer()->m_Controlled.end(); ++itr)
                            if ((*itr)->GetEntry() == petControlled->GetEntry())
                                (*itr)->GetCharmInfo()->ToggleCreatureAutocast(spellInfo, false);
                }
            }

            charmInfo->SetActionBar(position, actionBarSpellId, finalActState);

            // 立即保存永久食尸鬼的动作栏修改。
            // 不保存的话，替换 Move To 为 Stay 或移动食尸鬼技能等操作可能在重新登录/召唤时丢失。
            if (Pet* controlledPet = petControlled->ToPet())
                if (controlledPet->IsPermanentPetFor(_player))
                    controlledPet->SavePetToDB(PET_SAVE_AS_CURRENT);
        }
    }
}

void WorldSession::HandlePetRename(WorldPackets::Pet::PetRename& packet)
{
    ObjectGuid petguid = packet.RenameData.PetGUID;

    std::string name = packet.RenameData.NewName;
    Optional<DeclinedName> const& declinedname = packet.RenameData.DeclinedNames;

    PetStable* petStable = _player->GetPetStable();
    Pet* pet = ObjectAccessor::GetPet(*_player, petguid);
                                                            // check it!
    if (!pet || !pet->IsPet() || ((Pet*)pet)->getPetType() != HUNTER_PET ||
        !pet->HasPetFlag(UNIT_PET_FLAG_CAN_BE_RENAMED) ||
        pet->GetOwnerGUID() != _player->GetGUID() || !pet->GetCharmInfo() ||
        !petStable || !petStable->GetCurrentPet() || petStable->GetCurrentPet()->PetNumber != pet->GetCharmInfo()->GetPetNumber())
        return;

    PetNameInvalidReason res = ObjectMgr::CheckPetName(name);
    if (res != PET_NAME_SUCCESS)
    {
        SendPetNameInvalid(res, name, {});
        return;
    }

    if (sObjectMgr->IsReservedName(name))
    {
        SendPetNameInvalid(PET_NAME_RESERVED, name, {});
        return;
    }

    pet->SetName(name);

    pet->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_NAME);

    pet->RemovePetFlag(UNIT_PET_FLAG_CAN_BE_RENAMED);

    petStable->GetCurrentPet()->Name = name;
    petStable->GetCurrentPet()->WasRenamed = true;

    if (declinedname)
    {
        std::wstring wname;
        if (!Utf8toWStr(name, wname))
            return;

        if (!ObjectMgr::CheckDeclinedNames(wname, *declinedname))
        {
            SendPetNameInvalid(PET_NAME_DECLENSION_DOESNT_MATCH_BASE_NAME, name, declinedname);
            return;
        }
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (declinedname)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_PET_DECLINEDNAME);
        stmt->setUInt32(0, pet->GetCharmInfo()->GetPetNumber());
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_PET_DECLINEDNAME);
        stmt->setUInt32(0, pet->GetCharmInfo()->GetPetNumber());
        stmt->setUInt64(1, _player->GetGUID().GetCounter());

        for (uint8 i = 0; i < 5; i++)
            stmt->setString(i + 2, declinedname->name[i]);

        trans->Append(stmt);
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_PET_NAME);
    stmt->setString(0, name);
    stmt->setUInt64(1, _player->GetGUID().GetCounter());
    stmt->setUInt32(2, pet->GetCharmInfo()->GetPetNumber());
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);

    pet->SetPetNameTimestamp(uint32(GameTime::GetGameTime()));
}

void WorldSession::HandlePetAbandon(WorldPackets::Pet::PetAbandon& packet)
{
    // pet/charmed
    Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, packet.Pet);
    if (pet && pet->ToPet() && pet->ToPet()->getPetType() == HUNTER_PET)
    {
        _player->RemovePet((Pet*)pet, PET_SAVE_AS_DELETED);
    }
}

void WorldSession::HandlePetAbandonByNumber(WorldPackets::Pet::PetAbandonByNumber const& petAbandonByNumber)
{
    if (Pet* pet = _player->GetPet())
    {
        if (pet->IsHunterPet() && pet->m_unitData->PetNumber == petAbandonByNumber.PetNumber)
            _player->RemovePet(pet, PET_SAVE_AS_DELETED);
    }
    else
    {
        _player->DeletePetFromDB(petAbandonByNumber.PetNumber);
    }
}

void WorldSession::HandlePetSpellAutocastOpcode(WorldPackets::Pet::PetSpellAutocast& packet)
{
    Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, packet.PetGUID);
    if (!pet)
    {
        TC_LOG_ERROR("entities.pet", "WorldSession::HandlePetSpellAutocastOpcode: Pet {} not found.", packet.PetGUID.ToString());
        return;
    }

    if (pet != _player->GetGuardianPet() && pet != _player->GetCharmed())
    {
        TC_LOG_ERROR("entities.pet", "WorldSession::HandlePetSpellAutocastOpcode: {} isn't pet of player {} ({}).",
            packet.PetGUID.ToString(), GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(packet.SpellID, pet->GetMap()->GetDifficultyID());
    if (!spellInfo)
    {
        TC_LOG_ERROR("spells.pet", "WorldSession::HandlePetSpellAutocastOpcode: Unknown spell id {} used by {}.", packet.SpellID, packet.PetGUID.ToString());
        return;
    }

    std::vector<Unit*> pets;
    for (Unit* controlled : _player->m_Controlled)
        if (controlled->GetEntry() == pet->GetEntry() && controlled->IsAlive())
            pets.push_back(controlled);

    for (Unit* petControlled : pets)
    {
        uint32 autocastSpellId = packet.SpellID;
        uint32 actionBarSpellId = packet.SpellID;
        bool ghoulAutocastAlias = false;

        if (IsRisenGhoul(petControlled))
        {
            if (uint32 ghoulAutocastSpellId = GetDkGhoulAutocastExecutionSpellId(packet.SpellID))
            {
                // 自动释放使用服务端执行技能 ID
                autocastSpellId = ghoulAutocastSpellId;
                actionBarSpellId = GetDkGhoulActionBarSpellId(packet.SpellID);
                ghoulAutocastAlias = true;
            }
            else if (IsDkGhoulHuddleSpell(packet.SpellID))
                return; // 蜷缩是纯手动技能，不允许自动释放
        }

        SpellInfo const* autocastSpellInfo = autocastSpellId == spellInfo->Id
            ? spellInfo
            : sSpellMgr->GetSpellInfo(autocastSpellId, petControlled->GetMap()->GetDifficultyID());

        SpellInfo const* actionBarSpellInfo = actionBarSpellId == spellInfo->Id
            ? spellInfo
            : sSpellMgr->GetSpellInfo(actionBarSpellId, petControlled->GetMap()->GetDifficultyID());

        // 不添加未学习的技能/被动技能
        if (!autocastSpellInfo || !actionBarSpellInfo ||
            (!petControlled->HasSpell(packet.SpellID) && !petControlled->HasSpell(autocastSpellId)) ||
            (!ghoulAutocastAlias && !spellInfo->IsAutocastable()))
            return;

        CharmInfo* charmInfo = petControlled->GetCharmInfo();
        if (!charmInfo)
        {
            TC_LOG_ERROR("entities.pet", "WorldSession::HandlePetSpellAutocastOpcode: object {} is considered pet-like but doesn't have a charminfo!", petControlled->GetGUID().ToString());
            return;
        }

        if (petControlled->IsPet())
            petControlled->ToPet()->ToggleAutocast(autocastSpellInfo, packet.AutocastEnabled);
        else
            charmInfo->ToggleCreatureAutocast(autocastSpellInfo, packet.AutocastEnabled);

        charmInfo->SetSpellAutocast(actionBarSpellInfo, packet.AutocastEnabled);
    }
}

void WorldSession::HandlePetCastSpellOpcode(WorldPackets::Spells::PetCastSpell& petCastSpell)
{
    Unit* caster = ObjectAccessor::GetUnit(*_player, petCastSpell.PetGUID);
    if (!caster)
    {
        TC_LOG_ERROR("entities.pet", "WorldSession::HandlePetCastSpellOpcode: Caster {} not found.", petCastSpell.PetGUID.ToString());
        return;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(petCastSpell.Cast.SpellID, caster->GetMap()->GetDifficultyID());
    if (!spellInfo)
    {
        TC_LOG_ERROR("spells.pet", "WorldSession::HandlePetCastSpellOpcode: unknown spell id {} tried to cast by {}",
            petCastSpell.Cast.SpellID, petCastSpell.PetGUID.ToString());
        return;
    }

    // This opcode is also sent from charmed and possessed units (players and creatures)
    if (caster != _player->GetGuardianPet() && caster != _player->GetCharmed())
    {
        TC_LOG_ERROR("spells.pet", "WorldSession::HandlePetCastSpellOpcode: {} isn't pet of player {} ({}).", petCastSpell.PetGUID.ToString(), GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    SpellCastTargets targets(caster, petCastSpell.Cast);

    TriggerCastFlags triggerCastFlags = TRIGGERED_NONE;

    // DK 食尸鬼技能 ID 映射：客户端发送的 474xx 显示 ID 映射为 9xxxx 执行 ID
    if (IsRisenGhoul(caster))
    {
        uint32 const executionSpellId = GetDkGhoulExecutionSpellId(spellInfo->Id);
        if (executionSpellId != spellInfo->Id)
            if (SpellInfo const* executionSpellInfo = sSpellMgr->GetSpellInfo(executionSpellId, caster->GetMap()->GetDifficultyID()))
                spellInfo = executionSpellInfo;
    }

    if (spellInfo->IsPassive())
        return;

    // 仅施放已学习的技能
    if (!caster->HasSpell(spellInfo->Id))
    {
        bool allow = false;

        // DK 食尸鬼特殊处理：执行技能 ID 通过对应的显示技能 ID 来判断是否已学习
        if (IsRisenGhoul(caster))
        {
            switch (spellInfo->Id)
            {
                case SPELL_DK_GHOUL_CLAW_EXECUTION:
                    allow = caster->HasSpell(SPELL_DK_GHOUL_CLAW_DISPLAY);
                    break;
                case SPELL_DK_GHOUL_GNAW_EXECUTION:
                    allow = caster->HasSpell(SPELL_DK_GHOUL_GNAW_DISPLAY);
                    break;
                case SPELL_DK_GHOUL_LEAP_EXECUTION:
                    allow = caster->HasSpell(SPELL_DK_GHOUL_LEAP_DISPLAY);
                    break;
                case SPELL_DK_GHOUL_HUDDLE_EXECUTION:
                    allow = caster->HasSpell(SPELL_DK_GHOUL_HUDDLE_DISPLAY);
                    break;
                default:
                    break;
            }
        }

        // 允许施放由客户端周期性触发光环触发的技能
        if (!allow && caster->HasAuraTypeWithTriggerSpell(SPELL_AURA_PERIODIC_TRIGGER_SPELL_FROM_CLIENT, spellInfo->Id))
        {
            allow = true;
            triggerCastFlags = TRIGGERED_FULL_MASK;
        }

        if (!allow)
            return;
    }

    if (petCastSpell.Cast.MoveUpdate)
        HandleMovementOpcode(CMSG_MOVE_STOP, *petCastSpell.Cast.MoveUpdate);

    Spell* spell = new Spell(caster, spellInfo, triggerCastFlags);
    spell->m_fromClient = true;
    std::ranges::copy(petCastSpell.Cast.Misc, std::ranges::begin(spell->m_misc.Raw.Data));
    spell->m_targets = targets;

    SpellCastResult result = spell->CheckPetCast(nullptr);

    if (result == SPELL_CAST_OK)
    {
        // DK 食尸鬼黑暗突变手动技能替换：
        // 玩家点击 474xx 基础技能图标时，若食尸鬼处于黑暗突变状态，
        // 则将其替换为对应的强化技能（横扫爪击/巨兽猛击/蹒跚突袭）。
        // 强化技能检查成功后执行，成功后不再执行基础技能，
        // 使用原客户端 CastID 回包，手动处理能量和冷却。
        uint32 const overrideSpellId =
            GetDkGhoulDarkTransformationOverrideSpellId(
                caster, spellInfo->Id);

        if (overrideSpellId)
        {
            SpellInfo const* overrideSpellInfo = sSpellMgr->GetSpellInfo(
                overrideSpellId, caster->GetMap()->GetDifficultyID());

            // 强化技能冷却未就绪时直接失败
            if (overrideSpellInfo &&
                !caster->GetSpellHistory()->IsReady(overrideSpellInfo))
            {
                spell->SendPetCastResult(SPELL_FAILED_NOT_READY);
                spell->finish(SPELL_FAILED_NOT_READY);
                delete spell;
                return;
            }

            // 触发的变身强化技能会绕过正常的能量处理逻辑。
            // 这里显式保留基础技能的能量需求（爪击/横扫爪击需 40 点能量）。
            if (overrideSpellInfo &&
                !HasDkGhoulBaseSpellPower(caster, spell))
            {
                spell->SendPetCastResult(SPELL_FAILED_NO_POWER);
                spell->finish(SPELL_FAILED_NO_POWER);
                delete spell;
                return;
            }

            if (overrideSpellInfo)
            {
                Unit* unitTarget = spell->m_targets.GetUnitTarget();
                if (unitTarget)
                {
                    Spell* overrideSpell = new Spell(
                        caster, overrideSpellInfo, TRIGGERED_FULL_MASK);
                    overrideSpell->m_fromClient = true;
                    std::ranges::copy(
                        petCastSpell.Cast.Misc,
                        std::ranges::begin(overrideSpell->m_misc.Raw.Data));
                    overrideSpell->InitExplicitTargets(targets);

                    SpellCastResult const overrideResult =
                        overrideSpell->CheckPetCast(unitTarget);

                    if (overrideResult == SPELL_CAST_OK)
                    {
                        // 强化技能施放成功，发送宠物动作音效
                        if (Creature* creature = caster->ToCreature())
                        {
                            if (Pet* pet = creature->ToPet())
                            {
                                if (pet->getPetType() == SUMMON_PET &&
                                    (urand(0, 100) < 10))
                                {
                                    pet->SendPetTalk(
                                        PET_TALK_SPECIAL_SPELL);
                                }
                                else
                                    pet->SendPetAIReaction(
                                        petCastSpell.PetGUID);
                            }
                        }

                        // 使用原客户端 CastID 回包，让客户端正确显示施法条
                        WorldPackets::Spells::SpellPrepare spellPrepare;
                        spellPrepare.ClientCastID =
                            petCastSpell.Cast.CastID;
                        spellPrepare.ServerCastID =
                            overrideSpell->m_castId;
                        SendPacket(spellPrepare.Write());

                        overrideSpell->prepare(targets);

                        // 手动消耗基础技能能量并启动强化技能冷却
                        ConsumeDkGhoulBaseSpellPower(caster, spell);
                        StartDkGhoulOverrideCooldown(
                            caster, spellInfo, overrideSpellInfo);

                        // 基础技能不再执行
                        spell->finish(SPELL_CAST_OK);
                        delete spell;
                        return;
                    }

                    delete overrideSpell;
                    // 若强化技能由于数据或目标差异无法施放，
                    // 安全地继续执行基础技能。
                }
            }
        }

        if (Creature* creature = caster->ToCreature())
        {
            if (Pet* pet = creature->ToPet())
            {
                // 10% chance to play special pet attack talk, else growl
                // actually this only seems to happen on special spells, fire shield for imp, torment for voidwalker, but it's stupid to check every spell
                if (pet->getPetType() == SUMMON_PET && (urand(0, 100) < 10))
                    pet->SendPetTalk(PET_TALK_SPECIAL_SPELL);
                else
                    pet->SendPetAIReaction(petCastSpell.PetGUID);
            }
        }

        WorldPackets::Spells::SpellPrepare spellPrepare;
        spellPrepare.ClientCastID = petCastSpell.Cast.CastID;
        spellPrepare.ServerCastID = spell->m_castId;
        SendPacket(spellPrepare.Write());

        spell->prepare(targets);
    }
    else
    {
        spell->SendPetCastResult(result);

        if (!caster->GetSpellHistory()->HasCooldown(spellInfo))
            caster->GetSpellHistory()->ResetCooldown(spellInfo->Id, true);

        spell->finish(result);
        delete spell;
    }
}

void WorldSession::SendPetNameInvalid(uint32 error, const std::string& name, Optional<DeclinedName> const& declinedName)
{
    WorldPackets::Pet::PetNameInvalid petNameInvalid;
    petNameInvalid.Result = error;
    petNameInvalid.RenameData.NewName = name;
    petNameInvalid.RenameData.DeclinedNames = declinedName;

    SendPacket(petNameInvalid.Write());
}

void WorldSession::HandleRequestPetInfo(WorldPackets::Pet::RequestPetInfo& /*requestPetInfo*/)
{
    // Handle the packet CMSG_REQUEST_PET_INFO - sent when player does ingame /reload command

    // Packet sent when player has a pet
    if (_player->GetPet())
        _player->PetSpellInitialize();
    else if (Unit* charm = _player->GetCharmed())
    {
        // Packet sent when player has a possessed unit
        if (charm->HasUnitState(UNIT_STATE_POSSESSED))
            _player->PossessSpellInitialize();
        // Packet sent when player controlling a vehicle
        else if (charm->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED) && charm->HasUnitFlag(UNIT_FLAG_POSSESSED))
            _player->VehicleSpellInitialize();
        // Packet sent when player has a charmed unit
        else
            _player->CharmSpellInitialize();
    }
}
