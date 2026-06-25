// overlay.cpp — ImGui d3d9 覆盖层:dummy-device 取 Present 槽 → hook → 渲面板
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include "loot_filter.h"
#include "lang.h"
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {

using EndScene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
static EndScene_t oEndScene = nullptr;
static WNDPROC    oWndProc = nullptr;
static HWND       g_hwnd   = nullptr;
static bool       g_init   = false;
bool g_inited = false;

// 工具:窄字转给 ImGui 显示
static std::string WtoU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static inline int HexNib(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// TL2 富文本颜色标签(实测自 MEDIA/UNITS/ITEMS):|cAARRGGBB 开色、|u 复位。
//   如 "|cFFCC0066史诗的|u银质护手" → "史诗的"上色、"银质护手"默认色。
//   颜色可不复位直接到结尾(如 "|cFFF5DB94你的佣兵..." 后接其它字段)。复位符是 |u 不是 |r。
//   hex 大小写均可;alpha 字节(首字节)忽略 —— 实测有 |c00FF3030 这种 alpha=00 仍要可见的红字。
// 按颜色把字符串切段、同一行内联渲染;无标签时等价 TextUnformatted。
static void RenderColorTagged(const std::wstring& w) {
    if (w.empty()) { ImGui::TextUnformatted(""); return; }
    const ImVec4 defCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 cur = defCol;
    std::wstring run;
    bool any = false;                    // 已经渲染过至少一段?(决定要不要 SameLine)
    auto flush = [&]() {
        if (run.empty()) return;
        if (any) ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(cur, "%s", WtoU8(run).c_str());
        run.clear();
        any = true;
    };
    size_t i = 0, n = w.size();
    while (i < n) {
        if (w[i] == L'|' && i + 1 < n) {
            wchar_t t = w[i + 1];
            if ((t == L'c' || t == L'C') && i + 9 < n) {        // |c + 8 hex
                int h[8], k; bool ok = true;
                for (k = 0; k < 8; ++k) { h[k] = HexNib(w[i + 2 + k]); if (h[k] < 0) { ok = false; break; } }
                if (ok) {
                    flush();
                    int r = (h[2] << 4) | h[3], g = (h[4] << 4) | h[5], b = (h[6] << 4) | h[7];
                    cur = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
                    i += 10;
                    continue;
                }
            } else if (t == L'u' || t == L'U') {                 // |u 复位(TL2 的终止符)
                flush();
                cur = defCol;
                i += 2;
                continue;
            }
        }
        run += w[i++];
    }
    flush();
    if (!any) ImGui::TextUnformatted("");    // 整串都是标签 → 仍占一行(表格单元格不塌)
}

static LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (lf::G().uiVisible && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) {
        // UI 可见时吞掉鼠标/键盘,避免传给游戏
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MOUSEMOVE ||
            m == WM_KEYDOWN || m == WM_CHAR) return TRUE;
    }
    if (m == WM_KEYDOWN && w == VK_HOME) { lf::G().uiVisible = !lf::G().uiVisible; return TRUE; }
    return CallWindowProcW(oWndProc, h, m, w, l);
}

// ---- 面板 ----
static char s_utBuf[64] = "MAGIC SOCKETABLE";
static char s_nmBuf[64] = "";

