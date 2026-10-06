#!/usr/bin/env python3
"""[調査] JIT 生成コードのスタックフレーム(ZA_FRAME_TOTAL)の内訳を出す。

documents/jit-frame-survey.md。

**なぜ要るか。** ZA_FRAME_TOTAL は JIT 関数すべてのフレームで、
**JIT 関数の再帰深さの上限をそのまま決めている**(256KB / 7480 = 35 段。
実測の「深さ 33 で返る / 34 で STACK OVERFLOW」と一致)。
PR #118 で `let` スロットが 1536 byte だと分かったが、**残りは未検査だった。**

**手法**: src/c/za.c からフレームレイアウトの #define を切り出して C へ渡し、
プリプロセッサに計算させる(PR #118 と同じ)。手計算しない。
ZA_MAX_* を上書きして「もしこの値だったら」も出せる。

使い方:
  python3 tools/jit_frame_breakdown.py                 # 現状の内訳
  python3 tools/jit_frame_breakdown.py LOCALS=8        # 上書きして試算
  python3 tools/jit_frame_breakdown.py DEPTH=8 LOCALS=8
"""

import os
import re
import subprocess
import sys
import tempfile

ZA = "src/c/za.c"

# フレームレイアウトに関わる定数。これ以外は切り出さない
WANTED = """ZA_MAX_OPERANDS ZA_MAX_LET_DEPTH ZA_MAX_LOCALS_PER_LET ZA_MAX_FLET_BINDINGS
ZA_MAX_NLX_DEPTH ZA_NLX_SLOT_SIZE ZA_ARG_SLOT_SIZE ZA_LOCAL_SLOT_SIZE
ZA_MAX_CALL_DEPTH ZA_CALL_SLOT_SIZE ZA_MAX_ARITH_DEPTH ZA_ARITH_SLOT_SIZE
ZA_MAX_QQ_DEPTH ZA_MAX_QQ_ELEMENTS ZA_QQ_SLOT_SIZE ZA_QQ_LEVEL_SIZE
ZA_FLET_OLD_SLOT_SIZE ZA_MAX_FIXED_ENTRY_PARAMS
ZA_FRAME_EXTRA ZA_FRAME_TOTAL""".split()

# 領域の境界として並べるオフセット(宣言順ではなく値順に並べ直して差を取る)
OFFSETS = """ZA_OFF_ENV_SAVED_HEAD ZA_OFF_ENV_VAL ZA_OFF_ENV_NODE ZA_OFF_ARGS_VAL
ZA_OFF_ARGS_NODE ZA_OFF_FN_VAL ZA_OFF_FN_NODE ZA_OFF_ACC_VAL ZA_OFF_ACC_NODE
ZA_OFF_LAMBDA_SAVED_HEAD ZA_OFF_LAMBDA_ENV_VAL ZA_OFF_LAMBDA_ENV_NODE
ZA_OFF_LAMBDA_TMP_VAL ZA_OFF_LAMBDA_TMP_NODE ZA_OFF_NLX_BASE ZA_OFF_LOCAL_BASE
ZA_OFF_LET_SAVED_HEAD_BASE ZA_OFF_CALL_BASE ZA_OFF_ARITH_BASE ZA_OFF_FLET_BASE
ZA_OFF_FLET_OLD_BASE ZA_OFF_QQ_BASE ZA_OFF_SETQ_TMP_VAL ZA_OFF_SETQ_TMP_END
ZA_OFF_PARAM_BASE ZA_OFF_SAVED_R14""".split()


def extract_defines():
    """za.c から目的の #define を行継続・行内コメントごと取り出す。"""
    lines = open(ZA, encoding="utf-8").read().split("\n")
    names = set(WANTED) | set(OFFSETS)
    out = []
    for i, ln in enumerate(lines):
        m = re.match(r"#define\s+(\w+)\b", ln)
        if not m or m.group(1) not in names:
            continue
        buf, j = ln, i
        while buf.rstrip().endswith("\\"):
            j += 1
            buf = buf.rstrip()[:-1] + " " + lines[j]
        buf = re.sub(r"/\*.*?\*/", "", buf, flags=re.S)
        k = buf.find("/*")          # 閉じていないコメントの始まり
        if k >= 0:
            buf = buf[:k]
        out.append((m.group(1), buf.rstrip()))
    got = {n for n, _ in out}
    missing = names - got
    if missing:
        sys.exit(f"ERROR: za.c から取り出せなかった定数: {sorted(missing)}")
    return [t for _, t in out]


