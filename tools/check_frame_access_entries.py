#!/usr/bin/env python3
"""フレーム内の番地参照が **3 つの入口だけ**を通っていることを検査する。

## なぜ要るか

案 d 第 3 段(フレームの寸法を関数ごとの実寸にする)は、
**「この関数が触りうる最も高い位置」を出力時に数えて寸法を決める**。
数えているのは `jit_emit_rsp_disp32` ただ 1 箇所で、そこへは

    za_load_slot / za_store_slot / za_addr_of_slot

の 3 つの入口からしか来ない、という前提に乗っている。

**この前提が破れると、水位に穴が開く。** 数えられなかった参照より低い寸法で確保し、
**ビルドは通って実行時に静かにスタックが壊れる**(漏れた側のスロットは、
隣の関数のフレームや呼び出し元のデータを指す)。

検出器が無いと、`[rsp+disp]` を出す新しい経路を足した人には何も起きない。
だから**「3 入口以外に [rsp+disp] を出す経路が無いこと」を機械で見る**。

## 何を見るか

1. 低レベルの出力器(`jit_mov_reg_from_rsp` / `jit_mov_rsp_from_reg` / `jit_lea_reg_rsp`)を
   呼んでいるのは、**3 つの入口の定義行だけ**であること
2. `jit_emit_rsp_disp32` を呼んでいるのは、**上の低レベル出力器 3 つだけ**であること
3. 水位を更新している箇所が `jit_emit_rsp_disp32` の中に**ちょうど 1 つ**あること

**原則 12 を踏まえ、コメントの文字列は判定材料にしない。** 見るのは呼び出しだけである。

## 使い方

    python3 tools/check_frame_access_entries.py
    python3 tools/check_frame_access_entries.py --self-test
"""

import os
import re
import sys
import tempfile

ZA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "c", "za.c")

#: 3 つの入口。これ以外から低レベルの出力器を呼んではいけない
ENTRIES = ("za_load_slot", "za_store_slot", "za_addr_of_slot")
#: 低レベルの出力器。これ以外から jit_emit_rsp_disp32 を呼んではいけない
LOWLEVEL = ("jit_mov_reg_from_rsp", "jit_mov_rsp_from_reg", "jit_lea_reg_rsp")
#: 水位を更新する変数
HWM = "g_za_frame_hwm"


def callers_of(lines, name, defined_by):
    """name を呼んでいる行のうち、defined_by のどれの定義行でもないものを返す。

    定義行の判定は「その識別子が `{` より前にあること」。`[^;]*` のように `{` を
    またぐと、`static void bad(void) { jit_mov_reg_from_rsp(...); }` を定義行と
    誤認して**迂回を見逃す**(2026-10-30 に self-test が捕まえた)。
    """
    call = re.compile(r"\b%s\s*\(" % re.escape(name))
    decl = re.compile(r"^\s*static\s[^;{]*\b(%s)\s*\(" % "|".join(re.escape(d) for d in defined_by))
    out = []
    for i, line in enumerate(lines, 1):
        if not call.search(line):
            continue
        if decl.search(line):
            continue          # 入口自身の定義行(= 1 行定義の中の呼び出し)
        if re.match(r"^\s*static\s[^;{]*\b%s\s*\(" % re.escape(name), line):
            continue          # name 自身の定義行
        out.append((i, line.rstrip()))
    return out


