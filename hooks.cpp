// hooks.cpp — d3d9 wrapper + 引擎函数解析 + 掉落处理器 detour + ImGui Present hook
//
// 解析策略:Torchlight2.exe 是 2020 固定二进制 → 函数 RVA 恒定,仅 ASLR 改基址。
//   实际地址 = GetModuleHandleW(NULL) + (IDA地址 - 0x400000)
//   (蓝图 §6 的 AOB 签名作为二进制若变更时的 fallback,这里默认走 RVA。)
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <map>
#include <vector>
#include "loot_filter.h"
#include "MinHook.h"

// overlay.cpp
namespace overlay { void InstallPresentHook(); }

// ----------------------------------------------------------------------------
// 1) 引擎定位 —— AOB(字节特征码)扫描,不写死 RVA。
//    跨 build 通用:1.25.9.5 与 Steam 1.25.5.6 实测每条签名各唯一命中(地址不同、代码布局一致)。
//    运行时扫已加载镜像内存(代码已被 loader 重定位)→ 命中地址即活地址,无需基址换算;
//    call/绝对地址/SEH 表项在签名里均已通配,故重排后仍命中。
// ----------------------------------------------------------------------------
// 落地收口 sub_5EAA80(__thiscall(CLevel*, CBaseUnit* unit, Vector3* pos, char flag))——所有掉落统一落地点。
using Place_t    = int*(__fastcall*)(void* ecx, void* edx, int unit, int pos, char flag);
// 读命名属性 sub_677F00(__thiscall(node=ecx, name, def))。GUID 取到的持久节点可读。
using ReadProp_t = const void*(__fastcall*)(void* ecx, void* edx, const void* name, const void* def);
// GUID→单位定义节点 sub_65FDE0(__thiscall(manager, GUID_lo, GUID_hi)→node)。
using GetNode_t  = void*(__fastcall*)(void* ecx, void* edx, int guidLo, int guidHi);
static Place_t    pPlace = nullptr, oPlace = nullptr;
static ReadProp_t pReadProp = nullptr;
static GetNode_t  pGetNode = nullptr;
static uintptr_t  g_managerPtr = 0;         // &(units 数据库单例指针);从 pGetNode 体内 mov ecx,[X] 抽取
static void*      g_tdCItem    = nullptr;   // CItem RTTI TypeDescriptor(".?AVCItem@@" 串 − 8)
// UNITTYPE 中文名从内存翻译表读(替代读解包 TRANSLATION.DAT,普通玩家 PAK/MOD 也可用)。
// 只是"读全局":翻译实例=*(g_transInstPtr)。(unittype 英文名改用烤进 DLL 的 kItemUnittypes,见下。)
static uintptr_t  g_transInstPtr = 0;        // &CStringTranslate 实例全局(getter sub_6580E0 体内 mov eax,[X])

// 游戏 std::wstring(VC10/MSVCP100,_SECURE_SCL=1):_Myproxy@+0 / _Bx@+4 / _Mysize@+20 / _Myres@+24。
struct GameWStr { void* proxy; const wchar_t* ptr; char _pad[12]; uint32_t size; uint32_t cap; };