def main():
    overrides = {}
    for a in sys.argv[1:]:
        k, _, v = a.partition("=")
        key = {"DEPTH": "ZA_MAX_LET_DEPTH", "LOCALS": "ZA_MAX_LOCALS_PER_LET",
               "NLX": "ZA_MAX_NLX_DEPTH", "CALL": "ZA_MAX_CALL_DEPTH",
               "ARITH": "ZA_MAX_ARITH_DEPTH", "QQ": "ZA_MAX_QQ_DEPTH",
               "FLET": "ZA_MAX_FLET_BINDINGS"}.get(k, k)
        overrides[key] = int(v)

    defs = extract_defines()
    if overrides:
        patched = []
        for d in defs:
            name = re.match(r"#define\s+(\w+)", d).group(1)
            patched.append(f"#define {name} {overrides[name]}" if name in overrides else d)
        defs = patched

    rows = "\n".join(
        f'    {{ "{n}", (int)({n}) }},' for n in OFFSETS)
    src = f"""#include <stdio.h>
#include <stdlib.h>
{chr(10).join(defs)}
typedef struct {{ const char *name; int off; }} ent_t;
static int cmp(const void *a, const void *b) {{
    int d = ((const ent_t *)a)->off - ((const ent_t *)b)->off;
    return d ? d : 0;
}}
int main(void) {{
    ent_t e[] = {{
{rows}
    }};
    int n = (int)(sizeof(e)/sizeof(e[0]));
    qsort(e, (size_t)n, sizeof(e[0]), cmp);
    printf("SHADOW\\t0\\t40\\t0x28(既存のシャドウスペース)\\n");
    for (int i = 0; i < n; i++) {{
        int end = (i + 1 < n) ? e[i+1].off : ZA_FRAME_EXTRA;
        if (e[i].off == end) continue;          /* 同じオフセットの別名は畳む */
        printf("%s\\t%d\\t%d\\t\\n", e[i].name, e[i].off, end - e[i].off);
    }}
    printf("TOTALS\\t%d\\t%d\\t%d\\n", ZA_FRAME_EXTRA, ZA_FRAME_TOTAL,
           262144 / ZA_FRAME_TOTAL);
    printf("CONSTS\\t%d\\t%d\\t%d\\t%d\\t%d\\t%d\\t%d\\n",
           ZA_MAX_LET_DEPTH, ZA_MAX_LOCALS_PER_LET, ZA_MAX_NLX_DEPTH,
           ZA_MAX_CALL_DEPTH, ZA_MAX_ARITH_DEPTH, ZA_MAX_QQ_DEPTH,
           ZA_MAX_FLET_BINDINGS);
    return 0;
}}
"""
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "f.c")
        open(c, "w", encoding="utf-8").write(src)
        exe = os.path.join(d, "f")
        r = subprocess.run(["gcc", "-O0", "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("ERROR: 生成した C がコンパイルできない\n" + r.stderr[:2000])
        out = subprocess.run([exe], capture_output=True, text=True).stdout

    body, totals, consts = [], None, None
    for ln in out.strip().split("\n"):
        f = ln.split("\t")
        if f[0] == "TOTALS":
            totals = [int(x) for x in f[1:]]
        elif f[0] == "CONSTS":
            consts = [int(x) for x in f[1:]]
        else:
            body.append((f[0], int(f[1]), int(f[2])))

    print(f"ZA_MAX_LET_DEPTH={consts[0]}  ZA_MAX_LOCALS_PER_LET={consts[1]}  "
          f"ZA_MAX_NLX_DEPTH={consts[2]}  ZA_MAX_CALL_DEPTH={consts[3]}  "
          f"ZA_MAX_ARITH_DEPTH={consts[4]}  ZA_MAX_QQ_DEPTH={consts[5]}  "
          f"ZA_MAX_FLET_BINDINGS={consts[6]}")
    print()
    print("| 領域 | 開始 | 大きさ |")
    print("|---|---:|---:|")
    s = 0
    for name, off, size in body:
        print(f"| `{name}` | {off} | {size} |")
        s += size
    print(f"| **合計** | | **{s}** |")
    print()
    print(f"ZA_FRAME_EXTRA = {totals[0]} / **ZA_FRAME_TOTAL = {totals[1]}** "
          f"/ 256KB で {totals[2]} 段")
    # **足し合わせが合うことを確かめる。**合わなければ数えていない領域がある
    if s != totals[1]:
        sys.exit(f"ERROR: 内訳の合計 {s} が ZA_FRAME_TOTAL {totals[1]} と一致しない。"
                 f"数えていない領域がある")
    print("内訳の合計は ZA_FRAME_TOTAL と一致している。")


if __name__ == "__main__":
    main()