def check(path=ZA, quiet=False):
    out = (lambda *a: None) if quiet else (lambda *a: print(*a))
    lines = open(path, encoding="utf-8").read().split("\n")
    bad = 0

    for low in LOWLEVEL:
        extra = callers_of(lines, low, ENTRIES)
        if extra:
            bad += len(extra)
            print("NG: %s を 3 入口の外から呼んでいる (%d 箇所)" % (low, len(extra)),
                  file=sys.stderr)
            for ln, text in extra[:5]:
                print("    %s:%d: %s" % (path, ln, text.strip()), file=sys.stderr)
        else:
            out("OK: %-22s を呼ぶのは 3 入口の定義だけ" % low)

    extra = callers_of(lines, "jit_emit_rsp_disp32", LOWLEVEL)
    if extra:
        bad += len(extra)
        print("NG: jit_emit_rsp_disp32 を低レベル出力器の外から呼んでいる (%d 箇所)"
              % len(extra), file=sys.stderr)
        for ln, text in extra[:5]:
            print("    %s:%d: %s" % (path, ln, text.strip()), file=sys.stderr)
    else:
        out("OK: jit_emit_rsp_disp32 を呼ぶのは低レベル出力器 3 つだけ")

    # 水位の更新がちょうど 1 箇所で、しかも jit_emit_rsp_disp32 の中にあること
    body_start = next((i for i, l in enumerate(lines)
                       if re.match(r"^\s*static\s+void\s+jit_emit_rsp_disp32\s*\(", l)), None)
    if body_start is None:
        print("NG: jit_emit_rsp_disp32 の定義が見つからない", file=sys.stderr)
        return 1
    body_end = next((i for i in range(body_start, len(lines)) if lines[i].rstrip() == "}"),
                    len(lines))
    # **宣言行だけを除く。** `static` を含むかどうかで除いてはいけない
    # (`static void f(void) { g_za_frame_hwm = 4096; }` のような 1 行関数を見逃す。
    #  2026-10-30 に self-test が実際にこれを捕まえた)
    decl_re = re.compile(r"^\s*static\s+\w+\s+%s\s*=" % re.escape(HWM))
    assigns = [i + 1 for i, l in enumerate(lines)
               if re.search(r"\b%s\s*=[^=]" % re.escape(HWM), l) and not decl_re.match(l)]
    inside = [ln for ln in assigns if body_start < ln <= body_end + 1]
    outside = [ln for ln in assigns if ln not in inside]
    # リセット(コンパイル開始時に 0 を入れる)は外にあってよい。0 代入だけを許す
    outside_bad = [ln for ln in outside
                   if not re.search(r"\b%s\s*=\s*0\s*;" % re.escape(HWM), lines[ln - 1])]
    if len(inside) != 1:
        bad += 1
        print("NG: 水位の更新が jit_emit_rsp_disp32 の中に %d 箇所ある(1 箇所であること)"
              % len(inside), file=sys.stderr)
    else:
        out("OK: 水位の更新は jit_emit_rsp_disp32 の中の 1 箇所だけ")
    if outside_bad:
        bad += len(outside_bad)
        print("NG: 水位を外から書き換えている (%d 箇所。0 のリセットだけが許される)"
              % len(outside_bad), file=sys.stderr)
        for ln in outside_bad[:5]:
            print("    %s:%d: %s" % (path, ln, lines[ln - 1].strip()), file=sys.stderr)

    if bad:
        print("", file=sys.stderr)
        print("**3 入口の外からフレームに触ると、寸法の水位に穴が開く。**", file=sys.stderr)
        print("足したい参照は 3 入口のどれかを通すこと(通せない理由があるなら、"
              "水位の数え方を先に設計し直すこと)。", file=sys.stderr)
        return 1
    out("")
    out("OK: フレーム内の番地参照は 3 入口だけを通り、水位は 1 箇所で数えている")
    return 0


SELF_OK = """static void jit_emit_rsp_disp32(UINT8 opcode, UINT8 reg, INT32 disp32) {
    jit_emit8(0x24);
    if (disp32 >= 0 && (UINT32)disp32 + 8 > g_za_frame_hwm) {
        g_za_frame_hwm = (UINT32)disp32 + 8;
    }
    jit_emit32((UINT32)disp32);
}
static void jit_lea_reg_rsp(UINT8 reg, INT32 d) { jit_emit_rsp_disp32(0x8D, reg, d); }
static void jit_mov_reg_from_rsp(UINT8 reg, INT32 d) { jit_emit_rsp_disp32(0x8B, reg, d); }
static void jit_mov_rsp_from_reg(UINT8 reg, INT32 d) { jit_emit_rsp_disp32(0x89, reg, d); }
static void za_load_slot(UINT8 reg, UINT32 off) { jit_mov_reg_from_rsp(reg, (INT32)off); }
static void za_store_slot(UINT8 reg, UINT32 off) { jit_mov_rsp_from_reg(reg, (INT32)off); }
static void za_addr_of_slot(UINT8 reg, UINT32 off) { jit_lea_reg_rsp(reg, (INT32)off); }
static void za_reset(void) { g_za_frame_hwm = 0; }
"""


def self_test():
    """この検出器自身の陽性対照・陰性対照(規則 8)。"""
    print("[self-test] フレーム参照の 3 入口の検査")
    ok = True

    def run(label, text, want_rc):
        nonlocal ok
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "za.c")
            open(fp, "w").write(text)
            rc = check(fp, quiet=True)
        if rc == want_rc:
            print("OK(%s): rc=%d (期待 %d)" % (label, rc, want_rc))
        else:
            print("FAIL(%s): rc=%d。期待は %d" % (label, rc, want_rc), file=sys.stderr)
            ok = False

    run("陰性対照・3 入口だけ", SELF_OK, 0)
    run("陽性対照1・低レベル出力器を直接呼んだ",
        SELF_OK + "static void bad(void) { jit_mov_reg_from_rsp(1, 16); }\n", 1)
    run("陽性対照2・合流点を直接呼んだ",
        SELF_OK + "static void bad(void) { jit_emit_rsp_disp32(0x8B, 1, 16); }\n", 1)
    run("陽性対照3・水位の更新を消した",
        SELF_OK.replace("        g_za_frame_hwm = (UINT32)disp32 + 8;\n", ""), 1)
    run("陽性対照4・水位を外から書き換えた",
        SELF_OK + "static void bad(void) { g_za_frame_hwm = 4096; }\n", 1)

    if ok:
        print("self-test 成功: 3 入口の迂回・合流点の直接呼び出し・水位の消失・"
              "水位の外からの書き換えのすべてで落ちることを確かめた")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv[1:] else check())
