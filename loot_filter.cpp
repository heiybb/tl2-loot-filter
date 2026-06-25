// loot_filter.cpp — 过滤判定 + 日志
#include "loot_filter.h"
#include <algorithm>
#include <cwctype>
#include <cstdarg>
#include <cstdio>

namespace lf {

State& G() { static State s; return s; }

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = nullptr;
    fopen_s(&f, "tl2_loot_filter.log", "a");
    if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
}

std::wstring ToUpper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c){ return (wchar_t)std::towupper(c); });
    return s;
}

bool ContainsCI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    auto H = ToUpper(hay), N = ToUpper(needle);
    return H.find(N) != std::wstring::npos;
}

bool EvaluateDrop(DropInfo& d) {
    auto& s = G();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.totalSeen++;

    bool block = false;
    if (s.cfg.enabled) {
        // UNITTYPE 黑名单(按注册表反查出的类型名子串比对,大小写不敏感)
        if (!d.unittype_name.empty()) {
            for (auto& b : s.cfg.blockUnittypes)
                if (ContainsCI(d.unittype_name, b)) { block = true; break; }
        }
        // 稀有度过滤(unittype 名前缀,如 "LEGENDARY PISTOL";RANDOMMAGIC 不会被 MAGIC 误伤)
        if (!block && !d.unittype_name.empty()) {
            std::wstring U = ToUpper(d.unittype_name);
            auto pre = [&](const wchar_t* p) { return U.rfind(p, 0) == 0; };
            if ((s.cfg.blockNormal    && pre(L"NORMAL"))    ||
                (s.cfg.blockMagic     && pre(L"MAGIC"))     ||  // 绿+蓝
                (s.cfg.blockUnique    && pre(L"UNIQUE"))    ||
                (s.cfg.blockLegendary && pre(L"LEGENDARY")) ||
                (s.cfg.blockQuest     && pre(L"QUEST")))        // QUESTITEM
                block = true;
        }
        // 名称关键字
        if (!block)
            for (auto& kw : s.cfg.blockNameContains)
                if (ContainsCI(d.name, kw)) { block = true; break; }
        // 等级下限
        if (!block && s.cfg.useLevelFloor && s.cfg.levelFloor > 0 && d.level > 0
            && d.level < s.cfg.levelFloor)
            block = true;
    }

    d.filtered = block;
    if (block) s.totalFiltered++;

    // 写环形日志
    s.log.push_back(d);
    while (s.log.size() > s.logCap) s.log.pop_front();

    // logOnly 模式:只记录,不真删
    return block && !s.cfg.logOnly;
}

} // namespace lf
