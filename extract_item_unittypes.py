#!/usr/bin/env python3
# 提取 MEDIA/UNITS/ITEMS 下所有 .DAT 里出现的 UNITTYPE,与 UNITTYPES.HIE 全集对比,
# 区分「物品类型」(ITEMS 里用到的)vs「非物品类型」(道具/怪物等只在别处用的)。
# 给 loot filter 做白名单:只处理物品 unittype 的掉落,排除 大门/箱子/石棺 这类。
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")   # 避免 Windows 控制台 GBK 把中文表头打乱
except Exception:
    pass

GAME_DIR  = r"E:\Torchlight 2"
HIE_PATH  = os.path.join(GAME_DIR, "MEDIA", "UNITTYPES.HIE")

# 基础游戏 + 所有 mod 的 ITEMS 目录(mod 物品可能用到基础 ITEMS 没直接用的类型)
def item_dirs():
    dirs = [os.path.join(GAME_DIR, "MEDIA", "UNITS", "ITEMS")]
    mods = os.path.join(GAME_DIR, "mods")
    if os.path.isdir(mods):
        for m in os.listdir(mods):
            p = os.path.join(mods, m, "MEDIA", "UNITS", "ITEMS")
            if os.path.isdir(p):
                dirs.append(p)
    return [d for d in dirs if os.path.isdir(d)]


def read_utf16(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-16", errors="replace")  # 自动识别 BOM


def extract_item_unittypes(dirs):
    found = {}  # UNITTYPE(大写) -> 出现次数
    n_dat = 0
    for items_dir in dirs:
        for root, _, files in os.walk(items_dir):
            for fn in files:
                if not fn.lower().endswith(".dat"):
                    continue
                n_dat += 1
                try:
                    text = read_utf16(os.path.join(root, fn))
                except Exception:
                    continue
                for line in text.splitlines():
                    i = line.find("UNITTYPE:")            # <STRING>UNITTYPE:XXX
                    if i >= 0:
                        ut = line[i + 9:].strip()
                        if ut:
                            found[ut.upper()] = found.get(ut.upper(), 0) + 1
    return found, n_dat


def parse_hie(hie_path):
    all_uts = {}  # 段名(大写) -> id
    sect = None
    for line in read_utf16(hie_path).splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]") and not s.startswith("[/"):
            sect = s[1:-1]
        elif sect and "<INTEGER>ID:" in line:
            try:
                all_uts[sect.upper()] = int(line.split("ID:")[1].strip())
            except ValueError:
                pass
    return all_uts


def print_rarity_groups(all_uts):
    # 稀有度前缀(LEGENDARY 优先于 MAGIC;RANDOMMAGIC 优先于 MAGIC,避免误匹配)
    order = ["RANDOMMAGIC", "LEGENDARY", "UNIQUE", "MAGIC", "NORMAL", "RUNE"]
    groups = {r: {} for r in order}
    base = {}
    for name, id_ in all_uts.items():
        for r in order:
            if name.startswith(r):
                sub = name[len(r):].strip().lstrip("_ ").strip() or "(本身)"
                groups[r][sub] = id_
                break
        else:
            base[name] = id_

    print("\n=== 按稀有度前缀分组(UNITTYPES.HIE 全集)===")
    for r in order:
        print(f"\n[{r}] {len(groups[r])} 个子类型:  " +
              ", ".join(sorted(groups[r])))

    # 覆盖矩阵:装备子类型 × 稀有度
    rar = ["NORMAL", "MAGIC", "UNIQUE", "LEGENDARY"]
    subs = sorted(set().union(*(set(groups[r]) for r in rar)))
    print(f"\n=== 覆盖矩阵({len(subs)} 个装备子类型 × 4 主稀有度)===")
    print("子类型".ljust(22) + "".join(r.ljust(11) for r in rar))
    for s in subs:
        print(s.ljust(20) + "".join(("  YES" if s in groups[r] else "   - ").ljust(11) for r in rar))


def main():
    dirs = item_dirs()
    print("扫描目录:")
    for d in dirs:
        print(f"  {d}")
    print()
    item_uts, n_dat = extract_item_unittypes(dirs)
    all_uts = parse_hie(HIE_PATH)

    used     = sorted(k for k in all_uts if k in item_uts)
    unused   = sorted(k for k in all_uts if k not in item_uts)
    not_hie  = sorted(k for k in item_uts if k not in all_uts)

    print(f"扫描 {n_dat} 个 .DAT;HIE 共 {len(all_uts)} 个 unittype\n")

    print(f"=== 物品类型(ITEMS 里用到, {len(used)} 个)===")
    for k in used:
        print(f"  id {all_uts[k]:>4}  {k}  (x{item_uts[k]})")

    print(f"\n=== 非物品类型(HIE 有、ITEMS 没用到, {len(unused)} 个)===")
    for k in unused:
        print(f"  id {all_uts[k]:>4}  {k}")

    if not_hie:
        print(f"\n=== ITEMS 里出现但 HIE 没有的 UNITTYPE({len(not_hie)} 个,异常)===")
        for k in not_hie:
            print(f"  {k}  (x{item_uts[k]})")

    # 方便贴进 C++ 的白名单(物品 unittype {id, name};供 IsItemUnittype/UnittypeName 二分)
    # 名字烤进 DLL → 不必运行时读内存注册表(装备 UNITTYPE 是基础游戏固定定义,id 稳定)
    print(f"\n=== C++ 白名单(物品 unittype {{id, name}})===")
    ids = sorted(all_uts[k] for k in used)
    id2name = {v: k for k, v in all_uts.items()}
    print("struct UtEntry { int id; const wchar_t* name; };")
    print("static const UtEntry kItemUnittypes[] = {  // {id, 名};按 id 排序")
    for i in ids:
        nm = id2name.get(i, '?').replace('"', '')
        print(f'    {{ {i:>4}, L"{nm}" }},')
    print("};")

    print_rarity_groups(all_uts)


if __name__ == "__main__":
    main()
