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

#ifndef TRINITY_PETAI_H
#define TRINITY_PETAI_H

#include "CreatureAI.h"
#include "ObjectGuid.h"
#include "Timer.h"

class Creature;
class Spell;

typedef std::vector<std::pair<Unit*, Spell*>> TargetSpellList;

class TC_GAME_API PetAI : public CreatureAI
{
    public:
        static int32 Permissible(Creature const* creature);

        explicit PetAI(Creature* creature, uint32 scriptId = {});

        void UpdateAI(uint32) override;
        void KilledUnit(Unit* /*victim*/) override;
        // only start attacking if not attacking something else already
        void AttackStart(Unit* target) override;
        // always start attacking if possible
        void _AttackStart(Unit* target);
        void MovementInform(uint32 type, uint32 id) override;
        void OwnerAttackedBy(Unit* attacker) override;
        void OwnerAttacked(Unit* target) override;

        // 仅在玩家主人物本人造成真实伤害时由 Unit::DealDamage 调用。
        // 食尸鬼伤害记为主人威胁值时不会调用本方法。
        void OwnerDealtDamage(Unit* target);

        void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damageType, SpellInfo const* spellInfo = nullptr) override;
        void ReceiveEmote(Player* player, uint32 textEmote) override;
        void JustEnteredCombat(Unit* who) override;
        void JustExitedCombat() override;
        void OnDespawn() override;
        void OnCharmed(bool isNew) override;

        // The following aren't used by the PetAI but need to be defined to override
        // default CreatureAI functions which interfere with the PetAI

        void MoveInLineOfSight(Unit* /*who*/) override { } // CreatureAI interferes with returning pets
        void MoveInLineOfSight_Safe(Unit* /*who*/) { } // CreatureAI interferes with returning pets
        void JustAppeared() override { } // we will control following manually
        void EnterEvadeMode(EvadeReason /*why*/) override { } // For fleeing, pets don't use this type of Evade mechanic

    private:
        bool NeedToStop();
        void StopAttack();
        void UpdateAllies();
        Unit* SelectNextTarget(bool allowAutoSelect) const;
        void HandleReturnMovement();
        void DoAttack(Unit* target, bool chase);
        bool CanAttack(Unit* target);
        // Quick access to set all flags to FALSE
        void ClearCharmInfoFlags();

        // 永久 DK 食尸鬼专用。修正 12.x 黑暗突变的模型，
        // 不修改食尸鬼的 NativeDisplayID。
        void UpdateDkGhoulDarkTransformationModel();

        // 在黑暗突变期间将 利爪/撕咬/跳跃 替换为对应的变身执行技能，
        // 同时保留宠物栏基础技能、能量消耗、公共冷却和回退逻辑。
        uint8 TryDkGhoulDarkTransformationOverride(
            Spell* baseSpell, Unit* target);

        // 永久 DK 食尸鬼专用。显式命令控制移动；
        // 真实 CombatReference 决定主人战斗何时开始与结束。
        void TryDkGhoulAssistOwnerAfterRealAttack();
        void StartDkGhoulManualOwnerCombat(Unit* target);
        void UpdateDkGhoulManualOwnerCombat();
        void ClearDkGhoulManualOwnerCombat();

        TimeTracker _tracker;
        GuidSet _allySet;
        uint32 _updateAlliesTimer;
        // 食尸鬼当前持有真实 CombatReference 的所有敌对单位。
        // 黑暗突变的顺劈可能会加入多个目标。
        GuidSet _dkGhoulCombatTargets;

        // 由宠物 AI 创建或认领的主人战斗引用。
        // 在 跟随/被动/死亡/消失/战斗结束 时被移除。
        GuidSet _dkGhoulOwnedOwnerCombatRefs;

        // 玩家主人亲自参与战斗的目标（造成伤害或受到该目标伤害）。
        // 食尸鬼召回时这些关系不会被移除。
        GuidSet _dkGhoulOwnerDirectCombatTargets;

        // 永久 DK 食尸鬼专用。这三个集合刻意分开存放，以便协助与防御模式遵循
        // retail 风格的规则：
        // 协助：仅主人真实造成伤害。
        // 防御：主人真实造成伤害、主人真实受到伤害、或食尸鬼真实受到伤害。
        GuidSet _dkGhoulOwnerDamageDealtTargets;
        GuidSet _dkGhoulOwnerDamageTakenTargets;
        GuidSet _dkGhoulGhoulDamageTakenTargets;

        // 黑暗突变激活期间从 world.creature_template 条目 141244
        // 选取的模型。为零表示当前未持有任何强制模型。
        uint32 _dkGhoulDarkTransformationDisplayId = 0;
};

#endif
