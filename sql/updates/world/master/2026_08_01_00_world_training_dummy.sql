/*
 * 修复训练假人(entry=46647)的配置：
 * 1. AIName/ScriptName 设置为 npc_training_dummy，继承 NullCreatureAI 被动行为，
 *    并在 DamageTaken 中将伤害归零，实现"不掉血且伤害数字可见"。
 * 2. ContentTuningID=181 使假人等级同步玩家等级（官服机制）。
 * 3. HealthModifier=25 提供高血量基础值，避免初始血量被钳制到1HP。
 *    原 DB 中 HealthModifier=0.000013 导致初始血量=1HP，被打即死，并保存
 *    curHealthPct=0% 到 creature 表，重载后假人恒为0血量。
 *
 * 对应修复总结：训练假人掉血/0血量问题
 */

-- 设置AI脚本：使用 npc_training_dummy 脚本（继承 NullCreatureAI + DamageTaken 拦截）
UPDATE `creature_template` SET `AIName` = 'npc_training_dummy', `ScriptName` = 'npc_training_dummy' WHERE `entry` = 46647;

-- 设置等级同步玩家（ContentTuningID=181）+ 高血量倍数（HealthModifier=25）
UPDATE `creature_template_difficulty` SET `ContentTuningID` = 181, `HealthModifier` = 25 WHERE `Entry` = 46647;

-- 防止 creature 表中 curHealthPct=0 导致重载后假人0血量
UPDATE `creature` SET `curHealthPct` = 100 WHERE `id1` = 46647;
