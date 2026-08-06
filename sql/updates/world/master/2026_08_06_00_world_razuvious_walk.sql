-- =============================================
-- 教官拉苏维奥斯 (Entry 28357) 走路修复
-- 日期: 2026-08-06
-- 库: world_1205
-- =============================================

USE world_1205;

-- 1. 修正走路速度 (原 1.8 异常偏快, 改为 1.2)
UPDATE creature_template SET speed_walk = 1.2 WHERE entry = 28357;

-- 2. 新增 SmartAI 脚本: AI_INIT 时强制走路 (SetRun(false))
-- event_type=37 (SMART_EVENT_AI_INIT)
-- action_type=59 (SMART_ACTION_SET_RUN)
-- action_param1=0 (false = 走路模式)
INSERT INTO smart_scripts (entryorguid, source_type, id, event_type, action_type, action_param1, comment)
VALUES (28357, 0, 10, 37, 59, 0, 'Force walk on AI init')
ON DUPLICATE KEY UPDATE event_type = 37, action_type = 59, action_param1 = 0, comment = 'Force walk on AI init';

-- =============================================
-- 回退 SQL (如需回退):
-- UPDATE creature_template SET speed_walk = 1.8 WHERE entry = 28357;
-- DELETE FROM smart_scripts WHERE entryorguid = 28357 AND id = 10;
-- =============================================