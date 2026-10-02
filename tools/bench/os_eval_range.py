#!/usr/bin/env python3
"""[性能測定] os_eval の**実行時**アドレス範囲を求める。

なぜ必要か(documents/performance-measurement.md「インタプリタ落ちの検出器」節):

%%za-compiled-p が T でも、**本体が全部ネイティブで走るとは限らない。**
反復ごとに os_eval へ落ちていれば、その数字は「JIT の性能」でも
「AOT の性能」でもない。それを外から見るために、TCG プラグインの
start=/end= で os_eval 区間の実行命令数(range_insns)を数える。

**カーネルには一切手を入れない。** os_eval に数え上げを足すと、
その変更自体が性能を変えてしまう(計器を作る作業で性能を変えない)。

アドレスの出し方:
  UEFI はリンク時とは別の場所へイメージをロードする。しかも
  **アタッチするディスクイメージの構成によっても変わる**
  (tools/plugins/isiki_instcount.c の注意書き)。したがって
  リンク時アドレスはそのままでは使えない。

  実行時アドレス = リンク時アドレス + (実行時アンカー - リンク時アンカー)

  アンカーには %%DIAG-IMAGE-ANCHOR-PUB(os_heap_used_ratio の実行時
  アドレスを返す。GC_DEBUG なしでも使える)を使う。
  差は負になりうるので、符号付きで計算すること
  (符号なしで引いて巨大な値に化けた実例が disasm_symtab_lookup.c にある)。

使い方:
  python3 tools/bench/os_eval_range.py <nmファイル> <実行時アンカー(10進)>
    -> "0xSTART 0xEND" を標準出力へ
"""

import re
import sys

ANCHOR_SYM = "os_heap_used_ratio"   # %%DIAG-IMAGE-ANCHOR-PUB が返すシンボル
TARGET_SYM = "os_eval"


def load_syms(path):
    syms = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = re.match(r"^([0-9a-fA-F]{8,16}) (\S) (.+)$", ln.rstrip("\n"))
            if m:
                syms.append((int(m.group(1), 16), m.group(3)))
    syms.sort()
    return syms


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    syms = load_syms(sys.argv[1])
    anchor_runtime = int(sys.argv[2])

    byname = {}
    for a, n in syms:
        byname.setdefault(n, a)
    for need in (ANCHOR_SYM, TARGET_SYM):
        if need not in byname:
            sys.exit(f"ERROR: {sys.argv[1]} に {need} が無い")

    delta = anchor_runtime - byname[ANCHOR_SYM]   # 負になりうる
    start_link = byname[TARGET_SYM]

    # os_eval の終わりは「次の別アドレスのシンボル」。同じアドレスに複数の
    # シンボルが載ることがあるので、アドレスが変わるまで進める
    i = next(k for k, (a, n) in enumerate(syms) if a == start_link and n == TARGET_SYM)
    j = i + 1
    while j < len(syms) and syms[j][0] == start_link:
        j += 1
    if j >= len(syms):
        sys.exit(f"ERROR: {TARGET_SYM} の次のシンボルが無い")
    end_link = syms[j][0]

    print(f"0x{start_link + delta:x} 0x{end_link + delta:x}")
    print(f"# {TARGET_SYM}: link 0x{start_link:x}..0x{end_link:x} "
          f"({end_link - start_link} byte), delta {delta}, "
          f"次のシンボル {syms[j][1]}", file=sys.stderr)


if __name__ == "__main__":
    main()
