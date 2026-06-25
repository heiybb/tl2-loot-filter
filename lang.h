// lang.h — 编译期中英文切换。
//   默认中文;build 时传 /p:Lang=en(.vcxproj 据此定义 LF_EN)→ 英文。
//   用法:  LF_T("启用过滤", "Enable Filter")  —— 编译期选其一,零运行期开销。
//   注:宏名用 LF_T 而非裸 T —— 裸 T 会与 imgui.h 的 `p->~T()` 等模板 T 撞车(C4003)。
//   注:物品名/UNITTYPE 等来自游戏数据,始终按原文(可能中文),与 UI 语言无关。
#pragma once

#ifdef LF_EN
  #define LF_T(zh, en) (en)
#else
  #define LF_T(zh, en) (zh)
#endif
