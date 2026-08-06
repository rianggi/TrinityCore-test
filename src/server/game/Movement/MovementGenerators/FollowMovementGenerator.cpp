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

#include "FollowMovementGenerator.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Optional.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Unit.h"

static void DoMovementInform(Unit* owner, Unit* target)
{
    if (owner->GetTypeId() != TYPEID_UNIT)
        return;

    if (CreatureAI* AI = owner->ToCreature()->AI())
        AI->MovementInform(FOLLOW_MOTION_TYPE, target->GetGUID().GetCounter());
}

FollowMovementGenerator::FollowMovementGenerator(Unit* target, float range, Optional<ChaseAngle> angle, Optional<Milliseconds> duration,
    bool ignoreTargetWalk /*= false*/, Scripting::v2::ActionResultSetter<MovementStopReason>&& scriptResult /*= {}*/)
    : AbstractFollower(ASSERT_NOTNULL(target)), _range(range), _angle(angle), _ignoreTargetWalk(ignoreTargetWalk), _checkTimer(CHECK_INTERVAL),
      _dkGhoulMovingRepathTimer(0), _dkGhoulLastOwnerFacing()
{
    Mode = MOTION_MODE_DEFAULT;
    Priority = MOTION_PRIORITY_NORMAL;
    Flags = MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING;
    BaseUnitState = UNIT_STATE_FOLLOW;
    ScriptResult = std::move(scriptResult);
    if (duration)
        _duration.emplace(*duration);
}

// ponytail: 判断是否邪DK永久食尸鬼宠物(entry=26125,IsPet=true)
// UpgradePath: 后续如需支持更多DK召唤物类型,扩展entry白名单或改用SpellFamily判断
bool FollowMovementGenerator::_IsDKRisenGhoul(Unit const* owner)
{
    if (!owner) return false;
    Pet const* pet = owner->ToPet();
    if (!pet || !pet->IsPet()) return false;
    return pet->GetEntry() == 26125;
}
FollowMovementGenerator::~FollowMovementGenerator() = default;

static bool PositionOkay(Unit* owner, Unit* target, float range, Optional<ChaseAngle> angle = {})
{
    if (!owner->IsInDist(target, owner->GetCombatReach() + target->GetCombatReach() + range))
        return false;

    return !angle || angle->IsAngleOkay(target->GetRelativeAngle(owner));
}

void FollowMovementGenerator::Initialize(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING | MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    AddFlag(MOVEMENTGENERATOR_FLAG_INITIALIZED | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

    owner->StopMoving();
    UpdatePetSpeed(owner);
    _path = nullptr;
    _lastTargetPosition.reset();

    // ponytail: DK食尸鬼跟随状态重置
    _dkGhoulMovingRepathTimer = 0;
    _dkGhoulLastOwnerFacing.reset();
}

void FollowMovementGenerator::Reset(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);

    Initialize(owner);
}

