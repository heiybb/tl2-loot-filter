// loot_filter.h — TL2 掉落过滤器:共享配置 + 掉落日志 + 过滤判定
// 由 hook 线程(写日志/读规则)和 ImGui UI 线程(读日志/改规则)共用。
#pragma once
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>
#include <utility>

namespace lf {

// ---- 从 CItemSaveState 读出的一次掉落(RE 确认的偏移)----
// 名称 @ CItemSaveState+0x0C (wstring) ; 等级 @ +0x1FC(508) ; 见蓝图 §1
struct DropInfo {
    std::wstring name;              // CItem+0x520 / descriptor DISPLAYNAME
    int          level = 0;         // CItem+0x110
    int          unittype_id = -1;  // CItem+0x1AC(过滤用)
    std::wstring unittype_name;     // UNITTYPE 原文(英文,过滤/输入框比对用,勿翻译)
    std::wstring unittype_disp;     // UNITTYPE 显示名(zh build:TRANSLATION.DAT 译;en build=原文)
    bool         filtered = false;
    uint32_t     tick = 0;
};

// ---- 过滤规则(UI 可改)----
struct FilterConfig {
    bool   enabled = true;

    // 按 UNITTYPE 黑名单:存类型名(如 MAGIC SOCKETABLE);与掉落的 unittype 名(注册表反查)子串比对。
    std::vector<std::wstring> blockUnittypes;

    // 按名称关键字黑名单(子串,大小写不敏感)
    std::vector<std::wstring> blockNameContains;

    // 按稀有度过滤(unittype 名前缀,如 "LEGENDARY PISTOL"):勾选即隐藏该稀有度。
    // 注:绿(Enchanted)/蓝(Rare)底层都是 MAGIC unittype,颜色靠词缀数量区分,
    //     unittype 拆不开 → blockMagic 同时覆盖绿+蓝。
    bool   blockNormal    = false;   // 白 Common
    bool   blockMagic     = false;   // 绿 Enchanted + 蓝 Rare
    bool   blockUnique    = false;   // 金 Unique
    bool   blockLegendary = false;   // 红 Legendary
    bool   blockQuest     = false;   // 紫 Quest(QUESTITEM)

    // 按等级:低于玩家等级-N 的丢弃(needPlayerLevel 为真时才用)
    bool   useLevelFloor = false;
    int    levelFloor    = 0;     // 绝对等级下限;0 = 关

    // 安全开关:只记录不真删(先观察,确认无误再开真过滤)
    bool   logOnly = true;

    // Hook B(实验):进下一层时清空上一层地面掉落(默认关,见 hook_b.cpp 警告)
    bool   clearOnTransition = false;

    // UNITTYPE 读取(实验:GameWStr / sub_677F00 ABI 待核;默认关,先验证名称/等级再开)
    bool   readUnittype = false;
};

// 全局单例(进程内)
struct State {
    std::mutex            mtx;
    FilterConfig          cfg;
    std::deque<DropInfo>  log;       // 最近掉落(环形,UI 显示)
    size_t                logCap = 200;
    uint64_t              totalSeen = 0;
    uint64_t              totalFiltered = 0;
    bool                  uiVisible = true;
};
State& G();

// hook 调用:判定一次掉落是否应过滤(true=丢弃)。同时写入日志。
// 即便返回 true,若 cfg.logOnly 为真,hook 也不应真否决(只观察)。
bool EvaluateDrop(DropInfo& d);

// 诊断日志(写到游戏目录 tl2_loot_filter.log,排查注入/hook 是否生效)
void Log(const char* fmt, ...);

// 小工具
std::wstring ToUpper(std::wstring s);
bool ContainsCI(const std::wstring& hay, const std::wstring& needle);

} // namespace lf