static void DrawPanel() {
    auto& st = lf::G();
    if (!st.uiVisible) return;
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(LF_T("TL2 Loot Filter  (Home 隐藏)", "TL2 Loot Filter  (Home to hide)"))) {
        std::lock_guard<std::mutex> lk(st.mtx);

        ImGui::Checkbox(LF_T("启用过滤", "Enable Filter"), &st.cfg.enabled);
        ImGui::SameLine();
        ImGui::Checkbox(LF_T("仅记录不删 (logOnly)", "Log Only (no remove)"), &st.cfg.logOnly);
        ImGui::Checkbox(LF_T("离开关卡不保存地面掉落(重访即清空)",
                          "Don't keep ground loot on leave (clears on revisit)"), &st.cfg.clearOnTransition);
        ImGui::Text(LF_T("已见 %llu  |  命中 %llu", "Seen %llu  |  Filtered %llu"),
                    (unsigned long long)st.totalSeen, (unsigned long long)st.totalFiltered);
        ImGui::Separator();

        // —— 按稀有度隐藏(匹配 unittype 名前缀,如 "LEGENDARY PISTOL")——
        ImGui::TextUnformatted(LF_T("隐藏稀有度:", "Hide rarity:"));
        ImGui::SameLine(); ImGui::Checkbox(LF_T("白",    "White"),   &st.cfg.blockNormal);
        ImGui::SameLine(); ImGui::Checkbox(LF_T("绿+蓝", "Grn+Blu"), &st.cfg.blockMagic);
        ImGui::SameLine(); ImGui::Checkbox(LF_T("金",    "Gold"),    &st.cfg.blockUnique);
        ImGui::SameLine(); ImGui::Checkbox(LF_T("红",    "Red"),     &st.cfg.blockLegendary);
        ImGui::SameLine(); ImGui::Checkbox(LF_T("紫",    "Purple"),  &st.cfg.blockQuest);
        ImGui::Separator();

        // —— UNITTYPE 黑名单 ——
        ImGui::TextUnformatted(LF_T("按 UNITTYPE 屏蔽 (如 MAGIC SOCKETABLE / POTION):",
                                 "Block by UNITTYPE (e.g. MAGIC SOCKETABLE / POTION):"));
        ImGui::InputText("##ut", s_utBuf, sizeof(s_utBuf));
        ImGui::SameLine();
        if (ImGui::Button(LF_T("加##ut", "Add##ut")) && s_utBuf[0]) {
            wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, s_utBuf, -1, w, 64);
            st.cfg.blockUnittypes.push_back(w); s_utBuf[0] = 0;   // 仅存字符串;不调游戏函数(避免崩溃)
        }
        for (size_t i = 0; i < st.cfg.blockUnittypes.size(); ++i) {
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("x")) { st.cfg.blockUnittypes.erase(st.cfg.blockUnittypes.begin()+i); ImGui::PopID(); break; }
            ImGui::SameLine();
            ImGui::TextUnformatted(WtoU8(st.cfg.blockUnittypes[i]).c_str());
            ImGui::PopID();
        }
        ImGui::Separator();

        // —— 名称关键字 ——
        ImGui::TextUnformatted(LF_T("按名称关键字屏蔽:", "Block by name keyword:"));
        ImGui::InputText("##nm", s_nmBuf, sizeof(s_nmBuf));
        ImGui::SameLine();
        if (ImGui::Button(LF_T("加##nm", "Add##nm")) && s_nmBuf[0]) {
            wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, s_nmBuf, -1, w, 64);
            st.cfg.blockNameContains.push_back(w); s_nmBuf[0] = 0;
        }
        for (size_t i = 0; i < st.cfg.blockNameContains.size(); ++i) {
            ImGui::PushID(1000+(int)i);
            if (ImGui::SmallButton("x")) { st.cfg.blockNameContains.erase(st.cfg.blockNameContains.begin()+i); ImGui::PopID(); break; }
            ImGui::SameLine(); ImGui::TextUnformatted(WtoU8(st.cfg.blockNameContains[i]).c_str());
            ImGui::PopID();
        }
        ImGui::Separator();

        // —— 等级下限 ——
        ImGui::Checkbox(LF_T("按等级下限屏蔽", "Block below level"), &st.cfg.useLevelFloor);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt(LF_T("等级 <", "Level <"), &st.cfg.levelFloor);
        ImGui::Separator();

        // —— 掉落日志 ——
        ImGui::TextUnformatted(LF_T("最近掉落:", "Recent drops:"));
        if (ImGui::BeginTable("drops", 4, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                              ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
            ImGui::TableSetupColumn(LF_T("名称", "Name")); ImGui::TableSetupColumn(LF_T("等级", "Lv"), ImGuiTableColumnFlags_WidthFixed, 44);
            ImGui::TableSetupColumn("UNITTYPE"); ImGui::TableSetupColumn(LF_T("状态", "Status"), ImGuiTableColumnFlags_WidthFixed, 56);
            ImGui::TableHeadersRow();
            for (auto it = st.log.rbegin(); it != st.log.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); RenderColorTagged(it->name);   // 物品名也带稀有度颜色标签
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", it->level);
                ImGui::TableSetColumnIndex(2);
                {   // 显示用译名(zh:中文;en:原文);为空再退回原文/ID
                    const std::wstring& ut = !it->unittype_disp.empty() ? it->unittype_disp : it->unittype_name;
                    if (!ut.empty()) RenderColorTagged(ut);                    // 译名可能含 |cAARRGGBB...|r
                    else ImGui::Text("id %d", it->unittype_id);
                }
                ImGui::TableSetColumnIndex(3);
                if (it->filtered) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), LF_T("过滤", "Filtered"));
                else              ImGui::TextColored(ImVec4(0.5f,0.9f,0.5f,1), LF_T("保留", "Kept"));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