bool FollowMovementGenerator::Update(Unit* owner, uint32 diff)
{
    // owner might be dead or gone
    if (!owner || !owner->IsAlive())
        return false;

    // our target might have gone away
    Unit* const target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    if (_duration)
    {
        _duration->Update(diff);
        if (_duration->Passed())
        {
            owner->StopMoving();
            DoMovementInform(owner, target);
            return false;
        }
    }

    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE) || owner->IsMovementPreventedByCasting())
    {
        _path = nullptr;
        owner->StopMoving();
        _lastTargetPosition.reset();
        return true;
    }

    float range = _range;
    if (Creature* cOwner = owner->ToCreature())
        if (cOwner->IsIgnoringChaseRange())
            range = 0.0f;

    // ponytail: DK食尸鬼专属计时器更新
    bool const isDKGhoul = _IsDKRisenGhoul(owner);
    if (isDKGhoul && _dkGhoulMovingRepathTimer > 0)
        _dkGhoulMovingRepathTimer = _dkGhoulMovingRepathTimer > diff ? (_dkGhoulMovingRepathTimer - diff) : 0;

    _checkTimer.Update(diff);
    if (_checkTimer.Passed())
    {
        _checkTimer.Reset(CHECK_INTERVAL);
        if (HasFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED) && PositionOkay(owner, target, range, _angle))
        {
            RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
            _path = nullptr;
            owner->StopMoving();
            _lastTargetPosition.reset();
            DoMovementInform(owner, target);
            return true;
        }
    }

    if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) && owner->movespline->Finalized())
    {
        RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
        _path = nullptr;
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        DoMovementInform(owner, target);
    }

    // ponytail: 判断是否需要重发跟随spline
    // DK食尸鬼分支: 位置变化≥阈值 或 (转向幅度够大且repath冷却已到),避免每帧launch造成加速假象
    // 其他单位: 保留原逻辑(任何位置变化都触发)
    bool needRepath = false;
    if (isDKGhoul)
    {
        if (!_lastTargetPosition)
        {
            needRepath = true;
        }
        else
        {
            float distSq = _lastTargetPosition->GetExactDist2dSq(target->GetPosition());
            // 主人移动距离≥阈值才重算路径,防抖
            if (distSq >= float(DK_RISEN_GHOUL_MOVING_REPATH_DIST_THRESHOLD * DK_RISEN_GHOUL_MOVING_REPATH_DIST_THRESHOLD))
            {
                needRepath = true;
            }
            else
            {
                // 主人原地快速转向检测: 朝向变化够大,且repath冷却到了(防转向鬼畜)
                float curFacing = target->GetOrientation();
                if (_dkGhoulLastOwnerFacing)
                {
                    float turnDelta = std::fabs(Position::NormalizeOrientation(curFacing - *_dkGhoulLastOwnerFacing));
                    // 转向≥30度,且距离上次转向repath≥500ms,才允许触发
                    if (turnDelta >= 0.5236f && _dkGhoulMovingRepathTimer == 0)
                        needRepath = true;
                }
            }
        }
        if (needRepath)
            _dkGhoulLastOwnerFacing = target->GetOrientation();
    }
    else
    {
        needRepath = !_lastTargetPosition || (_lastTargetPosition->GetExactDistSq(target->GetPosition()) > 0.0f);
    }

    if (needRepath)
    {
        _lastTargetPosition = target->GetPosition();

        // ponytail: DK食尸鬼移动中不节流,直接允许重发spline(被中断而非完成→无停顿);
        // 但当前spline尚未Finalized时不重launch,避免每帧中断造成加速假象
        bool const splineRunning = owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) && !owner->movespline->Finalized();
        bool const posOkay = PositionOkay(owner, target, range + FOLLOW_RANGE_TOLERANCE);

        if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) || !posOkay)
        {
            if (isDKGhoul && splineRunning)
            {
                // ponytail: 食尸鬼spline在跑且距离目标位置尚远→不中断当前spline,等它自然结束或下次触发
                // 只有离目标点明显偏离时才中断重算(阈值=跟随距离+平滑距离)
                float offTargetSq = owner->GetExactDist2dSq(target->GetPositionX(), target->GetPositionY());
                float thresh = range + DK_RISEN_GHOUL_STRAFE_DEST_SMOOTH_DISTANCE + FOLLOW_RANGE_TOLERANCE;
                if (offTargetSq <= thresh * thresh)
                    return true;
            }

            if (!_path)
                _path = std::make_unique<PathGenerator>(owner);

            float x, y, z;

            // select angle
            float tAngle;
            float const curAngle = target->GetRelativeAngle(owner);
            if (!_angle || _angle->IsAngleOkay(curAngle))
                tAngle = curAngle;
            else
            {
                float const diffUpper = Position::NormalizeOrientation(curAngle - _angle->UpperBound());
                float const diffLower = Position::NormalizeOrientation(_angle->LowerBound() - curAngle);
                if (diffUpper < diffLower)
                    tAngle = _angle->UpperBound();
                else
                    tAngle = _angle->LowerBound();
            }

            target->GetNearPoint(owner, x, y, z, range, target->ToAbsoluteAngle(tAngle));

            if (owner->IsHovering())
                owner->UpdateAllowedPositionZ(x, y, z);

            // pets are allowed to "cheat" on pathfinding when following their master
            bool allowShortcut = false;
            if (Pet* oPet = owner->ToPet())
            {
                if (target->GetGUID() == oPet->GetOwnerGUID())
                    allowShortcut = true;
            }

            bool success = _path->CalculatePath(x, y, z, allowShortcut);
            if (!success || (_path->GetPathType() & PATHFIND_NOPATH))
            {
                owner->StopMoving();
                return true;
            }

            owner->AddUnitState(UNIT_STATE_FOLLOW_MOVE);
            AddFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

            Movement::MoveSplineInit init(owner);
            init.MovebyPath(_path->GetPath());
            if (!_ignoreTargetWalk)
                init.SetWalk(target->IsWalking());

            // ponytail: DK食尸鬼用SetVelocity控制spline持续时间≈repath间隔,避免"减速到0再启动"的卡顿感
            // UpgradePath: 如后续发现跟随抖动,可微调LEAD_TIME或改为基于主人速度动态计算
            if (isDKGhoul)
            {
                float pathLen = _path ? _path->GetPathLength() : 0.0f;
                if (pathLen > 0.1f)
                {
                    float durSec = std::max(DK_RISEN_GHOUL_MOVING_LEAD_TIME,
                        float(CHECK_INTERVAL) / 1000.0f);
                    float velocity = pathLen / durSec;
                    init.SetVelocity(velocity);
                }
                // 本次是因转向触发的repath→进入冷却,500ms内不再因转向重发
                _dkGhoulMovingRepathTimer = DK_RISEN_GHOUL_MOVING_TURN_REPATH_MIN_INTERVAL;
            }

            init.SetFacing(target->GetOrientation());
            init.Launch();
        }
    }
    return true;
}

void FollowMovementGenerator::Deactivate(Unit* owner)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    RemoveFlag(MOVEMENTGENERATOR_FLAG_TRANSITORY | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
    owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
}

void FollowMovementGenerator::Finalize(Unit* owner, bool active, bool movementInform)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_FINALIZED);
    if (active)
    {
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        UpdatePetSpeed(owner);
        if (movementInform)
            SetScriptResult(MovementStopReason::Finished);
    }
}

void FollowMovementGenerator::UpdatePetSpeed(Unit* owner)
{
    if (Pet* oPet = owner->ToPet())
    {
        if (!GetTarget() || GetTarget()->GetGUID() == owner->GetOwnerGUID())
        {
            oPet->UpdateSpeed(MOVE_RUN);
            oPet->UpdateSpeed(MOVE_WALK);
            oPet->UpdateSpeed(MOVE_SWIM);
        }
    }
}
