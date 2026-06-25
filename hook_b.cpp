// hook_b.cpp — 进下一层/存档时不持久化地面掉落物(默认关 cfg.clearOnTransition)
//
// 真实机制(subagent RE + sub_5C51A0/sub_5FF570 实读确认,旧版误判已纠正):
//   切层 sub_43D7B0 调 sub_5C51A0(CLevel*, CLevel) 把当前关卡地面单位序列化进新建的 CLevelState;
//   其中对每个地面单位调【两个孪生序列化函数】sub_5FF570 与 sub_5FF630
//     —— 各 alloc 568B、sub_5A96B0 建 CItemSaveState、调 unit->vtable[…] 序列化、push 进 vector。
//   两者都【只被 sub_5C51A0 调用】(各仅 1 个调用方),是关卡存盘里两趟物品序列化;
//   怪物走 sub_5FF6F0(不同大小,不在此列)。玩家背包/装备走另一条角色存档路径(sub_435E10),不碰这俩。
//
//   我们 hook 这两个函数:开关开 且 unit 是 CItem(掉落物)时直接跳过(不建、不存、不 push)→
//   该关卡存档里没有这些掉落 → 再回到该关卡时地面是空的。触发器/怪物照常序列化,关卡状态不破坏。
//   只跳过序列化、不碰活的 CItem、不释放任何对象 —— 零状态修改、零 free,安全。
//
//   ⚠ 注:sub_435E10 是角色存档(写 save.tmp),不是关卡存盘;早前误判照它设计的旧 Hook B 会破坏角色存档。
//
//   目标地址与 CItem RTTI TypeDescriptor 由 hooks.cpp 用 AOB 扫描得到后传入(不写死 RVA,跨版本通用)。
#include <windows.h>
#include <cstdint>
#include "loot_filter.h"
#include "MinHook.h"

// 本地 RTTI is-a CItem(与 hooks.cpp 的 IsCItem 同;hook_b.cpp 是独立 TU,自带一份)。
// 32-bit MSVC RTTI:vtable[-1]=COL;COL+0x10=CHD;CHD+8=基类数,+0xC=基类数组;BCD+0=TypeDescriptor。
static void* g_tdCItem_b = nullptr;
static bool IsCItemB(void* obj) {
    if (!obj || IsBadReadPtr(obj, 4)) return false;
    void** vt = *(void***)obj;
    if (!vt || IsBadReadPtr(vt - 1, 4)) return false;
    uint8_t* col = *(uint8_t**)(vt - 1);
    if (!col || IsBadReadPtr(col, 0x14)) return false;
    uint8_t* chd = *(uint8_t**)(col + 0x10);
    if (!chd || IsBadReadPtr(chd, 0x10)) return false;
    uint32_t num = *(uint32_t*)(chd + 8);
    void** arr  = *(void***)(chd + 0xC);
    if (!arr || num > 64 || IsBadReadPtr(arr, num * 4)) return false;
    for (uint32_t i = 0; i < num; ++i) {
        uint8_t* bcd = (uint8_t*)arr[i];
        if (bcd && !IsBadReadPtr(bcd, 4) && *(void**)bcd == g_tdCItem_b) return true;
    }
    return false;
}

// 单个地面单位序列化进关卡存档。__thiscall(CLevelState* this, Unit* unit):this=ecx,unit=栈参。
// 两个孪生目标各需独立 trampoline(MinHook 每 hook 一份原函数),故备两个 detour。
using SerItem_t = int(__fastcall*)(void* ecx, void* edx, void* unit);
static SerItem_t oSer0 = nullptr, oSer1 = nullptr;

static int __fastcall Detour_Ser0(void* ecx, void* edx, void* unit) {
    if (lf::G().cfg.clearOnTransition && IsCItemB(unit)) return 0;   // 跳过该掉落物的序列化(活物不受影响)
    return oSer0(ecx, edx, unit);
}
static int __fastcall Detour_Ser1(void* ecx, void* edx, void* unit) {
    if (lf::G().cfg.clearOnTransition && IsCItemB(unit)) return 0;
    return oSer1(ecx, edx, unit);
}

// targets[0..n-1] = AOB 命中的序列化函数(预期 2 个:sub_5FF570 + sub_5FF630);tdCItem = CItem RTTI TD。
void InstallHookB(void** targets, int n, void* tdCItem) {
    g_tdCItem_b = tdCItem;
    if (!tdCItem) { lf::Log("HookB: 无 CItem TD → 禁用"); return; }
    if (n >= 1 && targets[0]) {
        MH_STATUS s0 = MH_CreateHook(targets[0], (void*)&Detour_Ser0, (void**)&oSer0);
        if (s0 == MH_OK) MH_EnableHook(targets[0]);
        lf::Log("HookB: serialize hook[0] @ %p create=%d", targets[0], s0);
    }
    if (n >= 2 && targets[1]) {
        MH_STATUS s1 = MH_CreateHook(targets[1], (void*)&Detour_Ser1, (void**)&oSer1);
        if (s1 == MH_OK) MH_EnableHook(targets[1]);
        lf::Log("HookB: serialize hook[1] @ %p create=%d", targets[1], s1);
    }
    if (n < 1) lf::Log("HookB: 序列化签名未命中 → clearOnTransition 不可用");
}
