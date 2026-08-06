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

#ifndef TRINITY_FOLLOWMOVEMENTGENERATOR_H
#define TRINITY_FOLLOWMOVEMENTGENERATOR_H

#include "AbstractFollower.h"
#include "MovementDefines.h"
#include "MovementGenerator.h"
#include "Position.h"
#include "Timer.h"

class PathGenerator;
class Unit;

#define FOLLOW_RANGE_TOLERANCE 1.0f

class FollowMovementGenerator : public MovementGenerator, public AbstractFollower
{
    public:
        explicit FollowMovementGenerator(Unit* target, float range, Optional<ChaseAngle> angle, Optional<Milliseconds> duration,
            bool ignoreTargetWalk = false, Scripting::v2::ActionResultSetter<MovementStopReason>&& scriptResult = {});
        ~FollowMovementGenerator();

        void Initialize(Unit*) override;
        void Reset(Unit*) override;
        bool Update(Unit*, uint32) override;
        void Deactivate(Unit*) override;
        void Finalize(Unit*, bool, bool) override;
        MovementGeneratorType GetMovementGeneratorType() const override { return FOLLOW_MOTION_TYPE; }

        void UnitSpeedChanged() override { _lastTargetPosition.reset(); }

    private:
        static constexpr uint32 CHECK_INTERVAL = 100;

        // ponytail: DK食尸鬼跟随专属常量
        // UpgradePath: 如需调整跟随手感,改这些常量即可;要做职业配置化再移到conf
        static constexpr uint32 DK_RISEN_GHOUL_MOVING_REPATH_DIST_THRESHOLD = 4; // 主人移动≥4码才重算路径
        static constexpr uint32 DK_RISEN_GHOUL_MOVING_TURN_REPATH_MIN_INTERVAL = 500; // 快速转向时repath最小间隔(ms),防转向鬼畜
        static constexpr float DK_RISEN_GHOUL_MOVING_LEAD_TIME = 0.3f; // spline预计持续时间,配合SetVelocity让移动≈repath间隔
        static constexpr float DK_RISEN_GHOUL_STRAFE_DEST_SMOOTH_DISTANCE = 2.0f; // 侧移目标点平滑距离阈值

        // 判断是否DK食尸鬼(entry=26125,邪DK永久宠物)
        static bool _IsDKRisenGhoul(Unit const* owner);

        void UpdatePetSpeed(Unit* owner);

        float const _range;
        Optional<ChaseAngle const> _angle;
        bool _ignoreTargetWalk;

        TimeTracker _checkTimer;
        Optional<TimeTracker> _duration;
        std::unique_ptr<PathGenerator> _path;
        Optional<Position> _lastTargetPosition;

        // ponytail: DK食尸鬼跟随状态
        uint32 _dkGhoulMovingRepathTimer; // 转向repath冷却计时
        Optional<float> _dkGhoulLastOwnerFacing; // 上次主人朝向,用于检测转向幅度
};

#endif