// ---- AOB 签名(IDA 格式;'?'=通配一字节)。逐条已在两版 i64 交叉验证。----
static const char* SIG_PLACE     = "56 8B 74 24 ? 57 8B F9 85 F6 75 ? 5F 33 C0 5E C2 0C 00";          // sub_5EAA80
static const char* SIG_READPROP  = "8B 44 24 ? 50 E8 ? ? ? ? 85 C0 75 ? 8B 44 24 ? C2 08 00 6A 01";  // sub_677F00
static const char* SIG_GETNODE   = "83 7C 24 ? ? 75 ? 83 7C 24 ? ? 75 ? 33 C0";                      // sub_65FDE0
static const char* SIG_MGRLOAD   = "8B 0D ? ? ? ? 56 E8 ? ? ? ? 8B 46 48";                            // mov ecx,[manager](pGetNode 体内)
static const char* SIG_RTTICITEM = "2E 3F 41 56 43 49 74 65 6D 40 40 00";                             // ".?AVCItem@@\0"
// 关卡存盘逐单位序列化 sub_5FF570 + 孪生 sub_5FF630(都只被关卡序列化 sub_5C51A0 调,都 build 568B CItemSaveState)。
static const char* SIG_SERITEM   = "6A FF 68 ? ? ? ? 64 A1 ? ? ? ? 50 64 89 25 ? ? ? ? 51 56 8B F1 80 BE ? ? ? ? ? 57 8B 7C 24 ? 74 ? 68 43 01 00 00 8B CF E8 ? ? ? ? 84 C0 75 ? C6 86 ? ? ? ? ? E8 ? ? ? ? 8B 40 ? 89 44 24 ? 85 C0 74 ? 8B 10 8B C8 8B 02 68 38 02 00 00";
// 翻译实例全局 getter sub_6580E0(mov eax,[inst]; test; cmp byte[eax+38],0; ...)。
static const char* SIG_TRANSINST = "A1 ? ? ? ? 85 C0 74 06 80 78 38 00 75 02 33 C0 C3";

static uint8_t* g_modBase = nullptr;
static size_t   g_modSize = 0;

// 解析签名并在 [start,end) 内找首个匹配;无→nullptr。
static uint8_t* ScanRange(const uint8_t* start, const uint8_t* end, const char* sig) {
    uint8_t pat[300]; bool wild[300]; int n = 0;
    auto hx = [](char c)->int { if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return -1; };
    for (const char* p = sig; *p && n < 300; ) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { wild[n]=true; pat[n]=0; ++n; ++p; if (*p=='?') ++p; continue; }
        int hi = hx(p[0]), lo = (p[0] ? hx(p[1]) : -1);
        if (hi < 0 || lo < 0) break;
        pat[n] = (uint8_t)((hi << 4) | lo); wild[n] = false; ++n; p += 2;
    }
    if (n == 0 || !start) return nullptr;
    for (const uint8_t* s = start; s + n <= end; ++s) {
        int i = 0; for (; i < n; ++i) if (!wild[i] && s[i] != pat[i]) break;
        if (i == n) return (uint8_t*)s;
    }
    return nullptr;
}
static uint8_t* Scan(const char* sig) { return ScanRange(g_modBase, g_modBase + g_modSize, sig); }
// 找全部匹配(最多 maxOut 个),返回个数。
static int ScanAll(const char* sig, uint8_t** out, int maxOut) {
    int cnt = 0; const uint8_t* s = g_modBase; const uint8_t* end = g_modBase + g_modSize;
    while (cnt < maxOut) {
        uint8_t* m = ScanRange(s, end, sig);
        if (!m) break;
        out[cnt++] = m; s = m + 1;
    }
    return cnt;
}

static void ResolveEngine() {
    g_modBase = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (PIMAGE_DOS_HEADER)g_modBase;
    auto nt  = (PIMAGE_NT_HEADERS)(g_modBase + dos->e_lfanew);
    g_modSize = nt->OptionalHeader.SizeOfImage;

    pPlace     = (Place_t)    Scan(SIG_PLACE);
    pReadProp  = (ReadProp_t) Scan(SIG_READPROP);
    pGetNode   = (GetNode_t)  Scan(SIG_GETNODE);
    // units 数据库全局:pGetNode 体内 `mov ecx,[X]` 的 imm32。
    g_managerPtr = 0;
    if (pGetNode) {
        uint8_t* ld = ScanRange((uint8_t*)pGetNode, (uint8_t*)pGetNode + 0x80, SIG_MGRLOAD);
        if (ld) g_managerPtr = *(uintptr_t*)(ld + 2);
    }
    // CItem RTTI TypeDescriptor = 名称串 ".?AVCItem@@" − 8(TD 布局:vftable@0/spare@4/name@8)。
    uint8_t* rtti = Scan(SIG_RTTICITEM);
    g_tdCItem = rtti ? (void*)(rtti - 8) : nullptr;

    // 翻译实例全局:getter 体内 `mov eax,[imm32]` 的 imm32(已重定位的绝对地址)。
    uint8_t* ti = Scan(SIG_TRANSINST);
    g_transInstPtr = ti ? *(uintptr_t*)(ti + 1) : 0;

    lf::Log("AOB resolve: place=%p readprop=%p getnode=%p mgrPtr=%p tdItem=%p transInst=%p",
            pPlace, pReadProp, pGetNode, (void*)g_managerPtr, g_tdCItem, (void*)g_transInstPtr);
}

