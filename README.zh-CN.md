> [English](README.md) | 中文

# TL2 Loot Filter (tl2-loot-filter)

Torchlight 2 掉落过滤器 —— 注入式 `d3d9.dll` 代理 DLL,带 ImGui 界面。
落地前否决垃圾掉落(垃圾直接遁入虚空),并提供一个可调的过滤面板 + 最近掉落日志。
中/英双语编译版,逆向自 `Torchlight2.exe` 1.25.9.5(AOB 签名跨版本,Steam 1.25.5.6 亦验证)。

> 个人单机用途。**主动过滤是单机/房主专用**;联机请只用观察模式(`logOnly` 开、其余关),否则可能闪退。

> 💡 **强烈推荐配合 [DXVK](https://github.com/doitsujin/dxvk) 使用。** 本过滤器不依赖它也能跑,但 TL2 原生
> D3D9 在现代显卡(尤其 NVIDIA RTX)上很容易崩;DXVK(D3D9 → Vulkan)能消除大部分这类闪退,且 ImGui
> 覆盖层在它上面运行良好。装法:把 DXVK 的 `d3d9.dll` 改名为 `d3d9_dxvk.dll`,即可与本过滤器共存
> (见 *安装 / 使用* 第 4 步)。

## 它能做什么

- **落地前过滤**:hook 掉落落地点(`sub_5EAA80`),命中规则的掉落直接不生成。
  - 按 **UNITTYPE** 屏蔽(如 `MAGIC SOCKETABLE`、`POTION`、`SCROLL`……)(尚未完全测试)
  - 按 **名称关键字** 屏蔽(子串,大小写不敏感)(尚未完全测试)
  - 按 **稀有度** 屏蔽(白/绿+蓝/金/红/紫)
  - 按 **等级下限** 屏蔽
  - **安全开关 `logOnly`**:只记录不真删,先观察确认无误再开真过滤
- **ImGui 覆盖层**(`Home` 键开关):规则编辑 + 「最近掉落」表(名称/等级/UNITTYPE/状态),
  支持 TL2 富文本颜色标签 `|cAARRGGBB…|u` 渲染。
- **中文名**:zh 版加载系统中文字体,UNITTYPE + 物品名显示中文。
- **Hook B(可选,实验)**:离开关卡时不保存上一层地面掉落(`clearOnTransition`),重访即清空。

## 怎么工作的(关键设计)

- **注入**:`d3d9.dll` 代理 + [MinHook](https://github.com/TsudaKageyu/minhook)。运行时按序加载底层 d3d9:
  先找游戏目录的 `d3d9_dxvk.dll`(DXVK 用户改名而来),**没有则回退系统原生 `d3d9.dll`**。
  → **不依赖 DXVK**;`d3d9_dxvk.dll` 那层只是为了和 DXVK 共存(双方都想叫 d3d9.dll)。
- **引擎定位**:全部 **AOB(字节特征码)扫描**,不写死 RVA(`hooks.cpp` `ResolveEngine`),跨版本通用。
  关键签名没命中(不支持的版本)→ 安全退出,不挂任何 hook。
- **从内存读,不读解包 MEDIA 文件**(普通玩家的 MEDIA 在 PAK/MOD 里,没解包):
  - 物品名/等级/unittype id:从游戏内存的 CItem 读(GUID→DISPLAYNAME 等)。
  - **UNITTYPE id→英文名**:`kItemUnittypes` 表(`extract_item_unittypes.py` 生成;
    装备 UNITTYPE 是基础游戏固定定义、id 稳定,无需运行时读内存)。
  - **中文翻译**:直接读游戏内存的翻译表(`CStringTranslate`),反映 mod 真实生效的覆盖顺序。

## 文件

| 文件 | 作用 |
|---|---|
| `hooks.cpp` | d3d9 代理 + AOB 引擎定位 + 掉落 detour + 内存读取(unittype/翻译) |
| `overlay.cpp` | ImGui 覆盖层(Present hook、面板、颜色标签渲染、中文字体) |
| `loot_filter.cpp/.h` | 过滤规则 + 判定 + 掉落日志(共享状态) |
| `hook_b.cpp` | Hook B(离开关卡不持久化地面掉落) |
| `lang.h` | 编译期 i18n 宏 `LF_T(zh, en)`(`/DLF_EN` 选英文) |
| `d3d9.def` | 导出表(代理转发) |
| `extract_item_unittypes.py` | 生成 `kItemUnittypes` 白名单(扫 ITEMS + UNITTYPES.HIE) |
| `third_party/` | imgui + minhook(vendored) |

## 构建

需要 **MSVC(VS 2022/v143)+ Win32**。工程会**同时编 zh + en 两版**,并把 `DeployLang`(默认 en)那版复制进游戏目录。

```bat
:: 用 vswhere 找 MSBuild 后:
msbuild tl2_loot_filter.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /p:DeployLang=zh
```

- 产物:`bin\Release-zh\d3d9.dll`、`bin\Release-en\d3d9.dll`。
- **部署目标**可覆盖:`/p:GameDir="你的TL2目录"`(默认 `E:\Torchlight 2\`;目录不存在自动跳过部署)。
- 语言选择:`/p:DeployLang=zh`(中文)或 `en`(英文)。

## 安装 / 使用

1. 把要用的那版 `d3d9.dll`(zh 或 en)放进 **游戏根目录**(`Torchlight2.exe` 同级)。
2. 启动游戏,**`Home` 键** 开关过滤面板。
3. 先开 `logOnly` 观察「最近掉落」确认判定无误,再关 `logOnly` 启用真过滤。
4. **DXVK(强烈推荐)**:把 DXVK 的 `d3d9.dll` 改名为 `d3d9_dxvk.dll`,和本过滤器的 `d3d9.dll` 一起放进
   游戏根目录 —— 代理会自动链上它。能大幅提升现代 / NVIDIA 显卡上的稳定性。

## 安全须知

- 主动过滤(否决落地)= **单机 / 房主专用**。联机客户端开真过滤无意义且可能 desync。
- 关键 AOB 没命中(不支持的游戏版本)→ DLL 自动不挂 hook、不碰游戏。
- 所有内存读取均包 SEH(`__try`),坏指针不崩。