static HRESULT __stdcall HookEndScene(IDirect3DDevice9* dev) {
    // 只在"当前渲染目标 == 主 backbuffer"那次 EndScene 渲 UI(OGRE 一帧多次 EndScene)
    bool isMain = false;
    IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* bb = nullptr;
    if (dev->GetRenderTarget(0, &rt) == D3D_OK && rt &&
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) == D3D_OK && bb)
        isMain = (rt == bb);
    if (rt) rt->Release();
    if (bb) bb->Release();

    static bool logged = false;
    if (!logged) { logged = true; lf::Log("HookEndScene called, isMain=%d", isMain); }

    if (isMain) {
        if (!g_init) {
            lf::Log("HookEndScene FIRST main-RT call, dev=%p", dev);
            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            ImGuiIO& io = ImGui::GetIO();
#ifndef LF_EN
            // 中文字形:仅 zh build 才引用系统中文字体(en 版全英文,用内置默认字体,绝不碰系统字体文件)。
            // 不写死 C:\Windows —— 从 %WINDIR% 拼路径(Windows 不一定装在 C 盘);
            // 候选字体全缺失/加载失败时回落到 ImGui 内置默认字体,避免空 atlas 崩溃/空白面板。
            wchar_t winDir[MAX_PATH] = {0};
            ImFont* cnFont = nullptr;
            if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
                // GetGlyphRanges* 返回的指针必须在 atlas Build 前保持有效 → 用 static
                static const ImWchar* glyphs = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
                const wchar_t* candidates[] = { L"\\Fonts\\msyh.ttc", L"\\Fonts\\msyh.ttf",
                                                L"\\Fonts\\simhei.ttf", L"\\Fonts\\simsun.ttc" };
                for (const wchar_t* rel : candidates) {
                    wchar_t fontPath[MAX_PATH];
                    if (_snwprintf_s(fontPath, _TRUNCATE, L"%s%s", winDir, rel) < 0) continue;
                    if (GetFileAttributesW(fontPath) == INVALID_FILE_ATTRIBUTES) continue;
                    // ImGui 只认窄字符路径;把 UTF-16 路径转 UTF-8 再喂进去
                    char fontPathA[MAX_PATH * 2] = {0};
                    if (WideCharToMultiByte(CP_UTF8, 0, fontPath, -1, fontPathA,
                                            sizeof(fontPathA), nullptr, nullptr) == 0) continue;
                    cnFont = io.Fonts->AddFontFromFileTTF(fontPathA, 18.0f, nullptr, glyphs);
                    if (cnFont) { lf::Log("loaded CJK font: %s", fontPathA); break; }
                }
            }
            if (!cnFont) {
                lf::Log("WARN: no CJK system font found, using ImGui default (CJK shows as boxes)");
                io.Fonts->AddFontDefault();
            }
#else
            io.Fonts->AddFontDefault();   // en build:仅英文,内置默认字体即可,不引用系统字体
#endif
            D3DDEVICE_CREATION_PARAMETERS p{};
            dev->GetCreationParameters(&p);
            g_hwnd = p.hFocusWindow;
            oWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX9_Init(dev);
            lf::Log("ImGui init done, hwnd=%p", g_hwnd);
            g_init = g_inited = true;
        }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawPanel();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    return oEndScene(dev);
}

// dummy-device 取 Present(vtable[17])并 MinHook
void InstallPresentHook() {
    HMODULE d3d = GetModuleHandleW(L"d3d9.dll");
    auto Create = d3d ? (decltype(&Direct3DCreate9))GetProcAddress(d3d, "Direct3DCreate9") : nullptr;
    IDirect3D9* d9 = Create ? Create(D3D_SDK_VERSION) : nullptr;
    lf::Log("InstallPresentHook: d3d9 mod=%p Create=%p d9=%p", d3d, Create, d9);
    if (!d9) return;

    WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"lf_dummy";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0,0,1,1, nullptr,nullptr,wc.hInstance,nullptr);

    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hw; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw,
                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    lf::Log("CreateDevice hr=0x%08lX dev=%p", hr, dev);
    if (dev) {
        void** vt = *(void***)dev;                 // 设备 vtable
        MH_STATUS mh = MH_CreateHook(vt[42], (void*)&HookEndScene, (void**)&oEndScene);  // 槽42 = EndScene
        MH_EnableHook(vt[42]);
        lf::Log("EndScene vt[42]=%p hook MH=%d", vt[42], mh);
        dev->Release();
    }
    d9->Release();
    DestroyWindow(hw);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

} // namespace overlay