// SEH 安全:从数据节点(ecx=node)读命名属性(sub_677F00)。失败/异常 → 空。
static bool SafeReadPropRaw(void* node, const wchar_t* key, uint32_t keyLen, wchar_t* out, uint32_t outCap) {
    out[0] = 0;
    if (!pReadProp || !node) return false;
    __try {
        GameWStr def{ nullptr, L"", {}, 0, 0xFFFF };
        GameWStr k  { nullptr, key, {}, keyLen, 0xFFFF };
        const uint8_t* r = (const uint8_t*)pReadProp(node, nullptr, &k, &def);  // ecx = 节点
        if (!r) return false;
        uint32_t len = *(const uint32_t*)(r + 20);
        uint32_t cap = *(const uint32_t*)(r + 24);
        if (len == 0 || len >= outCap) return false;
        const wchar_t* p = (cap >= 8) ? *(const wchar_t* const*)(r + 4) : (const wchar_t*)(r + 4);
        if (!p) return false;
        for (uint32_t i = 0; i < len; ++i) out[i] = p[i];
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
static std::wstring SafeReadProp(void* node, const wchar_t* key, uint32_t keyLen) {
    wchar_t buf[256];
    return SafeReadPropRaw(node, key, keyLen, buf, 256) ? std::wstring(buf) : std::wstring();
}

// 读地址 a 处的 std::wstring 到定长缓冲(无对象,可入 __try);返回 wchar 数(0=空/异常)。
static uint32_t ReadGameWStrRaw(const void* a, wchar_t* out, uint32_t outCap) {
    out[0] = 0;
    __try {
        const uint8_t* r = (const uint8_t*)a;
        uint32_t len = *(const uint32_t*)(r + 20);          // _Mysize
        uint32_t cap = *(const uint32_t*)(r + 24);          // _Myres
        if (len == 0 || len >= outCap) return 0;
        const wchar_t* p = (cap >= 8) ? *(const wchar_t* const*)(r + 4) : (const wchar_t*)(r + 4);
        if (!p) return 0;
        for (uint32_t i = 0; i < len; ++i) out[i] = p[i];
        out[len] = 0;
        return len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
}
// 读 BST 节点子指针(left@+0 / right@+4);false=异常。
static bool ReadBstChildren(void* node, void** L, void** R) {
    __try { *L = *(void**)((uint8_t*)node + 0); *R = *(void**)((uint8_t*)node + 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 注:上面 ReadGameWStrRaw / ReadBstChildren 现仅供翻译表 BST(LookupTranslation)使用。
// unittype id→名 不再遍历游戏内存,改用烤进 DLL 的 kItemUnittypes 表(见下方 UnittypeName)——
// 装备/物品 UNITTYPE 是基础游戏固定定义(mod 只用不增),id 稳定,无需运行时读注册表。

// ----------------------------------------------------------------------------
// UNITTYPE 中文名:直接查游戏内存里的翻译表(CStringTranslate 的 ORIGINAL→TRANSLATION BST),
// 替代读 mod 的 TRANSLATION.DAT —— 普通玩家无解包文件也可用,且反映游戏真实 mod 覆盖顺序(load order)。
// 仅 zh build 用(en build TranslateUnittype 直接返回原文)。翻译实例 = *(g_transInstPtr),
// +0x38 为已加载标志,this[2](inst+8)= BST 根;节点 key(原文 wstring)@+16 / value(译文 wstring)@+44。
// 大小写敏感比较 → 只命中全大写的 unittype 类键(混合大小写的物品名/UI 文案不会误命中)。
// 不调游戏翻译函数 sub_6583B0(它构造 std::wstring,跨 DLL 用游戏 allocator 析构有风险),改自己读树。
// ----------------------------------------------------------------------------
// 取翻译 BST 根(__try 单独成函数返回 POD,避免与 wstring 临时对象同函数触发 C2712)。
static void* GetTranslateRoot() {
    if (!g_transInstPtr) return nullptr;
    __try {
        void* inst = *(void**)g_transInstPtr;
        if (inst && *(uint8_t*)((uint8_t*)inst + 0x38))     // +0x38 = 翻译已加载
            return *(void**)((uint8_t*)inst + 8);           // inst[2] = ORIGINAL→TRANSLATION BST 根
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
static std::wstring LookupTranslation(const std::wstring& key) {
    if (key.empty()) return std::wstring();
    void* node = GetTranslateRoot(); int guard = 0;
    while (node && guard++ < 8192) {
        wchar_t k[256]; ReadGameWStrRaw((uint8_t*)node + 16, k, 256);
        void *L = nullptr, *R = nullptr;
        if (!ReadBstChildren(node, &L, &R)) break;
        int cmp = key.compare(k);                            // 与游戏 std::wstring operator< 同字典序
        if (cmp < 0)      node = L;
        else if (cmp > 0) node = R;
        else { wchar_t v[256]; return ReadGameWStrRaw((uint8_t*)node + 44, v, 256) ? std::wstring(v) : std::wstring(); }
    }
    return std::wstring();
}

// "MAGIC 2HSWORD" → "魔法双手剑";"LEGENDARY BELT" 整串直命中→"传奇腰带";无译→原文。
static std::wstring TranslateUnittype(const std::wstring& full) {
#ifdef LF_EN
    return full;                                             // en 版不翻译
#else
    if (full.empty()) return full;
    std::wstring U = lf::ToUpper(full);
    std::wstring t = LookupTranslation(U);                   // 1) 整串(含 rarity)直接命中
    if (!t.empty()) return t;
    static const struct { const wchar_t* en; const wchar_t* zh; } kRar[] = {
        { L"NORMAL ",    L"普通" }, { L"MAGIC ",  L"魔法" }, { L"UNIQUE ", L"暗金" },
        { L"LEGENDARY ", L"传奇" }, { L"QUEST ",  L"任务" },
    };
    for (auto& r : kRar) {                                   // 2) 拆 rarity 前缀 + 翻译基本型
        size_t rl = wcslen(r.en);
        if (U.compare(0, rl, r.en) == 0) {
            std::wstring bt = LookupTranslation(U.substr(rl));
            if (!bt.empty()) return std::wstring(r.zh) + bt;
            break;
        }
    }
    return full;                                             // 3) 查不到 → 原文英文
#endif
}

// ----------------------------------------------------------------------------
// 物品名缓存:GUID→DISPLAYNAME。落地点 descriptor 临时节点已清空、NAME 字段落地后才写,
// 故改用 GUID 取持久单位定义节点再读 DISPLAYNAME(ItemNameByGuid),惰性填充本表 —— 纯内存、可移植。
// ----------------------------------------------------------------------------
static std::map<uint64_t, std::wstring> g_itemNames;
static std::mutex g_itemNamesMtx;                      // g_itemNames 读写互斥

// SEH 安全:GUID→单位定义节点(manager=*g_managerPtr;node=sub_65FDE0(manager, lo, hi))。
static void* GetItemNodeRaw(uint32_t lo, uint32_t hi) {
    if (!pGetNode || !g_managerPtr) return nullptr;
    __try {
        void* mgr = *(void**)g_managerPtr;
        return mgr ? pGetNode(mgr, nullptr, (int)lo, (int)hi) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// GUID→DISPLAYNAME(内存:取持久定义节点 → sub_677F00 读 DISPLAYNAME),带缓存。
static std::wstring ItemNameByGuid(uint32_t lo, uint32_t hi) {
    uint64_t guid = ((uint64_t)hi << 32) | lo;
    if (!guid || guid == 0xFFFFFFFFFFFFFFFFull) return L"";
    {
        std::lock_guard<std::mutex> lk(g_itemNamesMtx);
        auto it = g_itemNames.find(guid);
        if (it != g_itemNames.end()) return it->second;
    }
    void* node = GetItemNodeRaw(lo, hi);
    std::wstring nm = node ? SafeReadProp(node, L"DISPLAYNAME", 11) : std::wstring();
    {
        std::lock_guard<std::mutex> lk(g_itemNamesMtx);
        g_itemNames[guid] = nm;
    }
    return nm;
}

// 手写 RTTI is-a 检查:该 unit 的类层级里是否含 CItem(避开 CRT 的 __RTDynamicCast 声明冲突)。
// 32-bit MSVC RTTI:vtable[-1]=COL;COL+0x10=ClassHierarchyDescriptor;CHD+8=基类数,+0xC=基类数组;
// 每个 BaseClassDescriptor+0 = TypeDescriptor。CItem 是单继承,偏移 0,unit 本身即 CItem*。
static bool IsType(void* obj, void* td) {
    if (!obj || !td || IsBadReadPtr(obj, 4)) return false;
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
        if (bcd && !IsBadReadPtr(bcd, 4) && *(void**)bcd == td) return true;
    }
    return false;
}
static bool IsCItem(void* obj) { return IsType(obj, g_tdCItem); }

// 物品 unittype 白名单(tools/extract_item_unittypes.py 扫 ITEMS .DAT 得来,基础+所有 mod;已排序)。
// 非此集合的(INTERACTABLE/OPENABLE/WAYPOINTNODE 等道具场景)不是掉落物,跳过不处理。
struct UtEntry { int id; const wchar_t* name; };
static const UtEntry kItemUnittypes[] = {  // {id, 名};按 id 排序;供二分(IsItemUnittype/UnittypeName 共用)
    {    1, L"CONSUMABLE" },
    {    2, L"ITEM" },
    {   11, L"SWORD" },
    {   33, L"POTION" },
    {   34, L"GOLD" },
    {   42, L"SCROLL" },
    {   44, L"AXE" },
    {   47, L"UNIQUE BOW" },
    {   48, L"MAGIC SWORD" },
    {   50, L"MAGIC BOW" },
    {   51, L"NORMAL SWORD" },
    {   53, L"NORMAL BOW" },
    {   56, L"MAGIC NECKLACE" },
    {   57, L"MAGIC RING" },
    {   58, L"UNIQUE NECKLACE" },
    {   59, L"UNIQUE RING" },
    {   61, L"STAFF" },
    {   62, L"NORMAL STAFF" },
    {   63, L"MAGIC STAFF" },
    {   64, L"UNIQUE STAFF" },
    {   65, L"NORMAL HELMET" },
    {   66, L"MAGIC HELMET" },
    {   67, L"UNIQUE HELMET" },
    {   68, L"NORMAL SHIELD" },
    {   69, L"MAGIC SHIELD" },
    {   70, L"UNIQUE SHIELD" },
    {   71, L"NORMAL BELT" },
    {   72, L"MAGIC BELT" },
    {   73, L"UNIQUE BELT" },
    {   74, L"NORMAL GLOVES" },
    {   75, L"MAGIC GLOVES" },
    {   76, L"UNIQUE GLOVES" },
    {   77, L"NORMAL SHOULDER ARMOR" },
    {   78, L"MAGIC SHOULDER ARMOR" },
    {   79, L"UNIQUE SHOULDER ARMOR" },
    {   80, L"NORMAL BOOTS" },
    {   81, L"MAGIC BOOTS" },
    {   82, L"UNIQUE BOOTS" },
    {   83, L"NORMAL AXE" },
    {   84, L"NORMAL CHEST ARMOR" },
    {   85, L"MAGIC CHEST ARMOR" },
    {   86, L"UNIQUE CHEST ARMOR" },
    {   89, L"MACE" },
    {   95, L"NORMAL PISTOL" },
    {   96, L"MAGIC PISTOL" },
    {   97, L"UNIQUE PISTOL" },
    {   99, L"NORMAL WAND" },
    {  100, L"MAGIC WAND" },
    {  101, L"UNIQUE WAND" },
    {  103, L"QUESTITEM" },
    {  105, L"POLEARM" },
    {  106, L"NORMAL POLEARM" },
    {  107, L"MAGIC POLEARM" },
    {  108, L"UNIQUE POLEARM" },
    {  112, L"NORMAL CROSSBOW" },
    {  113, L"MAGIC CROSSBOW" },
    {  114, L"UNIQUE CROSSBOW" },
    {  117, L"NORMAL RIFLE" },
    {  118, L"MAGIC RIFLE" },
    {  119, L"UNIQUE RIFLE" },
    {  120, L"SOCKETABLE" },
    {  121, L"UNIQUE SOCKETABLE" },
    {  124, L"FISH" },
    {  129, L"SPELL" },
    {  155, L"MAGIC POTION" },
    {  156, L"UNIQUE POTION" },
    {  160, L"RANDOMMAGIC SOCKETABLE" },
    {  172, L"MAP" },
    {  177, L"DAGGER" },
    {  179, L"2HAXE" },
    {  180, L"2HSWORD" },
    {  187, L"2HMACE" },
    {  191, L"UNIQUE 2HAXE" },
    {  192, L"UNIQUE 2HSWORD" },
    {  193, L"UNIQUE 2HMACE" },
    {  194, L"UNIQUECANNON" },
    {  197, L"NORMAL 2HAXE" },
    {  198, L"NORMAL 2HSWORD" },
    {  199, L"NORMAL 2HMACE" },
    {  200, L"NORMAL CANNON" },
    {  203, L"MAGIC 2HSWORD" },
    {  205, L"MAGIC CANNON" },
    {  207, L"UNIQUE PANTS" },
    {  208, L"UNIQUE FIST" },
    {  209, L"NORMAL PANTS" },
    {  210, L"NORMAL FIST" },
    {  211, L"MAGIC PANTS" },
    {  212, L"MAGIC FIST" },
    {  223, L"MAGIC 2HAXE" },
    {  224, L"MAGIC 2HMACE" },
    {  226, L"1HMACE" },
    {  227, L"1HSWORD" },
    {  228, L"UNIQUE 1HSWORD" },
    {  229, L"UNIQUE 1HAXE" },
    {  230, L"UNIQUE 1HMACE" },
    {  231, L"MAGIC 1HSWORD" },
    {  233, L"MAGIC 1HMACE" },
    {  237, L"MAGIC 1HAXE" },
    {  238, L"NORMAL 1HAXE" },
    {  239, L"NORMAL 1HMACE" },
    {  240, L"NORMAL 1HSWORD" },
    {  241, L"LEVEL ITEM" },
    {  250, L"NORMAL NECKLACE" },
    {  251, L"NORMAL RING" },
    {  266, L"IDENTIFY SCROLL" },
    {  270, L"MAGIC SOCKETABLE" },
    {  271, L"BLOOD EMBER" },
    {  272, L"VOID EMBER" },
    {  273, L"IRON EMBER" },
    {  274, L"CHAOS EMBER" },
    {  278, L"HEALTHPOTION" },
    {  279, L"MANAPOTION" },
    {  285, L"LEGENDARY 1HAXE" },
    {  286, L"LEGENDARY 1HMACE" },
    {  287, L"LEGENDARY 1HSWORD" },
    {  288, L"LEGENDARY 2HAXE" },
    {  289, L"LEGENDARY 2HMACE" },
    {  290, L"LEGENDARY 2HSWORD" },
    {  291, L"LEGENDARY AXE" },
    {  292, L"LEGENDARY BELT" },
    {  293, L"LEGENDARY BOOTS" },
    {  294, L"LEGENDARY BOW" },
    {  295, L"LEGENDARY CHEST ARMOR" },
    {  296, L"LEGENDARY CROSSBOW" },
    {  297, L"LEGENDARY FIST" },
    {  298, L"LEGENDARY GLOVES" },
    {  299, L"LEGENDARY HELMET" },
    {  301, L"LEGENDARY NECKLACE" },
    {  302, L"LEGENDARY PANTS" },
    {  303, L"LEGENDARY PISTOL" },
    {  304, L"LEGENDARY POLEARM" },
    {  305, L"LEGENDARY RIFLE" },
    {  306, L"LEGENDARY RING" },
    {  307, L"LEGENDARY SHIELD" },
    {  308, L"LEGENDARY SHOULDER ARMOR" },
    {  309, L"LEGENDARY SOCKETABLE" },
    {  310, L"LEGENDARY STAFF" },
    {  312, L"LEGENDARY WAND" },
    {  313, L"LEGENDARY CANNON" },
    {  317, L"DYNAMITE" },
};
static const UtEntry* FindUt(int id) {         // 二分(kItemUnittypes 按 id 排序)
    int lo = 0, hi = (int)(sizeof(kItemUnittypes) / sizeof(kItemUnittypes[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kItemUnittypes[mid].id == id) return &kItemUnittypes[mid];
        if (kItemUnittypes[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}
static bool IsItemUnittype(int id) { return FindUt(id) != nullptr; }   // 是否该过滤的掉落物
static std::wstring UnittypeName(int id) {     // id→英文名(烤进 DLL 的 kItemUnittypes,id 稳定,无需读内存注册表)
    const UtEntry* e = FindUt(id);
    return e ? std::wstring(e->name) : std::wstring();
}

// hook sub_5577D0(背包初始化):原函数跑完后 this+0x5D0=新建的拾取管理器;若 this 是 CPlayer 则捕获它。
// __thiscall(this) 无栈参,用 __fastcall(ecx,edx) 接收(edx 占位,原函数不读)。
// ----------------------------------------------------------------------------
// 4) Detour:落地前读 CItem(GUID→名 / 等级@+0x110 / unittype ID@+0x1AC)→ 判定
//    → 否决(不转发原函数,物品永不落地)。非物品 unittype(道具/场景)直接跳过。
// ----------------------------------------------------------------------------
static int* __fastcall Detour_Place(void* ecx, void* edx, int unit, int pos, char flag) {
    if (IsCItem((void*)unit)) {
        uint8_t* item = (uint8_t*)unit;       // 单继承,unit 本身即 CItem*
        int ut = *(int*)(item + 0x1AC);
        if (IsItemUnittype(ut)) {             // 只处理真掉落物;大门/箱子/石棺 等跳过
            lf::DropInfo info;
            uint32_t glo = *(uint32_t*)(item + 0x1A0), ghi = *(uint32_t*)(item + 0x1A4);  // 物品 GUID
            info.name          = ItemNameByGuid(glo, ghi);     // 内存:GUID→单位定义节点→DISPLAYNAME
            info.level         = *(int*)(item + 0x110);
            info.unittype_id   = ut;
            info.unittype_name = UnittypeName(ut);
            info.unittype_disp = TranslateUnittype(info.unittype_name);  // zh:中文显示名;en:同原文
            info.tick          = GetTickCount();
            bool veto = lf::EvaluateDrop(info);   // 判定 + 写环形日志(UI 显示);logOnly 时永不 veto
            if (info.filtered)                    // 只记被过滤的(低噪声,审计用)
                lf::Log("FILTER lvl=%d ut=[%ls] name=[%ls]",
                        info.level, info.unittype_name.c_str(), info.name.c_str());
            if (veto) return (int*)unit;          // 不转发原函数 → 物品永不落地
        }
    }
    return oPlace(ecx, edx, unit, pos, flag);
}

// ----------------------------------------------------------------------------
// 5) d3d9 wrapper:运行时加载底层 d3d9(DXVK 改名优先,否则系统),转发导出
// ----------------------------------------------------------------------------
static HMODULE g_real = nullptr;
static FARPROC g_p[12] = {};
static const char* kExports[] = {
    "Direct3DCreate9","Direct3DCreate9Ex","Direct3DShaderValidatorCreate9",
    "D3DPERF_BeginEvent","D3DPERF_EndEvent","D3DPERF_GetStatus",
    "D3DPERF_QueryRepeatFrame","D3DPERF_SetMarker","D3DPERF_SetOptions",
    "D3DPERF_SetRegion","DebugSetMute","Direct3DCreate9On12",
};
static void LoadRealD3D9() {
    wchar_t path[MAX_PATH];
    if (GetFileAttributesW(L"d3d9_dxvk.dll") != INVALID_FILE_ATTRIBUTES)
        g_real = LoadLibraryW(L"d3d9_dxvk.dll");          // DXVK 用户改名而来
    if (!g_real) {                                         // 回退系统 d3d9
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\d3d9.dll");
        g_real = LoadLibraryW(path);
    }
    if (g_real)
        for (int i = 0; i < 12; ++i) g_p[i] = GetProcAddress(g_real, kExports[i]);
}

// ----------------------------------------------------------------------------
// 6) 初始化:解析引擎 + MinHook 掉落处理器 + 装 Present hook(见 overlay.cpp)
// ----------------------------------------------------------------------------
extern void InstallHookB(void** targets, int n, void* tdCItem);   // hook_b.cpp

static DWORD WINAPI InitThread(LPVOID) {
    ResolveEngine();            // AOB 扫描定位所有引擎函数/全局(跨版本)
    // UNITTYPE 英文名 = 烤进 DLL 的 kItemUnittypes(id 稳定,无需读内存注册表);
    // 中文名 = 按需查内存翻译表(TranslateUnittype→LookupTranslation),都无需预加载文件
    MH_STATUS s = MH_Initialize();
    lf::Log("MH_Initialize=%d", s);
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return 0;
    if (!pPlace || !g_tdCItem) {   // 关键签名没命中(不支持的 build)→ 安全退出,不挂任何 hook,不碰游戏
        lf::Log("FATAL: AOB 未命中 place=%p tdCItem=%p → 过滤禁用(不支持的游戏版本?)", pPlace, g_tdCItem);
        return 0;
    }
    MH_STATUS dh = MH_CreateHook((void*)pPlace, (void*)&Detour_Place, (void**)&oPlace);
    if (dh == MH_OK) MH_EnableHook((void*)pPlace);
    lf::Log("place hook create=%d @ %p", dh, pPlace);
    // Hook B:关卡存盘逐单位序列化(sub_5FF570 + 孪生 sub_5FF630),两处都挂(各带 CItem 守卫)。默认关。
    uint8_t* sers[4]; int nser = ScanAll(SIG_SERITEM, sers, 4);
    InstallHookB((void**)sers, nser, g_tdCItem);
    lf::Log("calling InstallPresentHook...");
    overlay::InstallPresentHook();   // dummy-device 取 Present vtable 槽并 hook,渲 ImGui
    lf::Log("InitThread done");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        lf::Log("=== DllMain attach ===");
        LoadRealD3D9();
        lf::Log("LoadRealD3D9: g_real=%p Direct3DCreate9=%p", g_real, g_p[0]);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    // DLL_PROCESS_DETACH:故意不做 MinHook 清理 —— 进程退出时在 loader lock 下
    // 挂起线程/卸 hook 会崩。代理 DLL 活到进程结束,交给 OS 回收即可。
    return TRUE;
}

// ---- 导出转发 stub(配 d3d9.def;运行时转发,兼容 DXVK/系统二选一)----
extern "C" {
IDirect3D9* WINAPI Wrap_Direct3DCreate9(UINT v) {
    using F = IDirect3D9*(WINAPI*)(UINT);
    return g_p[0] ? ((F)g_p[0])(v) : nullptr;
}
HRESULT WINAPI Wrap_Direct3DCreate9Ex(UINT v, IDirect3D9Ex** o) {
    using F = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    return g_p[1] ? ((F)g_p[1])(v, o) : E_FAIL;
}
// 其余 D3DPERF_*/DebugSetMute 等纯转发(裸 jmp),交给 .def 的导出别名或同名 stub。
}
